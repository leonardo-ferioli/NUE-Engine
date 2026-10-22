#include "lx_text.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace nue::lx {

std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string_view> split(std::string_view s, char sep, bool keep_empty) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) pos = s.size();
        std::string_view part = s.substr(start, pos - start);
        if (keep_empty || !part.empty()) out.push_back(part);
        start = pos + 1;
    }
    return out;
}

std::vector<std::string_view> split_lines(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t pos = s.find('\n', start);
        if (pos == std::string_view::npos) pos = s.size();
        std::string_view line = s.substr(start, pos - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.push_back(line);
        start = pos + 1;
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

bool contains_ci(std::string_view h, std::string_view n) {
    return to_lower_ascii(h).find(to_lower_ascii(n)) != std::string::npos;
}

bool parse_u64(std::string_view s, uint64_t& out, int base) {
    s = trim(s);
    if (s.empty()) return false;
    if ((base == 0 || base == 16) && s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    } else if (base == 0) {
        base = 10;
    }
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        if (v > (UINT64_MAX - static_cast<uint64_t>(d)) / static_cast<uint64_t>(base)) return false;
        v = v * static_cast<uint64_t>(base) + static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

bool parse_i64(std::string_view s, int64_t& out) {
    s = trim(s);
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    uint64_t u;
    if (!parse_u64(s, u, 10) || u > static_cast<uint64_t>(INT64_MAX) + (neg ? 1u : 0u)) return false;
    out = neg ? static_cast<int64_t>(0 - u) : static_cast<int64_t>(u);
    return true;
}

bool parse_cpulist(std::string_view s, std::vector<int>& out) {
    out.clear();
    s = trim(s);
    if (s.empty()) return true;
    for (std::string_view part : split(s, ',')) {
        part = trim(part);
        size_t dash = part.find('-');
        uint64_t a, b;
        if (dash == std::string_view::npos) {
            if (!parse_u64(part, a)) return false;
            b = a;
        } else {
            if (!parse_u64(part.substr(0, dash), a) || !parse_u64(part.substr(dash + 1), b) || b < a) return false;
        }
        if (b > 65535) return false;
        for (uint64_t i = a; i <= b; ++i) out.push_back(static_cast<int>(i));
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

std::string format_cpulist(const std::vector<int>& in) {
    std::vector<int> cpus = in;
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    std::string out;
    for (size_t i = 0; i < cpus.size();) {
        size_t j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
        if (!out.empty()) out += ',';
        out += std::to_string(cpus[i]);
        if (j > i) out += "-" + std::to_string(cpus[j]);
        i = j + 1;
    }
    return out;
}

namespace {

// Fold one code point; returns the folded code point.
uint32_t fold_cp(uint32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c >= 0x100 && c <= 0x137) return (c & 1) ? c : c + 1;
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c + 1 : c;
    if (c >= 0x14A && c <= 0x177) return (c & 1) ? c : c + 1;
    if (c == 0x178) return 0xFF;
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

}  // namespace

std::string fold_name(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            out += static_cast<char>((b >= 'A' && b <= 'Z') ? b + 32 : b);
            ++i;
        } else if ((b & 0xE0) == 0xC0 && i + 1 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
            uint32_t cp = ((b & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            cp = fold_cp(cp);
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
            i += 2;
        } else {
            out += s[i++];
        }
    }
    return out;
}

namespace {

bool wild(const char* p, const char* pe, const char* n, const char* ne) {
    while (p < pe) {
        if (*p == '*') {
            while (p < pe && *p == '*') ++p;
            if (p == pe) return true;
            for (const char* t = n; t <= ne; ++t)
                if (wild(p, pe, t, ne)) return true;
            return false;
        }
        if (n == ne) return false;
        if (*p != '?' && *p != *n) return false;
        ++p;
        ++n;
    }
    return n == ne;
}

}  // namespace

bool wildcard_match(std::string_view pattern, std::string_view name) {
    if (pattern == "*.*") return true;
    std::string p = fold_name(pattern), n = fold_name(name);
    return wild(p.data(), p.data() + p.size(), n.data(), n.data() + n.size());
}

}  // namespace nue::lx
