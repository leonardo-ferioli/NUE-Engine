// Path rules shared by every Storage implementation (platform/storage.hpp): splitting "root:relative/path", normalisation and the case folding
// used for lookup. Pure functions, no file system access.
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::platform::path {

// Folds one UTF-8 string for comparison: ASCII letters and the Latin-1 supplement letters are lower-cased, everything else is kept.
std::string fold(std::string_view text);
bool equal_folded(std::string_view a, std::string_view b);

struct Split {
    std::string root;           // empty for a native path
    std::string rest;           // the part after "root:" (or the whole native path)
    bool native = false;
};
// "data:GLOBAL/X" -> {data, GLOBAL/X}. A single letter before ':' is a drive letter (native); no ':' is native.
Split split_root(std::string_view path);

// '\' -> '/', repeated separators and "." segments dropped, ".." resolved; false when ".." would climb above the start. No leading '/'.
bool normalize_relative(std::string_view relative, std::vector<std::string>& segments);
std::string join(const std::vector<std::string>& segments);

}  // namespace nue::legacy::platform::path
