// Small text helpers shared by the Linux backend (no OS calls, portable).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nue::lx {

std::string_view trim(std::string_view s);
std::vector<std::string_view> split(std::string_view s, char sep, bool keep_empty = false);
std::vector<std::string_view> split_lines(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string to_lower_ascii(std::string_view s);
bool iequals_ascii(std::string_view a, std::string_view b);
bool contains_ci(std::string_view haystack, std::string_view needle);

// Decimal / hexadecimal ("0x" prefix allowed when base == 0 or 16). Whole string must parse.
bool parse_u64(std::string_view s, uint64_t& out, int base = 10);
bool parse_i64(std::string_view s, int64_t& out);

// Kernel cpulist format: "0-3,8,10-11". Returns false on malformed text.
bool parse_cpulist(std::string_view s, std::vector<int>& out);
std::string format_cpulist(const std::vector<int>& cpus);

// Case folding used for Windows-style file name comparison. ASCII, Latin-1 supplement,
// Latin Extended-A, Greek and Cyrillic basic letters (UTF-8 in, UTF-8 out). Invalid UTF-8
// bytes pass through unchanged.
std::string fold_name(std::string_view utf8);

// Windows-style wildcard match (`*` and `?`), case-insensitive via fold_name. The pattern
// "*.*" matches every name, as FindFirstFile does.
bool wildcard_match(std::string_view pattern, std::string_view name);

}  // namespace nue::lx
