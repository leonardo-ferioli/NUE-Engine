// Configuration store: typed key/value pairs with dotted keys, INI text format, change watchers.
#include "internal/core_internal.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Entry {
    nue_config_type type = NUE_CONFIG_STRING;
    std::string text;           // canonical text; getters convert from it
    bool declared = false;
};

struct Declared {
    nue_config_type type;
    int64_t default_int;
    double default_double;
    std::string default_string;
    double min_value, max_value;
};

struct Watch {
    nue_config_watch_id id;
    std::string prefix;
    nue_config_callback cb;
    void* userdata;
};

struct ConfigState {
    std::mutex mutex;
    std::map<std::string, Entry> entries;   // ordered: INI output is deterministic
    std::map<std::string, Declared> decls;
    std::vector<Watch> watches;
    nue_config_watch_id next_watch = 1;
};

ConfigState& cs() {
    static ConfigState* s = new ConfigState();
    return *s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32);
    return s;
}

bool parse_bool(const std::string& t, bool* out) {
    std::string l = lower(t);
    if (l == "true" || l == "yes" || l == "on" || l == "1") { *out = true; return true; }
    if (l == "false" || l == "no" || l == "off" || l == "0") { *out = false; return true; }
    return false;
}

bool parse_int(const std::string& t, int64_t* out) {
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(t.c_str(), &end, 0);
    if (errno != 0 || *end != 0) return false;
    *out = v;
    return true;
}

bool parse_double(const std::string& t, double* out) {
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(t.c_str(), &end);
    if (errno != 0 || *end != 0 || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

nue_config_type infer_type(const std::string& value, bool quoted) {
    if (quoted) return NUE_CONFIG_STRING;
    bool b;
    int64_t i;
    double d;
    std::string l = lower(value);
    if (l == "true" || l == "false" || l == "yes" || l == "no" || l == "on" || l == "off") return NUE_CONFIG_BOOL;
    if (parse_int(value, &i)) return NUE_CONFIG_INT;
    if (parse_double(value, &d)) return NUE_CONFIG_DOUBLE;
    (void)b;
    return NUE_CONFIG_STRING;
}

std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
        } else out += s[i];
    }
    return out;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

std::string format_double(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    // prefer the shortest text that round-trips
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string t = buf;
    if (t.find_first_of(".eEn") == std::string::npos) t += ".0";   // keeps the type DOUBLE when read back
    return t;
}

struct Pending { std::string key; };

// Applies a change under the lock and records the key for notification. Returns true if the value changed.
bool store_locked(ConfigState& s, const std::string& key, nue_config_type type, const std::string& text) {
    auto it = s.entries.find(key);
    if (it != s.entries.end() && it->second.text == text && it->second.type == type) return false;
    Entry& e = s.entries[key];
    e.type = type;
    e.text = text;
    return true;
}

void notify(const std::vector<std::string>& keys) {
    if (keys.empty()) return;
    std::vector<Watch> watches;
    { ConfigState& s = cs(); std::lock_guard<std::mutex> lock(s.mutex); watches = s.watches; }
    for (const std::string& k : keys)
        for (const Watch& w : watches)
            if (k.compare(0, w.prefix.size(), w.prefix) == 0 && w.cb) w.cb(w.userdata, k.c_str());
}

nue_result set_value(const char* key, nue_config_type type, const std::string& text) {
    if (!key || !*key) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "config key is empty");
    bool changed;
    {
        ConfigState& s = cs();
        std::lock_guard<std::mutex> lock(s.mutex);
        changed = store_locked(s, key, type, text);
    }
    if (changed) notify({key});
    return NUE_OK;
}

// Fetch with conversion; returns false when the key is missing or not convertible.
bool get_text(const char* key, Entry* out) {
    if (!key) return false;
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.entries.find(key);
    if (it == s.entries.end()) return false;
    *out = it->second;
    return true;
}

}  // namespace

namespace nuei {
void config_init() {}
void config_shutdown() {
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.entries.clear();
    s.decls.clear();
    s.watches.clear();
}
}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_config_declare(const nue_config_decl* d) {
    if (!d || !d->key || !*d->key) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_config_declare: key is required");
    Declared decl{d->type, d->default_int, d->default_double, d->default_string ? d->default_string : "", d->min_value, d->max_value};
    std::string text;
    switch (d->type) {
        case NUE_CONFIG_BOOL: text = d->default_int ? "true" : "false"; break;
        case NUE_CONFIG_INT: text = std::to_string(d->default_int); break;
        case NUE_CONFIG_DOUBLE: text = format_double(d->default_double); break;
        case NUE_CONFIG_STRING: text = decl.default_string; break;
        default: return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_config_declare: bad type");
    }
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.decls[d->key] = decl;
    auto it = s.entries.find(d->key);
    if (it == s.entries.end()) { Entry e; e.type = d->type; e.text = text; e.declared = true; s.entries[d->key] = e; }
    else { it->second.declared = true; it->second.type = d->type; }   // a value loaded earlier keeps its text, the type is now authoritative
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_config_has(const char* key) { Entry e; return get_text(key, &e) ? NUE_TRUE : NUE_FALSE; }
NUE_API nue_config_type NUE_CALL nue_config_type_of(const char* key) { Entry e; return get_text(key, &e) ? e.type : NUE_CONFIG_NONE; }

NUE_API nue_bool NUE_CALL nue_config_get_bool(const char* key, nue_bool def) {
    Entry e; bool b; int64_t i; double d;
    if (!get_text(key, &e)) return def;
    if (parse_bool(e.text, &b)) return b ? NUE_TRUE : NUE_FALSE;
    if (parse_int(e.text, &i)) return i != 0;
    if (parse_double(e.text, &d)) return d != 0.0;
    return def;
}

NUE_API int64_t NUE_CALL nue_config_get_int(const char* key, int64_t def) {
    Entry e; bool b; int64_t i; double d;
    if (!get_text(key, &e)) return def;
    int64_t v;
    if (parse_int(e.text, &i)) v = i;
    else if (parse_double(e.text, &d)) v = int64_t(d);
    else if (parse_bool(e.text, &b)) v = b;
    else return def;
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.decls.find(key);
    if (it != s.decls.end() && it->second.min_value < it->second.max_value) v = std::max<int64_t>(int64_t(it->second.min_value), std::min<int64_t>(int64_t(it->second.max_value), v));
    return v;
}

NUE_API double NUE_CALL nue_config_get_double(const char* key, double def) {
    Entry e; bool b; int64_t i; double d;
    if (!get_text(key, &e)) return def;
    double v;
    if (parse_double(e.text, &d)) v = d;
    else if (parse_int(e.text, &i)) v = double(i);
    else if (parse_bool(e.text, &b)) v = b;
    else return def;
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto it = s.decls.find(key);
    if (it != s.decls.end() && it->second.min_value < it->second.max_value) v = std::max(it->second.min_value, std::min(it->second.max_value, v));
    return v;
}

NUE_API nue_result NUE_CALL nue_config_get_string(const char* key, const char* def, char* buffer, size_t capacity, size_t* out_size) {
    Entry e;
    if (!get_text(key, &e)) {
        nuei::copy_out_string(def ? def : "", def ? std::strlen(def) : 0, buffer, capacity, out_size);
        return NUE_ERR_NOT_FOUND;
    }
    return nuei::copy_out_string(e.text, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_config_set_bool(const char* key, nue_bool v) { return set_value(key, NUE_CONFIG_BOOL, v ? "true" : "false"); }
NUE_API nue_result NUE_CALL nue_config_set_int(const char* key, int64_t v) { return set_value(key, NUE_CONFIG_INT, std::to_string(v)); }
NUE_API nue_result NUE_CALL nue_config_set_double(const char* key, double v) { return set_value(key, NUE_CONFIG_DOUBLE, format_double(v)); }
NUE_API nue_result NUE_CALL nue_config_set_string(const char* key, const char* v) { return set_value(key, NUE_CONFIG_STRING, v ? v : ""); }

NUE_API nue_result NUE_CALL nue_config_remove(const char* key) {
    if (!key) return NUE_ERR_INVALID_ARGUMENT;
    bool removed;
    { ConfigState& s = cs(); std::lock_guard<std::mutex> lock(s.mutex); removed = s.entries.erase(key) > 0; }
    if (removed) notify({key});
    return removed ? NUE_OK : NUE_ERR_NOT_FOUND;
}

NUE_API nue_result NUE_CALL nue_config_load_text(const char* text, size_t length) {
    if (!text) return NUE_ERR_INVALID_ARGUMENT;
    std::string all(text, length);
    std::string section;
    std::vector<std::string> changed;
    size_t pos = 0;
    int line_no = 0;
    std::vector<std::pair<std::string, std::pair<nue_config_type, std::string>>> parsed;
    while (pos <= all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = trim(all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? all.size() + 1 : nl + 1;
        ++line_no;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t close = line.find(']');
            if (close == std::string::npos) return nuei::fail(NUE_ERR_CORRUPT, "config line %d: unterminated section header", line_no);
            section = trim(line.substr(1, close - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) return nuei::fail(NUE_ERR_CORRUPT, "config line %d: expected key = value", line_no);
        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (key.empty()) return nuei::fail(NUE_ERR_CORRUPT, "config line %d: empty key", line_no);
        bool quoted = false;
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') { value = unescape(value.substr(1, value.size() - 2)); quoted = true; }
        else {
            // trailing comment after whitespace: "value ; comment"
            size_t c = value.find(" ;");
            if (c == std::string::npos) c = value.find(" #");
            if (c != std::string::npos) value = trim(value.substr(0, c));
        }
        std::string full = section.empty() ? key : section + "." + key;
        parsed.push_back({full, {infer_type(value, quoted), value}});
    }
    {
        ConfigState& s = cs();
        std::lock_guard<std::mutex> lock(s.mutex);
        for (auto& p : parsed) {
            nue_config_type t = p.second.first;
            auto d = s.decls.find(p.first);
            if (d != s.decls.end()) t = d->second.type;   // a declared key keeps its declared type
            if (store_locked(s, p.first, t, p.second.second)) changed.push_back(p.first);
        }
    }
    notify(changed);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_config_load(const char* path) {
    if (nuei::require_init() < 0 && !path) return NUE_ERR_NOT_INITIALIZED;
    std::string p = path ? path : nuei::init_info().config_file;
    if (p.empty()) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_config_load: no path and no config_file in the init descriptor");
    void* data = nullptr;
    size_t size = 0;
    nue_result r = nue_file_load(p.c_str(), &data, &size);
    if (NUE_FAILED(r)) return r;
    r = nue_config_load_text(static_cast<const char*>(data), size);
    nue_free(data);
    return r;
}

NUE_API nue_result NUE_CALL nue_config_save(const char* path) {
    std::string p = path ? path : nuei::init_info().config_file;
    if (p.empty()) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_config_save: no path and no config_file in the init descriptor");
    std::string out = "; NUE settings\n";
    {
        ConfigState& s = cs();
        std::lock_guard<std::mutex> lock(s.mutex);
        std::string section;
        bool first_global = true;
        // keys without a dot first (global), then sections in key order
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& kv : s.entries) {
                size_t dot = kv.first.find('.');
                bool global = dot == std::string::npos;
                if ((pass == 0) != global) continue;
                std::string sec = global ? "" : kv.first.substr(0, dot);
                std::string key = global ? kv.first : kv.first.substr(dot + 1);
                if (!global && sec != section) { out += "\n[" + sec + "]\n"; section = sec; }
                (void)first_global;
                const Entry& e = kv.second;
                out += key + " = ";
                if (e.type == NUE_CONFIG_STRING) {
                    // quote when it could be re-read as another type or has special characters
                    bool needs = e.text.empty() || infer_type(e.text, false) != NUE_CONFIG_STRING || e.text.find_first_of("\n\t\r\\\";#") != std::string::npos ||
                                 e.text.front() == ' ' || e.text.back() == ' ';
                    out += needs ? "\"" + escape(e.text) + "\"" : e.text;
                } else out += e.text;
                out += "\n";
            }
        }
    }
    return nue_file_save(p.c_str(), out.data(), out.size());
}

NUE_API nue_result NUE_CALL nue_config_watch(const char* prefix, nue_config_callback cb, void* userdata, nue_config_watch_id* out_id) {
    if (!cb) return NUE_ERR_INVALID_ARGUMENT;
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    Watch w{s.next_watch++, prefix ? prefix : "", cb, userdata};
    if (out_id) *out_id = w.id;
    s.watches.push_back(w);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_config_unwatch(nue_config_watch_id id) {
    ConfigState& s = cs();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (size_t i = 0; i < s.watches.size(); ++i)
        if (s.watches[i].id == id) { s.watches.erase(s.watches.begin() + long(i)); return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

}  // extern "C"
