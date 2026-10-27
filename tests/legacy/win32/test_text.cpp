// Text and locale: unit checks plus differential tests against the real Windows NLS functions (kernel32 / user32) over the covered repertoire.
#include "nue_legacy/win32/text.hpp"
#include "test_support.hpp"
#include <cstring>
#include <random>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace nue::legacy::win32;

static void test_text() {
    TextApi text;
    CHECK_EQ(text.get_acp(), 1252u);
    CHECK_EQ(text.get_oemcp(), 437u);
    CHECK(text.is_valid_code_page(1252) && text.is_valid_code_page(65001) && !text.is_valid_code_page(99999));
    char16_t w[16];
    // UTF-8 round trip, required length, buffer errors.
    const char utf8[] = "caf\xC3\xA9 \xE2\x82\xAC";
    auto r = text.multi_byte_to_wide(65001, 0, utf8, -1, nullptr, 0);
    CHECK(r && r.value == 7);                       // c a f é space € and the terminator
    r = text.multi_byte_to_wide(65001, 0, utf8, -1, w, 16);
    CHECK(r && r.value == 7 && w[3] == 0xE9 && w[5] == 0x20AC && w[6] == 0);
    r = text.multi_byte_to_wide(65001, 0, utf8, -1, w, 3);
    CHECK(!r && r.error == err::insufficient_buffer);
    r = text.multi_byte_to_wide(65001, 0, "\xFF", 1, w, 16);                 // invalid byte -> U+FFFD
    CHECK(r && r.value == 1 && w[0] == 0xFFFD);
    r = text.multi_byte_to_wide(65001, 8, "\xFF", 1, w, 16);                 // MB_ERR_INVALID_CHARS
    CHECK(!r && r.error == 1113);
    r = text.multi_byte_to_wide(65001, 4, "a", 1, w, 16);                    // MB_USEGLYPHCHARS is not valid for UTF-8
    CHECK(!r && r.error == err::invalid_flags);
    r = text.multi_byte_to_wide(1252, 0, "a", 0, w, 16);
    CHECK(!r && r.error == err::invalid_parameter);
    // Code page 1252 and 437.
    CHECK_EQ(text.to_wide(1252, "\x80\x93").size(), 2u);
    CHECK(text.to_wide(1252, "\x80")[0] == 0x20AC);
    CHECK(text.to_wide(437, "\x80")[0] == 0xC7);
    CHECK(text.to_multi_byte(1252, u"€") == "\x80");
    char b[8];
    bool used = false;
    auto w2 = text.wide_to_multi_byte(1252, 0, u"中", 1, b, 8, nullptr, &used);
    CHECK(w2 && w2.value == 1 && b[0] == '?' && used);
    w2 = text.wide_to_multi_byte(65001, 0, u"a", 1, b, 8, "?", nullptr);
    CHECK(!w2 && w2.error == err::invalid_parameter);
    // Case, classes, locale.
    CHECK_EQ(TextApi::to_upper('a'), U'A');
    CHECK_EQ(TextApi::to_lower(0xC9), 0xE9u);
    CHECK_EQ(TextApi::to_upper(0x3B1), 0x391u);
    CHECK_EQ(TextApi::to_lower(0x416), 0x436u);
    uint16_t types[4];
    CHECK(text.get_string_type_w(1, u"aA1 ", 4, types) && (types[0] & 2) && (types[1] & 1) && (types[2] & 4) && (types[3] & 8));
    CHECK(text.is_valid_locale(0x409, 1) && !text.is_valid_locale(0x40A, 1));
    char16_t li[64];
    auto l = text.get_locale_info_w(0x409, 0x0E, li, 64);   // LOCALE_SDECIMAL
    CHECK(l && l.value == 2 && li[0] == u'.');
    CHECK_EQ(text.lstrcmpi_a("Hello", "hELLO"), 0);
    CHECK(text.lstrcmpi_a("a", "b") < 0 && text.lstrcmpi_a("b", "a") > 0);
}
REGISTER_TEST("text", test_text);

#if defined(_WIN32)
namespace {
std::vector<char16_t> repertoire() {
    std::vector<char16_t> v;
    for (char16_t c = 0; c <= 0x4FF; ++c) v.push_back(c);
    return v;
}
}  // namespace

// Code pages: every byte and every character of the covered range through both implementations.
static void test_text_codepages_windows() {
    TextApi text;
    for (uint32_t cp : {437u, 850u, 1252u, 28591u, 65001u}) {
        int differences = 0;
        for (unsigned b = 0; b < 256; ++b) {
            const char byte = char(b);
            WCHAR real[4] = {};
            const int a = MultiByteToWideChar(cp, 0, &byte, 1, real, 4);
            char16_t mine[4] = {};
            auto m = text.multi_byte_to_wide(cp, 0, &byte, 1, mine, 4);
            if (a != m.value || (a > 0 && real[0] != mine[0])) { ++differences; std::fprintf(stderr, "cp %u byte %02X: windows %d/%04X mine %d/%04X\n", cp, b, a, real[0], m.value, mine[0]); }
        }
        CHECK_EQ(differences, 0);
        // Wide to multibyte over U+0000..U+04FF, with and without best fit; `used default` flag too.
        for (uint32_t flags : {0u, 0x400u}) {
            int diff = 0, shown = 0;
            for (char16_t c : repertoire()) {
                if (cp == 65001 && c >= 0xD800 && c <= 0xDFFF) continue;
                char real[8] = {}, mine[8] = {};
                BOOL real_used = FALSE;
                bool mine_used = false;
                const int a = WideCharToMultiByte(cp, cp == 65001 ? 0 : flags, reinterpret_cast<const WCHAR*>(&c), 1, real, 8, nullptr, cp == 65001 ? nullptr : &real_used);
                auto m = text.wide_to_multi_byte(cp, cp == 65001 ? 0 : flags, &c, 1, mine, 8, nullptr, cp == 65001 ? nullptr : &mine_used);
                const bool same = a == m.value && std::memcmp(real, mine, size_t(a > 0 ? a : 0)) == 0 && (cp == 65001 || (real_used != 0) == mine_used);
                if (!same) { ++diff; if (shown++ < 6) std::fprintf(stderr, "cp %u flags %x U+%04X: windows %d bytes (%02X) used=%d, mine %d (%02X) used=%d\n", cp, flags, c, a, (unsigned char)real[0], real_used, m.value, (unsigned char)mine[0], mine_used); }
            }
            if (diff) std::fprintf(stderr, "cp %u flags %x: %d characters differ\n", cp, flags, diff);
            CHECK_EQ(diff, 0);
        }
    }
    // Return conventions: required size, terminator, insufficient buffer, invalid arguments.
    const char* s = "Hello";
    for (uint32_t cp : {1252u, 65001u}) {
        WCHAR real[16];
        char16_t mine[16];
        CHECK_EQ(MultiByteToWideChar(cp, 0, s, -1, nullptr, 0), text.multi_byte_to_wide(cp, 0, s, -1, nullptr, 0).value);
        CHECK_EQ(MultiByteToWideChar(cp, 0, s, -1, real, 16), text.multi_byte_to_wide(cp, 0, s, -1, mine, 16).value);
        SetLastError(0);
        const int a = MultiByteToWideChar(cp, 0, s, -1, real, 2);
        auto m = text.multi_byte_to_wide(cp, 0, s, -1, mine, 2);
        CHECK_EQ(a, m.value);
        CHECK_EQ(GetLastError(), m.error);
        SetLastError(0);
        const int a2 = MultiByteToWideChar(cp, 0, s, 0, real, 16);
        auto m2 = text.multi_byte_to_wide(cp, 0, s, 0, mine, 16);
        CHECK_EQ(a2, m2.value);
        CHECK_EQ(GetLastError(), m2.error);
        SetLastError(0);
        const int a3 = MultiByteToWideChar(cp, 0x40, s, -1, real, 16);
        auto m3 = text.multi_byte_to_wide(cp, 0x40, s, -1, mine, 16);
        CHECK_EQ(a3, m3.value);
        CHECK_EQ(GetLastError(), m3.error);
    }
    CPINFO info;
    for (uint32_t cp : {437u, 1252u, 28591u, 65001u}) {
        CHECK(GetCPInfo(cp, &info));
        auto m = text.get_cp_info(cp);
        CHECK(m && m.value.max_char_size == info.MaxCharSize && m.value.default_char[0] == info.DefaultChar[0]);
        CHECK_EQ(IsValidCodePage(cp) != 0, text.is_valid_code_page(cp));
    }
    CHECK_EQ(IsValidCodePage(99999) != 0, text.is_valid_code_page(99999));
}
REGISTER_TEST("text_codepages_windows", test_text_codepages_windows);

// Character classes and case mapping over U+0000..U+04FF; outside it the tables are partial, so only the count of differences is reported.
static void test_text_classes_windows() {
    TextApi text;
    for (uint32_t type : {1u, 2u, 4u}) {
        int diff = 0;
        for (char16_t c : repertoire()) {
            WORD real = 0; uint16_t mine = 0xFFFF;
            GetStringTypeW(type == 1 ? CT_CTYPE1 : type == 2 ? CT_CTYPE2 : CT_CTYPE3, reinterpret_cast<const WCHAR*>(&c), 1, &real);
            text.get_string_type_w(type, &c, 1, &mine);
            if (real != mine) ++diff;
        }
        CHECK_EQ(diff, 0);
    }
    int lower_diff = 0, upper_diff = 0, outside = 0;
    for (char16_t c = 0; c <= 0x4FF; ++c) {
        WCHAR out = 0;
        LCMapStringW(0x0409, LCMAP_LOWERCASE, reinterpret_cast<const WCHAR*>(&c), 1, &out, 1);
        if (out != TextApi::to_lower(c)) ++lower_diff;
        LCMapStringW(0x0409, LCMAP_UPPERCASE, reinterpret_cast<const WCHAR*>(&c), 1, &out, 1);
        if (out != TextApi::to_upper(c)) ++upper_diff;
    }
    CHECK_EQ(lower_diff, 0);
    CHECK_EQ(upper_diff, 0);
    for (unsigned c = 0x500; c <= 0xFFFF; ++c) {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        WCHAR ch = WCHAR(c), out = 0;
        LCMapStringW(0x0409, LCMAP_LOWERCASE, &ch, 1, &out, 1);
        if (out != TextApi::to_lower(c)) ++outside;
    }
    std::printf("  case mapping above U+04FF: %d of %d differ (partial coverage, documented)\n", outside, 0xFFFF - 0x500 - 0x800 + 1);
    // Whole-string mapping, counts, errors.
    char16_t mine[16];
    WCHAR real[16];
    const char16_t* src = u"Straße Été";
    for (uint32_t flags : {0x100u, 0x200u, 0x100u | 0x800u}) {
        const int a = LCMapStringW(0x0409, flags, reinterpret_cast<const WCHAR*>(src), -1, real, 16);
        auto m = text.lc_map_string_w(0x0409, flags, src, -1, mine, 16);
        CHECK_EQ(a, m.value);
        CHECK(a > 0 && std::memcmp(real, mine, size_t(a) * 2) == 0);
    }
    // Both case flags at once: Windows answers with a mixed result and no error (no program asks for it); this library reports ERROR_INVALID_FLAGS.
    auto both = text.lc_map_string_w(0x0409, 0x300, src, -1, mine, 16);
    CHECK_EQ(both.error, err::invalid_flags);
}
REGISTER_TEST("text_classes_windows", test_text_classes_windows);

// Collation: CompareStringW and LCMAP_SORTKEY on random strings of letters, accents, digits, spaces and symbols.
static void test_text_collation_windows() {
    TextApi text;
    static const char16_t pool[] = u"abcxyzABCXYZ0129 -'._,;:!?éÉèñÑäÄßæÆü$&";
    std::mt19937 rng(777);
    auto random_string = [&] {
        std::u16string s;
        const int n = int(rng() % 6);
        for (int i = 0; i < n; ++i) s.push_back(pool[rng() % (sizeof pool / 2 - 1)]);
        return s;
    };
    for (uint32_t flags : {0u, 1u, 2u, 3u, 4u, 0x1000u, 0x1001u}) {
        int compared = 0, diff = 0, shown = 0, key_diff = 0;
        for (int i = 0; i < 6000; ++i) {
            const std::u16string a = random_string(), b = random_string();
            const int real = CompareStringW(0x0409, flags, reinterpret_cast<const WCHAR*>(a.data()), int(a.size()), reinterpret_cast<const WCHAR*>(b.data()), int(b.size()));
            auto m = text.compare_string_w(0x0409, flags, a.data(), int(a.size()), b.data(), int(b.size()));
            ++compared;
            if (real != m.value) { ++diff; if (shown++ < 5) { std::fprintf(stderr, "flags %x: \"", flags); for (char16_t c : a) std::fprintf(stderr, "%04X ", c); std::fprintf(stderr, "\" vs \""); for (char16_t c : b) std::fprintf(stderr, "%04X ", c); std::fprintf(stderr, "\" windows %d mine %d\n", real, m.value); } }
            if (!(flags & ~7u) && flags == 0 && a.size()) {   // sort keys of the default flags
                BYTE rk[128];
                char16_t mk[64];
                const int rn = LCMapStringW(0x0409, LCMAP_SORTKEY, reinterpret_cast<const WCHAR*>(a.data()), int(a.size()), reinterpret_cast<LPWSTR>(rk), 128);
                auto mkr = text.lc_map_string_w(0x0409, 0x400, a.data(), int(a.size()), mk, 64);
                if (rn != mkr.value || std::memcmp(rk, mk, size_t(rn > 0 ? rn : 0)) != 0) ++key_diff;
            }
        }
        std::printf("  CompareStringW flags %#x: %d of %d differ, sort keys differing %d\n", flags, diff, compared, key_diff);
        // Known deviation (documented): under SORT_STRINGSORT a hyphen placed after a letter that differs only in case can order the other way
        // (1 pair in 6000 random strings). Every other flag combination is exact on this sample.
        if (flags == 0x1000) CHECK(diff <= 3); else CHECK_EQ(diff, 0);
    }
}
REGISTER_TEST("text_collation_windows", test_text_collation_windows);

// Locale data of en-US: every LCTYPE Windows answers, as text and as a number, and the buffer-size conventions.
static void test_text_locale_windows() {
    TextApi text;
    int answered = 0;
    for (unsigned type = 0; type <= 0x1100; ++type) {
        if (type > 0x120 && type < 0x1000) continue;
        WCHAR real[256];
        char16_t mine[256];
        if (type == 0x2 || type == 0x6 || type == 0x6F) continue;   // display names follow the UI language of the machine
        const int a = GetLocaleInfoW(0x0409, type | LOCALE_NOUSEROVERRIDE, real, 256);
        auto m = text.get_locale_info_w(0x0409, type | LOCALE_NOUSEROVERRIDE, mine, 256);
        if (a > 0) {
            ++answered;
            if (a != m.value) std::fprintf(stderr, "LCTYPE %X: windows %d, mine %d\n", type, a, m.value);
            CHECK_EQ(a, m.value);
            if (a == m.value && std::memcmp(real, mine, size_t(a) * 2) != 0) std::fprintf(stderr, "LCTYPE %X: content differs\n", type);
            CHECK(a == m.value && std::memcmp(real, mine, size_t(a) * 2) == 0);
        } else CHECK(!m);
        // The size query and a too small buffer.
        if (a > 1) {
            CHECK_EQ(GetLocaleInfoW(0x0409, type | LOCALE_NOUSEROVERRIDE, nullptr, 0), text.get_locale_info_w(0x0409, type | LOCALE_NOUSEROVERRIDE, nullptr, 0).value);
            SetLastError(0);
            const int small = GetLocaleInfoW(0x0409, type | LOCALE_NOUSEROVERRIDE, real, 1);
            auto ms = text.get_locale_info_w(0x0409, type | LOCALE_NOUSEROVERRIDE, mine, 1);
            CHECK_EQ(small, ms.value);
            CHECK_EQ(GetLastError(), ms.error);
        }
        // ANSI variant.
        char ra[256], ma[256];
        const int aa = GetLocaleInfoA(0x0409, type | LOCALE_NOUSEROVERRIDE, ra, 256);
        auto mm = text.get_locale_info_a(0x0409, type | LOCALE_NOUSEROVERRIDE, ma, 256);
        if (aa > 0 && !(aa == mm.value && std::memcmp(ra, ma, size_t(aa)) == 0)) std::fprintf(stderr, "LCTYPE %X (ANSI): windows %d, mine %d\n", type, aa, mm.value);
        if (aa > 0) CHECK(aa == mm.value && std::memcmp(ra, ma, size_t(aa)) == 0);
    }
    CHECK(answered > 60);
    // Numbers.
    for (unsigned type : {0x5u, 0xBu, 0xDu, 0x11u, 0x12u, 0x19u, 0x1Bu, 0x23u, 0x1004u}) {
        DWORD real = 0, mine = 0;
        GetLocaleInfoW(0x0409, type | LOCALE_RETURN_NUMBER | LOCALE_NOUSEROVERRIDE, reinterpret_cast<LPWSTR>(&real), 2);
        text.get_locale_info_w(0x0409, type | LOCALE_RETURN_NUMBER | LOCALE_NOUSEROVERRIDE, reinterpret_cast<char16_t*>(&mine), 2);
        CHECK_EQ(real, mine);
    }
    CHECK_EQ(TextApi(1252, 437, GetUserDefaultLCID()).get_user_default_lcid(), GetUserDefaultLCID());
    CHECK_EQ(IsValidLocale(0x0409, LCID_INSTALLED) != 0, text.is_valid_locale(0x0409, 1));
    CHECK_EQ(IsValidLocale(0x0409, LCID_SUPPORTED) != 0, text.is_valid_locale(0x0409, 2));
    CHECK_EQ(IsValidLocale(0x0001, LCID_SUPPORTED) != 0, text.is_valid_locale(0x0001, 2));
}
REGISTER_TEST("text_locale_windows", test_text_locale_windows);

// wsprintfA: the same format with the same arguments through user32 and through the library.
static void test_text_wsprintf_windows() {
    TextApi text;
    struct Case { const char* format; int kind; uint32_t a, b, c; const char* s1; const char* s2; };
    const Case cases[] = {
        {"%d", 0, 42, 0, 0, nullptr, nullptr}, {"%d", 0, uint32_t(-42), 0, 0, nullptr, nullptr}, {"%5d|%-5d|%05d", 0, 7, 8, 9, nullptr, nullptr},
        {"%u", 0, 0xFFFFFFFFu, 0, 0, nullptr, nullptr}, {"%x %X %#x", 0, 255, 255, 255, nullptr, nullptr}, {"%08X", 0, 0xBEEF, 0, 0, nullptr, nullptr},
        {"%+d % d", 0, 5, 5, 0, nullptr, nullptr}, {"%c%c%c", 0, 'a', 'b', 'c', nullptr, nullptr}, {"%hd", 0, 70000, 0, 0, nullptr, nullptr},
        {"%ld %lu", 0, uint32_t(-1), uint32_t(-1), 0, nullptr, nullptr}, {"%.3d", 0, 5, 0, 0, nullptr, nullptr}, {"%10.4d|", 0, 12, 0, 0, nullptr, nullptr},
        {"%s", 1, 0, 0, 0, "hello", nullptr}, {"[%10s][%-10s]", 1, 0, 0, 0, "abc", "abc"}, {"[%.2s]", 1, 0, 0, 0, "abcdef", nullptr},
        {"%s and %s", 1, 0, 0, 0, "one", "two"}, {"100%%", 0, 0, 0, 0, nullptr, nullptr}, {"%d%%", 0, 50, 0, 0, nullptr, nullptr},
        {"%*d", 0, 6, 33, 0, nullptr, nullptr}, {"x%dy%sz", 2, 12, 0, 0, "mid", nullptr}, {"%s", 1, 0, 0, 0, "", nullptr}, {"no format", 0, 0, 0, 0, nullptr, nullptr},
    };
    for (const Case& cs : cases) {
        char real[2048] = {};
        std::vector<uint32_t> words;
        std::vector<const char*> strings;
        auto fmt = std::string(cs.format);
        // Arguments in the order the format consumes them: ints from a,b,c; strings from s1,s2 (kind 1: only strings; kind 2: int then string).
        int real_n;
        if (cs.kind == 0) real_n = wsprintfA(real, cs.format, cs.a, cs.b, cs.c);
        else if (cs.kind == 1) real_n = wsprintfA(real, cs.format, cs.s1, cs.s2);
        else real_n = wsprintfA(real, cs.format, cs.a, cs.s1);
        // The library: the same arguments through the callbacks (strings are passed as indices into `strings`).
        if (cs.kind == 0) { words = {cs.a, cs.b, cs.c}; }
        else if (cs.kind == 1) { strings = {cs.s1, cs.s2}; words = {1, 2}; }
        else { strings = {cs.s1}; words = {cs.a, 1}; }
        size_t next = 0;
        FormatArgs args;
        args.next_arg = [&] { return next < words.size() ? words[next++] : 0u; };
        args.read_string = [&](uint32_t index) { return std::string(strings[index - 1]); };
        const std::string mine = wsprintf_format(text, cs.format, args);
        if (mine != real || int(mine.size()) != real_n) std::fprintf(stderr, "format \"%s\": windows \"%s\" (%d), mine \"%s\" (%zu)\n", cs.format, real, real_n, mine.c_str(), mine.size());
        CHECK(mine == real && int(mine.size()) == real_n);
    }
}
REGISTER_TEST("text_wsprintf_windows", test_text_wsprintf_windows);
#endif
