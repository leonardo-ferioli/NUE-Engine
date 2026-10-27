// Text and locale: code pages (MultiByteToWideChar / WideCharToMultiByte), character classes (GetStringType), comparison and mapping
// (CompareString, LCMapString, lstrcmpi), locale data (GetLocaleInfo, IsValidLocale, EnumSystemLocales) and wsprintf formatting. Pure functions plus the
// two code pages the process runs with (ACP, OEMCP). Unicode knowledge is the Latin, Greek and Cyrillic repertoire a Western game meets; everything
// else is classified as "defined, not a letter" (documented partial coverage, tested against Windows for the covered ranges).
#pragma once
#include "nue_legacy/win32/common.hpp"
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::win32 {

namespace cp { enum : uint32_t { acp = 0, oem = 1, mac = 2, thread_acp = 3, latin1 = 28591, ascii = 20127, oem437 = 437, oem850 = 850, ansi1252 = 1252, utf7 = 65000, utf8 = 65001 }; }

struct CodePageInfo {                    // CPINFO
    uint32_t max_char_size = 1;
    uint8_t default_char[2] = {'?', 0};
    uint8_t lead_byte[12] = {};
};

namespace lcid { enum : uint32_t { en_us = 0x0409, invariant = 0x007F, user_default = 0x0400, system_default = 0x0800, neutral = 0 }; }

namespace cmp { enum : uint32_t { less = 1, equal = 2, greater = 3 }; }   // CSTR_*

class TextApi {
public:
    // The process code pages (GetACP / GetOEMCP). The defaults describe a Western Windows: 1252 and 437.
    explicit TextApi(uint32_t ansi_code_page = cp::ansi1252, uint32_t oem_code_page = cp::oem437, uint32_t user_lcid = 0x0409) : acp_(ansi_code_page), oemcp_(oem_code_page), user_lcid_(user_lcid) {}
    uint32_t get_acp() const { return acp_; }
    uint32_t get_oemcp() const { return oemcp_; }
    bool is_valid_code_page(uint32_t code_page) const;
    Ret<CodePageInfo> get_cp_info(uint32_t code_page) const;

    // MultiByteToWideChar / WideCharToMultiByte. `source_count` -1 means NUL-terminated and the terminator is converted too. With capacity 0 the
    // result is the number of units needed and nothing is written. On failure the Ret has the Win32 error and value 0.
    Ret<int> multi_byte_to_wide(uint32_t code_page, uint32_t flags, const char* source, int source_count, char16_t* out, int out_capacity) const;
    Ret<int> wide_to_multi_byte(uint32_t code_page, uint32_t flags, const char16_t* source, int source_count, char* out, int out_capacity, const char* default_char, bool* used_default) const;
    // Convenience for the library and the tests: whole strings, no terminator.
    std::u16string to_wide(uint32_t code_page, std::string_view s) const;
    std::string to_multi_byte(uint32_t code_page, std::u16string_view s) const;

    // GetStringTypeW/A: `type` is CT_CTYPE1 (1), CT_CTYPE2 (2) or CT_CTYPE3 (4).
    Ret<bool> get_string_type_w(uint32_t type, const char16_t* source, int count, uint16_t* out) const;
    Ret<bool> get_string_type_a(uint32_t locale, uint32_t type, const char* source, int count, uint16_t* out) const;

    // CompareStringW/A and lstrcmpiA. Result is CSTR_LESS_THAN/EQUAL/GREATER_THAN, 0 on error.
    Ret<int> compare_string_w(uint32_t locale, uint32_t flags, const char16_t* a, int a_count, const char16_t* b, int b_count) const;
    Ret<int> compare_string_a(uint32_t locale, uint32_t flags, const char* a, int a_count, const char* b, int b_count) const;
    int lstrcmpi_a(const char* a, const char* b) const;      // <0, 0, >0 like the original

    // LCMapStringW/A: LCMAP_LOWERCASE (0x100), LCMAP_UPPERCASE (0x200), LCMAP_SORTKEY (0x400), LCMAP_BYTEREV (0x800).
    Ret<int> lc_map_string_w(uint32_t locale, uint32_t flags, const char16_t* source, int source_count, char16_t* out, int out_capacity) const;
    Ret<int> lc_map_string_a(uint32_t locale, uint32_t flags, const char* source, int source_count, char* out, int out_capacity) const;

    // GetLocaleInfoW/A. With LOCALE_RETURN_NUMBER (0x20000000) the value is a DWORD written to `out` as two UTF-16 units / four bytes.
    Ret<int> get_locale_info_w(uint32_t locale, uint32_t type, char16_t* out, int out_capacity) const;
    Ret<int> get_locale_info_a(uint32_t locale, uint32_t type, char* out, int out_capacity) const;
    uint32_t get_user_default_lcid() const { return user_lcid_; }   // the locale data below is en-US for every LCID (documented deviation)
    bool is_valid_locale(uint32_t locale, uint32_t flags) const;
    // The locales EnumSystemLocalesA reports (each as an 8-digit hex string, "00000409"); the facade calls the guest callback for each.
    std::vector<std::string> enum_system_locales(uint32_t flags) const;

    // Unicode simple case mapping and classification over the covered repertoire.
    static char32_t to_lower(char32_t c);
    static char32_t to_upper(char32_t c);
    static uint16_t ctype1(char32_t c);   // C1_* bits

private:
    uint32_t acp_, oemcp_, user_lcid_;
};

// wsprintfA / wvsprintfA: the user32 formatter (no floating point, output limited to 1024 characters, flags - + 0 space #, width, precision,
// length l/h/I64... and types c C d i u x X s S p). `next_arg` returns the next 32-bit argument word (the facade reads the guest stack);
// `read_string` turns a string argument (a guest pointer) into text, `read_wide_string` the same for %S / %ls. Returns the length written.
struct FormatArgs {
    std::function<uint32_t()> next_arg;
    std::function<std::string(uint32_t)> read_string;
    std::function<std::u16string(uint32_t)> read_wide_string;
};
std::string wsprintf_format(const TextApi& text, std::string_view format, FormatArgs& args);

}  // namespace nue::legacy::win32
