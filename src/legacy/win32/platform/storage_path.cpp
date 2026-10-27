#include "nue_legacy/platform/storage_path.hpp"

namespace nue::legacy::platform::path {

std::string fold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 'A' && c <= 'Z') out.push_back(char(c + 32));
        else if (c == 0xC3 && i + 1 < text.size()) {   // UTF-8 Latin-1 supplement: U+00C0..U+00DE (except U+00D7) fold to +0x20
            const unsigned char d = static_cast<unsigned char>(text[i + 1]);
            out.push_back(char(c));
            out.push_back((d >= 0x80 && d <= 0x9E && d != 0x97) ? char(d + 0x20) : char(d));
            ++i;
        } else out.push_back(char(c));
    }
    return out;
}

bool equal_folded(std::string_view a, std::string_view b) { return a.size() == b.size() ? fold(a) == fold(b) : fold(a) == fold(b); }

Split split_root(std::string_view p) {
    Split s;
    const size_t colon = p.find(':');
    if (colon == std::string_view::npos || colon < 2) {   // no root, or a drive letter
        s.native = true;
        s.rest = std::string(p);
        return s;
    }
    for (size_t i = 0; i < colon; ++i) {
        const char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            s.native = true;
            s.rest = std::string(p);
            return s;
        }
    }
    s.root = fold(p.substr(0, colon));
    s.rest = std::string(p.substr(colon + 1));
    return s;
}

bool normalize_relative(std::string_view relative, std::vector<std::string>& segments) {
    segments.clear();
    size_t i = 0;
    while (i <= relative.size()) {
        size_t j = i;
        while (j < relative.size() && relative[j] != '/' && relative[j] != '\\') ++j;
        const std::string_view part = relative.substr(i, j - i);
        if (part.empty() || part == ".") {
        } else if (part == "..") {
            if (segments.empty()) return false;
            segments.pop_back();
        } else {
            segments.emplace_back(part);
        }
        i = j + 1;
    }
    return true;
}

std::string join(const std::vector<std::string>& segments) {
    std::string out;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i) out.push_back('/');
        out += segments[i];
    }
    return out;
}

}  // namespace nue::legacy::platform::path
