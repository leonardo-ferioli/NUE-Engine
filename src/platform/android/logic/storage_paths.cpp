#include "storage_paths.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <unordered_set>

namespace nue::android {

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

SafePath sanitize_relative_path(std::string_view in, NameCase name_case) {
    SafePath r;
    if (in.empty()) {
        r.status = PathStatus::Empty;
        return r;
    }
    if (in.size() > 4096) {
        r.status = PathStatus::TooLong;
        return r;
    }
    // "C:\x" / "C:/x" and "\\server\share" are PC absolute paths.
    if (in.size() >= 2 && in[1] == ':' && ((in[0] >= 'A' && in[0] <= 'Z') || (in[0] >= 'a' && in[0] <= 'z'))) {
        r.status = PathStatus::Absolute;
        return r;
    }
    if (in.front() == '/' || in.front() == '\\') {
        r.status = PathStatus::Absolute;
        return r;
    }
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    bool any = false;
    while (i <= in.size()) {
        size_t j = i;
        while (j < in.size() && in[j] != '/' && in[j] != '\\') ++j;
        std::string_view comp = in.substr(i, j - i);
        i = j + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") {
            r.status = PathStatus::Traversal;
            return r;
        }
        if (comp.size() > 255) {
            r.status = PathStatus::TooLong;
            return r;
        }
        for (char c : comp) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || c == ':' || c == '*' || c == '?' || c == '"' ||
                c == '<' || c == '>' || c == '|') {
                r.status = PathStatus::BadChar;
                return r;
            }
        }
        if (any) out.push_back('/');
        out.append(comp);
        any = true;
    }
    if (!any) {
        r.status = PathStatus::Empty;
        return r;
    }
    if (name_case == NameCase::Lower) out = to_lower_ascii(out);
    r.status = PathStatus::Ok;
    r.path = std::move(out);
    return r;
}

std::string join_path(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    std::string out(a);
    if (out.back() != '/') out.push_back('/');
    out.append(b.front() == '/' ? b.substr(1) : b);
    return out;
}

std::string parent_path(std::string_view p) {
    size_t pos = p.rfind('/');
    if (pos == std::string_view::npos) return std::string();
    return std::string(p.substr(0, pos));
}

uint64_t Manifest::required_bytes() const {
    uint64_t sum = 0;
    for (const ManifestEntry& e : entries)
        if (!e.optional) sum += e.size;
    return sum;
}

const ManifestEntry* Manifest::find(std::string_view sanitised_path) const {
    for (const ManifestEntry& e : entries)
        if (e.path == sanitised_path) return &e;
    return nullptr;
}

namespace {

std::string_view trim(std::string_view s) {
    auto is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

// Takes the next space separated token off `line`.
std::string_view take_token(std::string_view& line) {
    line = trim(line);
    size_t n = 0;
    while (n < line.size() && line[n] != ' ' && line[n] != '\t' && line[n] != '\r' && line[n] != '\n') ++n;
    std::string_view tok = line.substr(0, n);
    line.remove_prefix(n);
    return tok;
}

bool parse_u64(std::string_view s, uint64_t* out) {
    if (s.empty()) return false;
    uint64_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 10);
    if (ec != std::errc() || p != s.data() + s.size()) return false;
    *out = v;
    return true;
}

bool parse_hex32(std::string_view s, uint32_t* out) {
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc() || p != s.data() + s.size()) return false;
    *out = v;
    return true;
}

} // namespace

Manifest parse_manifest(std::string_view text, NameCase name_case) {
    Manifest m;
    std::unordered_set<std::string> seen;
    size_t line_no = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        ++line_no;
        if (line.empty() || line.front() == '#') continue;
        auto fail = [&](const char* why) {
            m.errors.push_back("line " + std::to_string(line_no) + ": " + why);
        };
        ManifestEntry e;
        if (line.front() == '?') {
            e.optional = true;
            line.remove_prefix(1);
        }
        std::string_view size_tok = take_token(line);
        std::string_view crc_tok = take_token(line);
        std::string_view path_tok = trim(line);
        if (!parse_u64(size_tok, &e.size)) {
            fail("bad size");
            continue;
        }
        if (crc_tok == "-") {
            e.has_crc = false;
        } else if (parse_hex32(crc_tok, &e.crc)) {
            e.has_crc = true;
        } else {
            fail("bad crc");
            continue;
        }
        SafePath sp = sanitize_relative_path(path_tok, name_case);
        if (sp.status != PathStatus::Ok) {
            fail("unsafe path");
            continue;
        }
        if (!seen.insert(sp.path).second) {
            fail("duplicate path");
            continue;
        }
        e.path = std::move(sp.path);
        m.entries.push_back(std::move(e));
    }
    return m;
}

std::string format_manifest_line(const ManifestEntry& e) {
    char buf[64];
    if (e.has_crc)
        std::snprintf(buf, sizeof buf, "%llu %08x ", static_cast<unsigned long long>(e.size), e.crc);
    else
        std::snprintf(buf, sizeof buf, "%llu - ", static_cast<unsigned long long>(e.size));
    return std::string(e.optional ? "? " : "") + buf + e.path;
}

SpaceReport check_free_space(uint64_t bytes_to_copy, uint64_t free_bytes) {
    constexpr uint64_t kMinMargin = 64ull << 20;
    uint64_t margin = std::max<uint64_t>(kMinMargin, bytes_to_copy / 50);
    SpaceReport r;
    r.needed = bytes_to_copy + margin;
    r.available = free_bytes;
    if (free_bytes >= r.needed) {
        r.result = SpaceCheck::Ok;
    } else {
        r.result = SpaceCheck::NotEnough;
        r.shortfall = r.needed - free_bytes;
    }
    return r;
}

ImportLayout ImportLayout::for_root(std::string_view root) {
    ImportLayout l;
    l.root = std::string(root);
    while (l.root.size() > 1 && l.root.back() == '/') l.root.pop_back();
    l.staging = join_path(l.root, ".import");
    l.journal = join_path(l.staging, "journal.txt");
    l.complete = join_path(l.staging, "complete.txt");
    return l;
}

std::string make_complete_marker(uint32_t version, uint32_t files, uint64_t bytes) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "nue-import-complete %u %u %llu\n", version, files,
                  static_cast<unsigned long long>(bytes));
    return buf;
}

bool parse_complete_marker(std::string_view text, uint32_t* version, uint32_t* files, uint64_t* bytes) {
    constexpr std::string_view kHead = "nue-import-complete";
    text = trim(text);
    if (text.substr(0, kHead.size()) != kHead) return false;
    text.remove_prefix(kHead.size());
    uint64_t v = 0, f = 0, b = 0;
    if (!parse_u64(take_token(text), &v) || !parse_u64(take_token(text), &f) || !parse_u64(take_token(text), &b))
        return false;
    if (!trim(text).empty()) return false;
    if (version) *version = static_cast<uint32_t>(v);
    if (files) *files = static_cast<uint32_t>(f);
    if (bytes) *bytes = b;
    return true;
}

std::string format_journal_line(const JournalEntry& e) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%llu %08x ", static_cast<unsigned long long>(e.size), e.crc);
    return buf + e.path + "\n";
}

std::vector<JournalEntry> parse_journal(std::string_view text) {
    std::vector<JournalEntry> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) break; // torn last line: ignore, the file will be copied again
        std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.empty()) continue;
        JournalEntry e;
        std::string_view size_tok = take_token(line);
        std::string_view crc_tok = take_token(line);
        std::string_view path_tok = trim(line);
        if (!parse_u64(size_tok, &e.size) || !parse_hex32(crc_tok, &e.crc) || path_tok.empty()) continue;
        e.path = std::string(path_tok);
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace nue::android
