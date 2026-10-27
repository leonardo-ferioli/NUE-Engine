#include "nue_legacy/win32/registry.hpp"
#include "nue_legacy/win32/handles.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace nue::legacy::win32 {
namespace {

// Hive names as they appear in the persisted keys.
const char* hive_name(Handle h) {
    switch (h) { case hkey::classes_root: return "HKCR"; case hkey::current_user: return "HKCU"; case hkey::local_machine: return "HKLM"; case hkey::users: return "HKU"; default: return nullptr; }
}

std::string hex(const uint8_t* d, uint32_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(size_t(n) * 2);
    for (uint32_t i = 0; i < n; ++i) { s.push_back(digits[d[i] >> 4]); s.push_back(digits[d[i] & 15]); }
    return s;
}
std::vector<uint8_t> unhex(std::string_view s) {
    std::vector<uint8_t> out;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(nibble(s[i]) * 16 + nibble(s[i + 1])));
    return out;
}

// Splits "A\\B/C" into components, dropping empty ones; false for a component that is just ".." (not a registry name).
std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = i;
        while (j < path.size() && path[j] != '\\' && path[j] != '/') ++j;
        if (j > i) out.emplace_back(path.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

struct Value { std::string name; uint32_t type = reg::none; std::vector<uint8_t> data; };
struct Node {
    std::string name;
    std::map<std::string, Value> values;                        // key: folded name
    std::map<std::string, std::unique_ptr<Node>> children;      // key: folded name
};

struct KeyObject : Object {
    KeyObject(std::string h, std::vector<std::string> p) : Object(ObjectKind::registry_key), hive(std::move(h)), path(std::move(p)) {}
    std::string hive;
    std::vector<std::string> path;                              // components with their original case
};

class ConfigRegistry final : public Registry {
public:
    explicit ConfigRegistry(platform::Config& config) : config_(config) { load(); }

    uint32_t open_key(Handle parent, std::string_view sub_key, Handle& out) override {
        std::lock_guard lock(m_);
        std::string hive; std::vector<std::string> path;
        if (!resolve(parent, sub_key, hive, path)) return err::invalid_handle;
        if (!find(hive, path)) return err::file_not_found;
        return new_handle(hive, path, out);
    }
    uint32_t create_key(Handle parent, std::string_view sub_key, Handle& out, uint32_t* disposition) override {
        std::lock_guard lock(m_);
        std::string hive; std::vector<std::string> path;
        if (!resolve(parent, sub_key, hive, path)) return err::invalid_handle;
        bool created = false;
        Node* n = root(hive);
        for (auto& part : path) {
            auto& child = n->children[platform::path::fold(part)];
            if (!child) { child = std::make_unique<Node>(); child->name = part; created = true; }
            n = child.get();
        }
        if (created) persist_key(hive, path);
        if (disposition) *disposition = created ? reg_disposition::created_new_key : reg_disposition::opened_existing_key;
        return new_handle(hive, path, out);
    }
    uint32_t close_key(Handle key) override {
        if (hive_name(key)) return err::success;               // closing a predefined key is accepted and does nothing
        return table_.close(key) ? err::success : err::invalid_handle;
    }
    uint32_t query_value(Handle key, std::string_view name, uint32_t* type, uint8_t* data, uint32_t* size) override {
        std::lock_guard lock(m_);
        Node* n = node_of(key);
        if (!n) return err::invalid_handle;
        auto it = n->values.find(platform::path::fold(name));
        if (it == n->values.end()) { if (type) *type = reg::none; return err::file_not_found; }   // Windows zeroes the type of a value that is not there
        const Value& v = it->second;
        if (type) *type = v.type;
        if (!size) return data ? err::invalid_parameter : err::success;
        const uint32_t needed = uint32_t(v.data.size());
        if (!data) { *size = needed; return err::success; }
        if (*size < needed) { *size = needed; return err::more_data; }
        std::memcpy(data, v.data.data(), needed);
        *size = needed;
        return err::success;
    }
    uint32_t set_value(Handle key, std::string_view name, uint32_t type, const uint8_t* data, uint32_t size) override {
        std::lock_guard lock(m_);
        Node* n = node_of(key);
        if (!n) return err::invalid_handle;
        if (!data && size) return err::invalid_parameter;
        Value v;
        v.name = std::string(name);
        v.type = type;
        if (size) v.data.assign(data, data + size);
        n->values[platform::path::fold(name)] = v;
        persist_value(key, v);
        return err::success;
    }
    uint32_t delete_value(Handle key, std::string_view name) override {
        std::lock_guard lock(m_);
        Node* n = node_of(key);
        if (!n) return err::invalid_handle;
        if (!n->values.erase(platform::path::fold(name))) return err::file_not_found;
        config_.remove(value_config_key(key, name));
        return err::success;
    }
    uint32_t delete_key(Handle parent, std::string_view sub_key) override {
        std::lock_guard lock(m_);
        std::string hive; std::vector<std::string> path;
        if (!resolve(parent, sub_key, hive, path) || path.empty()) return err::invalid_parameter;
        Node* node = find(hive, path);
        if (!node) return err::file_not_found;
        if (!node->children.empty()) return err::access_denied;   // RegDeleteKey fails on a key with subkeys (Windows 2000 and later)
        for (const auto& v : node->values) config_.remove(value_key(hive, path, v.second.name));
        config_.remove(key_key(hive, path));
        std::vector<std::string> parent_path(path.begin(), path.end() - 1);
        find(hive, parent_path)->children.erase(platform::path::fold(path.back()));
        return err::success;
    }
    uint32_t enum_key(Handle key, uint32_t index, std::string& name) override {
        std::lock_guard lock(m_);
        Node* n = node_of(key);
        if (!n) return err::invalid_handle;
        if (index >= n->children.size()) return err::no_more_items;
        auto it = n->children.begin();
        std::advance(it, index);
        name = it->second->name;
        return err::success;
    }
    uint32_t enum_value(Handle key, uint32_t index, std::string& name, uint32_t* type) override {
        std::lock_guard lock(m_);
        Node* n = node_of(key);
        if (!n) return err::invalid_handle;
        if (index >= n->values.size()) return err::no_more_items;
        auto it = n->values.begin();
        std::advance(it, index);
        name = it->second.name;
        if (type) *type = it->second.type;
        return err::success;
    }

private:
    // ---- tree
    Node* root(const std::string& hive) { auto& n = roots_[hive]; if (!n) { n = std::make_unique<Node>(); n->name = hive; } return n.get(); }
    Node* find(const std::string& hive, const std::vector<std::string>& path) {
        Node* n = root(hive);
        for (const auto& part : path) {
            auto it = n->children.find(platform::path::fold(part));
            if (it == n->children.end()) return nullptr;
            n = it->second.get();
        }
        return n;
    }
    // The hive and absolute path that `parent` + `sub_key` denote. false when `parent` is not a key.
    bool resolve(Handle parent, std::string_view sub_key, std::string& hive, std::vector<std::string>& path) {
        if (const char* h = hive_name(parent)) { hive = h; path.clear(); }
        else {
            auto object = table_.get_as<KeyObject>(parent, ObjectKind::registry_key);
            if (!object) return false;
            hive = object->hive; path = object->path;
        }
        for (auto& c : components(sub_key)) path.push_back(std::move(c));
        return true;
    }
    Node* node_of(Handle key) {
        std::string hive; std::vector<std::string> path;
        if (!resolve(key, {}, hive, path)) return nullptr;
        return find(hive, path);
    }
    uint32_t new_handle(const std::string& hive, const std::vector<std::string>& path, Handle& out) {
        out = table_.add(std::make_shared<KeyObject>(hive, path));
        return out ? err::success : err::not_enough_memory;
    }

    // ---- persistence: "registry.k|HIVE|path" = "" for a key, "registry.v|HIVE|path|name" = "type|hex" for a value
    static std::string join(const std::vector<std::string>& path) { std::string s; for (size_t i = 0; i < path.size(); ++i) { if (i) s += '\\'; s += path[i]; } return s; }
    static std::string key_key(const std::string& hive, const std::vector<std::string>& path) { return "registry.k|" + hive + "|" + join(path); }
    static std::string value_key(const std::string& hive, const std::vector<std::string>& path, std::string_view name) { return "registry.v|" + hive + "|" + join(path) + "|" + std::string(name); }
    std::string value_config_key(Handle key, std::string_view name) {
        std::string hive; std::vector<std::string> path;
        resolve(key, {}, hive, path);
        // The persisted name keeps the case it was first written with.
        Node* n = find(hive, path);
        auto it = n->values.find(platform::path::fold(name));
        return value_key(hive, path, it == n->values.end() ? name : std::string_view(it->second.name));
    }
    void persist_key(const std::string& hive, const std::vector<std::string>& path) {
        for (size_t i = 1; i <= path.size(); ++i) config_.set_string(key_key(hive, std::vector<std::string>(path.begin(), path.begin() + i)), "");
    }
    void persist_value(Handle key, const Value& v) {
        std::string hive; std::vector<std::string> path;
        resolve(key, {}, hive, path);
        config_.set_string(value_key(hive, path, v.name), std::to_string(v.type) + "|" + hex(v.data.data(), uint32_t(v.data.size())));
    }
    void load() {
        for (const auto& k : config_.keys_with_prefix("registry.k|")) {
            const auto parts = split(k.substr(11));
            if (parts.size() == 2) { Node* n = root(parts[0]); for (auto& c : components(parts[1])) { auto& ch = n->children[platform::path::fold(c)]; if (!ch) { ch = std::make_unique<Node>(); ch->name = c; } n = ch.get(); } }
        }
        for (const auto& k : config_.keys_with_prefix("registry.v|")) {
            const auto parts = split(k.substr(11));
            std::string text;
            if (parts.size() != 3 || config_.get_string(k, text) != platform::Status::ok) continue;
            Node* n = root(parts[0]);
            for (auto& c : components(parts[1])) { auto& ch = n->children[platform::path::fold(c)]; if (!ch) { ch = std::make_unique<Node>(); ch->name = c; } n = ch.get(); }
            const size_t bar = text.find('|');
            if (bar == std::string::npos) continue;
            Value v;
            v.name = parts[2];
            v.type = uint32_t(std::strtoul(text.substr(0, bar).c_str(), nullptr, 10));
            v.data = unhex(std::string_view(text).substr(bar + 1));
            n->values[platform::path::fold(v.name)] = std::move(v);
        }
    }
    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        size_t i = 0;
        for (;;) { const size_t j = s.find('|', i); out.push_back(s.substr(i, j == std::string::npos ? j : j - i)); if (j == std::string::npos) break; i = j + 1; }
        return out;
    }

    platform::Config& config_;
    std::mutex m_;
    HandleTable table_;
    std::map<std::string, std::unique_ptr<Node>> roots_;
};
}  // namespace

std::unique_ptr<Registry> make_config_registry(platform::Config& config) { return std::make_unique<ConfigRegistry>(config); }

}  // namespace nue::legacy::win32
