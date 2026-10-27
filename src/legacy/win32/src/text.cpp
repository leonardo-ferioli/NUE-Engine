#include "nue_legacy/win32/text.hpp"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace nue::legacy::win32 {
namespace {
#include "text_data.inc"

constexpr char32_t kReplacement = 0xFFFD;

enum CType1 : uint16_t { C1_UPPER = 1, C1_LOWER = 2, C1_DIGIT = 4, C1_SPACE = 8, C1_PUNCT = 0x10, C1_CNTRL = 0x20, C1_BLANK = 0x40, C1_XDIGIT = 0x80, C1_ALPHA = 0x100, C1_DEFINED = 0x200 };

// ------------------------------------------------------------------------------------------------------------------------------ code pages
const uint16_t* high_table(uint32_t code_page) {
    switch (code_page) {
    case 437: return kCp437;
    case 850: return kCp850;
    case 1252: return kCp1252;
    case 28591: return kCp28591;
    default: return nullptr;
    }
}

uint32_t resolve_code_page(uint32_t code_page, uint32_t acp, uint32_t oemcp) {
    switch (code_page) {
    case 0: case 3: return acp;      // CP_ACP, CP_THREAD_ACP
    case 1: return oemcp;            // CP_OEMCP
    default: return code_page;
    }
}

// Decodes one UTF-8 sequence; length 0 for an invalid one (the caller skips one byte).
char32_t decode_utf8(const uint8_t* p, size_t available, size_t& length) {
    const uint8_t b = p[0];
    length = 0;
    if (b < 0x80) { length = 1; return b; }
    size_t need;
    char32_t value, minimum;
    if (b >= 0xC2 && b <= 0xDF) { need = 1; value = b & 0x1F; minimum = 0x80; }
    else if (b >= 0xE0 && b <= 0xEF) { need = 2; value = b & 0x0F; minimum = 0x800; }
    else if (b >= 0xF0 && b <= 0xF4) { need = 3; value = b & 0x07; minimum = 0x10000; }
    else return 0;
    if (available < need + 1) return 0;
    for (size_t i = 1; i <= need; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return 0;
    length = need + 1;
    return value;
}

void append_utf16(std::u16string& out, char32_t c) {
    if (c >= 0x10000) { c -= 0x10000; out.push_back(char16_t(0xD800 + (c >> 10))); out.push_back(char16_t(0xDC00 + (c & 0x3FF))); }
    else out.push_back(char16_t(c));
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) out.push_back(char(c));
    else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
    else if (c < 0x10000) { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
    else { out.push_back(char(0xF0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 0x3F))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
}

// What Windows "best fit" maps a character that the single-byte code page lacks to (0 = none: the default character is used). Generated data.
uint8_t best_fit(uint32_t code_page, char16_t c) {
    const BestFit* table = code_page == 437 ? kBestFit437 : code_page == 850 ? kBestFit850 : code_page == 1252 ? kBestFit1252 : code_page == 28591 ? kBestFit28591 : nullptr;
    if (!table) return 0;
    size_t count = 0;
    while (table[count].c) ++count;   // the tables end with a {0,0} sentinel and are sorted by character
    size_t lo = 0, hi = count;
    while (lo < hi) { const size_t mid = (lo + hi) / 2; if (table[mid].c < c) lo = mid + 1; else hi = mid; }
    return lo < count && table[lo].c == c ? table[lo].byte : 0;
}

// ------------------------------------------------------------------------------------------------------------------------------ Unicode data
char16_t map_char(const uint16_t (*table)[2], size_t count, char16_t c) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (table[mid][0] < c) lo = mid + 1; else hi = mid;
    }
    return lo < count && table[lo][0] == c ? char16_t(table[lo][1]) : c;
}

// ------------------------------------------------------------------------------------------------------------------------------ collation
// A sort key built from the per-character keys that Windows gives (kSortKeys): primary weights of all characters, then diacritic, case, "extra"
// and special weights; a character with no primary weight (hyphen, apostrophe...) is ignored unless STRINGSORT is asked.
struct CharKey { std::string primary, diacritic, kase, extra, special; };

std::map<char16_t, CharKey> parse_keys(const SortKeyEntry* entries, size_t count) {
        std::map<char16_t, CharKey> t;
        for (size_t index = 0; index < count; ++index) {
            const auto& e = entries[index];
            CharKey k;
            std::string* sections[5] = {&k.primary, &k.diacritic, &k.kase, &k.extra, &k.special};
            int section = 0;
            // Layout: P 01 D 01 C 01 X 01 S 00. A special-weight character starts with four empty sections (01 01 01 01) then FF FF ...
            const uint8_t* p = reinterpret_cast<const uint8_t*>(e.bytes);
            for (int i = 0; i < e.length - 1; ++i) {
                if (p[i] == 1 && section < 4) { ++section; continue; }
                sections[section]->push_back(char(p[i]));
            }
            t[char16_t(e.c)] = std::move(k);
        }
        return t;
}

// `string_sort`: SORT_STRINGSORT keys (hyphen and apostrophe count as symbols there); characters without an own string key use the word key.
const CharKey* char_key(char16_t c, bool string_sort) {
    static const std::map<char16_t, CharKey> word = parse_keys(kSortKeys, sizeof kSortKeys / sizeof kSortKeys[0]);
    static const std::map<char16_t, CharKey> string = parse_keys(kSortKeysString, sizeof kSortKeysString / sizeof kSortKeysString[0]);
    if (string_sort) { auto it = string.find(c); if (it != string.end()) return &it->second; }
    auto it = word.find(c);
    return it == word.end() ? nullptr : &it->second;
}

std::string make_sort_key(const char16_t* s, size_t n, uint32_t flags) {
    const bool ignore_case = flags & 1, ignore_nonspace = flags & 2, ignore_symbols = flags & 4, string_sort = flags & 0x1000;
    std::string primary, diacritic, kase, extra, special;
    size_t kept = 0;   // characters that carry a primary weight so far: the position stored with an ignored character
    for (size_t i = 0; i < n; ++i) {
        const char16_t c = s[i];
        const CharKey* k = char_key(c, string_sort);
        if (!k) {   // outside the table: ordered after every covered character by code point
            primary.push_back(char(0xFE)); primary.push_back(char(c >> 8)); primary.push_back(char(c & 0xFF));
            diacritic.push_back(2); kase.push_back(2);
            ++kept;
            continue;
        }
        if (k->primary.empty()) {   // an ignored character (hyphen, apostrophe) or a control: word sort skips it
            if (k->special.size() < 3 || ignore_symbols) continue;
            special.push_back(char(0xFF));                  // an ignored character is remembered with its position: FF, 0xFF - characters before it, weights
            special.push_back(char(0xFF - (kept > 0xFE ? 0xFE : kept)));
            special += k->special.substr(2);
            continue;
        }
        const bool symbol = static_cast<uint8_t>(k->primary[0]) < 0x0D;   // script weights below the digits are symbols (space, punctuation, currency...)
        if (ignore_symbols && symbol) continue;
        primary += k->primary;
        ++kept;
        const size_t units = k->primary.size() / 2;
        for (size_t u = 0; u < units; ++u) {
            diacritic.push_back(u < k->diacritic.size() ? k->diacritic[u] : char(2));
            kase.push_back(u < k->kase.size() ? k->kase[u] : char(2));
        }
    }
    auto trim = [](std::string& v) { while (!v.empty() && static_cast<uint8_t>(v.back()) == 2) v.pop_back(); };
    trim(diacritic);
    trim(kase);
    std::string key = primary;
    key.push_back(1);
    if (!ignore_nonspace) key += diacritic;
    key.push_back(1);
    if (!ignore_case) key += kase;
    key.push_back(1);
    key += extra;
    key.push_back(1);
    key += special;
    key.push_back(0);
    return key;
}

int count_of(const char16_t* s, int count) {
    if (count >= 0) return count;
    int n = 0;
    while (s[n]) ++n;
    return n;
}

}  // namespace

// ------------------------------------------------------------------------------------------------------------------------------ character data
char32_t TextApi::to_lower(char32_t c) { return c <= 0x4FF ? map_char(kLowerMap, sizeof kLowerMap / sizeof kLowerMap[0], char16_t(c)) : c; }
char32_t TextApi::to_upper(char32_t c) { return c <= 0x4FF ? map_char(kUpperMap, sizeof kUpperMap / sizeof kUpperMap[0], char16_t(c)) : c; }
uint16_t TextApi::ctype1(char32_t c) {
    if (c <= 0x4FF) return kCType1[c];
    if (c >= 0xD800 && c <= 0xDFFF) return 0;                          // lone surrogates are not defined
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3040 && c <= 0x30FF)) return C1_ALPHA | C1_DEFINED;   // ideographs, kana: letters without case
    if (c >= 0x2000 && c <= 0x206F) return C1_PUNCT | C1_DEFINED;      // general punctuation
    return C1_DEFINED;
}

// ------------------------------------------------------------------------------------------------------------------------------ code pages
bool TextApi::is_valid_code_page(uint32_t code_page) const {
    switch (code_page) { case 437: case 850: case 1252: case 28591: case 20127: case 65001: return true; default: return false; }
}

Ret<CodePageInfo> TextApi::get_cp_info(uint32_t code_page) const {
    code_page = resolve_code_page(code_page, acp_, oemcp_);
    if (!is_valid_code_page(code_page)) return fail<CodePageInfo>(err::invalid_parameter);
    CodePageInfo info;
    if (code_page == cp::utf8) { info.max_char_size = 4; info.default_char[0] = '?'; }
    return ok(info);
}

Ret<int> TextApi::multi_byte_to_wide(uint32_t code_page, uint32_t flags, const char* source, int source_count, char16_t* out, int out_capacity) const {
    code_page = resolve_code_page(code_page, acp_, oemcp_);
    if (!source || source_count == 0 || source_count < -1 || out_capacity < 0 || (out_capacity > 0 && !out)) return fail<int>(err::invalid_parameter);
    if (!is_valid_code_page(code_page)) return fail<int>(err::invalid_parameter);
    const bool utf8 = code_page == cp::utf8;
    if (utf8 ? (flags & ~0x8u) != 0 : (flags & ~0xFu) != 0) return fail<int>(err::invalid_flags);
    const size_t n = source_count == -1 ? std::strlen(source) + 1 : size_t(source_count);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(source);
    std::u16string result;
    const uint16_t* table = high_table(code_page);
    for (size_t i = 0; i < n;) {
        if (utf8) {
            size_t length;
            const char32_t c = decode_utf8(bytes + i, n - i, length);
            if (length == 0) {
                if (flags & 0x8) return fail<int>(1113);   // ERROR_NO_UNICODE_TRANSLATION
                append_utf16(result, kReplacement);
                ++i;
            } else { append_utf16(result, c); i += length; }
        } else {
            const uint8_t b = bytes[i++];
            if (b < 0x80) result.push_back(b);
            else if (table) result.push_back(char16_t(table[b - 0x80]));
            else if (flags & 0x8) return fail<int>(1113);
            else result.push_back(char16_t(0xFFFD));   // ASCII code page: a byte above 0x7F has no translation
        }
    }
    if (out_capacity == 0) return ok(int(result.size()));
    if (result.size() > size_t(out_capacity)) return fail<int>(err::insufficient_buffer);
    std::copy(result.begin(), result.end(), out);
    return ok(int(result.size()));
}

Ret<int> TextApi::wide_to_multi_byte(uint32_t code_page, uint32_t flags, const char16_t* source, int source_count, char* out, int out_capacity, const char* default_char, bool* used_default) const {
    code_page = resolve_code_page(code_page, acp_, oemcp_);
    if (!source || source_count == 0 || source_count < -1 || out_capacity < 0 || (out_capacity > 0 && !out)) return fail<int>(err::invalid_parameter);
    if (!is_valid_code_page(code_page)) return fail<int>(err::invalid_parameter);
    const bool utf8 = code_page == cp::utf8;
    if (utf8 && (default_char || used_default)) return fail<int>(err::invalid_parameter);
    if (utf8 ? (flags & ~0x80u) != 0 : (flags & ~(0x10u | 0x20u | 0x40u | 0x200u | 0x400u)) != 0) return fail<int>(err::invalid_flags);
    const int n = count_of(source, source_count) + (source_count == -1 ? 1 : 0);
    const char replacement = default_char ? default_char[0] : '?';
    std::string result;
    if (used_default) *used_default = false;
    const uint16_t* table = high_table(code_page);
    for (int i = 0; i < n; ++i) {
        char32_t c = source[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && source[i + 1] >= 0xDC00 && source[i + 1] <= 0xDFFF) { c = 0x10000 + ((c - 0xD800) << 10) + (source[i + 1] - 0xDC00); ++i; }
        else if (c >= 0xD800 && c <= 0xDFFF) {   // unpaired surrogate
            if (utf8 && (flags & 0x80)) return fail<int>(1113);
            if (utf8) { append_utf8(result, kReplacement); continue; }
            result.push_back(replacement);
            if (used_default) *used_default = true;
            continue;
        }
        if (utf8) { append_utf8(result, c); continue; }
        if (c < 0x80) { result.push_back(char(c)); continue; }
        int found = -1;
        if (table) for (int b = 0; b < 128; ++b) if (table[b] == c) { found = 0x80 + b; break; }
        if (found >= 0) { result.push_back(char(found)); continue; }
        if (!(flags & 0x400) && c <= 0xFFFF) {   // best fit: an accented letter becomes its plain letter, as Windows does
            if (const uint8_t fit = best_fit(code_page, char16_t(c))) { result.push_back(char(fit)); continue; }
        }
        result.push_back(replacement);
        if (used_default) *used_default = true;
    }
    if (out_capacity == 0) return ok(int(result.size()));
    if (result.size() > size_t(out_capacity)) return fail<int>(err::insufficient_buffer);
    std::memcpy(out, result.data(), result.size());
    return ok(int(result.size()));
}

std::u16string TextApi::to_wide(uint32_t code_page, std::string_view s) const {
    if (s.empty()) return {};
    auto needed = multi_byte_to_wide(code_page, 0, s.data(), int(s.size()), nullptr, 0);
    std::u16string out(needed.value > 0 ? size_t(needed.value) : 0, u'\0');
    if (needed.value > 0) multi_byte_to_wide(code_page, 0, s.data(), int(s.size()), out.data(), int(out.size()));
    return out;
}

std::string TextApi::to_multi_byte(uint32_t code_page, std::u16string_view s) const {
    if (s.empty()) return {};
    auto needed = wide_to_multi_byte(code_page, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(needed.value > 0 ? size_t(needed.value) : 0, '\0');
    if (needed.value > 0) wide_to_multi_byte(code_page, 0, s.data(), int(s.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}

// ------------------------------------------------------------------------------------------------------------------------------ character types
Ret<bool> TextApi::get_string_type_w(uint32_t type, const char16_t* source, int count, uint16_t* out) const {
    if (!source || !out || count == 0 || count < -1 || (type != 1 && type != 2 && type != 4)) return fail<bool>(err::invalid_parameter);
    const int n = count_of(source, count) + (count == -1 ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        const char16_t c = source[i];
        if (type == 1) out[i] = ctype1(c);
        else if (c <= 0x4FF) out[i] = type == 2 ? kCType2[c] : kCType3[c];
        else out[i] = 0;
    }
    return ok(true);
}

Ret<bool> TextApi::get_string_type_a(uint32_t locale, uint32_t type, const char* source, int count, uint16_t* out) const {
    (void)locale;
    if (!source || !out || count == 0 || count < -1) return fail<bool>(err::invalid_parameter);
    auto wide = multi_byte_to_wide(cp::acp, 0, source, count, nullptr, 0);
    if (!wide) return fail<bool>(wide.error);
    std::u16string w(size_t(wide.value), u'\0');
    multi_byte_to_wide(cp::acp, 0, source, count, w.data(), wide.value);
    return get_string_type_w(type, w.data(), wide.value, out);
}

// ------------------------------------------------------------------------------------------------------------------------------ comparison and mapping
Ret<int> TextApi::compare_string_w(uint32_t locale, uint32_t flags, const char16_t* a, int a_count, const char16_t* b, int b_count) const {
    (void)locale;
    if (!a || !b || a_count == 0 && false) return fail<int>(err::invalid_parameter);
    if (flags & ~(0x1u | 0x2u | 0x4u | 0x1000u | 0x10000u | 0x20000u | 0x20000000u | 0x08000000u)) return fail<int>(err::invalid_flags);
    const int na = count_of(a, a_count), nb = count_of(b, b_count);
    const std::string ka = make_sort_key(a, size_t(na), flags), kb = make_sort_key(b, size_t(nb), flags);
    const int c = ka.compare(kb);
    return ok(c < 0 ? int(cmp::less) : c > 0 ? int(cmp::greater) : int(cmp::equal));
}

Ret<int> TextApi::compare_string_a(uint32_t locale, uint32_t flags, const char* a, int a_count, const char* b, int b_count) const {
    if (!a || !b) return fail<int>(err::invalid_parameter);
    const int na = a_count < 0 ? int(std::strlen(a)) : a_count, nb = b_count < 0 ? int(std::strlen(b)) : b_count;
    const std::u16string wa = to_wide(cp::acp, std::string_view(a, size_t(na))), wb = to_wide(cp::acp, std::string_view(b, size_t(nb)));
    return compare_string_w(locale, flags, wa.data(), int(wa.size()), wb.data(), int(wb.size()));
}

int TextApi::lstrcmpi_a(const char* a, const char* b) const {
    auto r = compare_string_a(lcid::user_default, 1, a ? a : "", -1, b ? b : "", -1);
    return r.value == int(cmp::less) ? -1 : r.value == int(cmp::greater) ? 1 : 0;
}

Ret<int> TextApi::lc_map_string_w(uint32_t locale, uint32_t flags, const char16_t* source, int source_count, char16_t* out, int out_capacity) const {
    (void)locale;
    if (!source || source_count == 0 || source_count < -1 || out_capacity < 0 || (out_capacity > 0 && !out)) return fail<int>(err::invalid_parameter);
    const uint32_t kinds = flags & (0x100 | 0x200);
    if (kinds == (0x100 | 0x200)) return fail<int>(err::invalid_flags);
    const int n = count_of(source, source_count) + (source_count == -1 ? 1 : 0);
    if (flags & 0x400) {   // LCMAP_SORTKEY: the output is bytes, written over the buffer
        const std::string key = make_sort_key(source, size_t(source_count == -1 ? n - 1 : n), flags & ~0x400u);
        if (out_capacity == 0) return ok(int(key.size()));
        if (key.size() > size_t(out_capacity) * 2) return fail<int>(err::insufficient_buffer);
        std::memcpy(out, key.data(), key.size());
        return ok(int(key.size()));
    }
    if (out_capacity == 0) return ok(n);
    if (n > out_capacity) return fail<int>(err::insufficient_buffer);
    for (int i = 0; i < n; ++i) {
        char16_t c = source[i];
        if (kinds == 0x100) c = char16_t(to_lower(c));
        else if (kinds == 0x200) c = char16_t(to_upper(c));
        if (flags & 0x800) c = char16_t((c >> 8) | (c << 8));
        out[i] = c;
    }
    return ok(n);
}

Ret<int> TextApi::lc_map_string_a(uint32_t locale, uint32_t flags, const char* source, int source_count, char* out, int out_capacity) const {
    if (!source || source_count == 0 || source_count < -1 || out_capacity < 0 || (out_capacity > 0 && !out)) return fail<int>(err::invalid_parameter);
    const int n = source_count < 0 ? int(std::strlen(source)) + 1 : source_count;
    const std::u16string w = to_wide(cp::acp, std::string_view(source, size_t(n)));
    if (flags & 0x400) {
        auto key = lc_map_string_w(locale, flags, w.data(), int(w.size()), nullptr, 0);
        if (out_capacity == 0 || !key) return key;
        if (key.value > out_capacity) return fail<int>(err::insufficient_buffer);
        std::string bytes(size_t(key.value), '\0');
        // Re-run into a wide-sized buffer, then hand the bytes over.
        std::u16string tmp((size_t(key.value) + 1) / 2, u'\0');
        lc_map_string_w(locale, flags, w.data(), int(w.size()), tmp.data(), int(tmp.size()));
        std::memcpy(out, tmp.data(), size_t(key.value));
        return key;
    }
    std::u16string mapped(w.size(), u'\0');
    auto r = lc_map_string_w(locale, flags, w.data(), int(w.size()), mapped.data(), int(mapped.size()));
    if (!r) return r;
    return wide_to_multi_byte(cp::acp, 0, mapped.data(), int(mapped.size()), out_capacity ? out : nullptr, out_capacity, nullptr, nullptr);
}

// ------------------------------------------------------------------------------------------------------------------------------ locale
bool TextApi::is_valid_locale(uint32_t locale, uint32_t flags) const {
    if (flags != 1 && flags != 2) return false;
    return locale == lcid::en_us || locale == lcid::invariant;
}

std::vector<std::string> TextApi::enum_system_locales(uint32_t flags) const {
    (void)flags;
    return {"00000409"};
}

Ret<int> TextApi::get_locale_info_w(uint32_t locale, uint32_t type, char16_t* out, int out_capacity) const {
    if (out_capacity < 0 || (out_capacity > 0 && !out)) return fail<int>(err::invalid_parameter);
    const bool number = type & 0x20000000u;
    uint32_t base = type & ~(0x80000000u | 0x20000000u | 0x10000000u);
    if (locale == lcid::user_default || locale == lcid::system_default || locale == lcid::neutral) locale = lcid::en_us;
    if (locale != lcid::en_us && locale != lcid::invariant) return fail<int>(err::invalid_parameter);
    const char16_t* text = nullptr;
    int length = 0;
    for (const auto& e : kLocaleEnUs) if (e.type == base) { text = e.text; length = int(e.length); break; }   // `length` counts the terminator when there is one
    if (!text) {   // display names: English, whatever the UI language of the machine the table was generated on
        if (base == 0x2) text = u"English (United States)"; else if (base == 0x6) text = u"United States"; else if (base == 0x6F) text = u"English";
        if (text) { while (text[length]) ++length; ++length; }
    }
    if (!text) return fail<int>(err::invalid_flags);
    if (number) {
        if (out_capacity != 0 && out_capacity < 2) return fail<int>(err::insufficient_buffer);
        uint32_t value = 0;
        const int radix = base == 1 ? 16 : 10;   // LOCALE_ILANGUAGE is a hex string
        for (const char16_t* p = text; *p; ++p) { const int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 0x20) >= 'a' && (*p | 0x20) <= 'f' ? (*p | 0x20) - 'a' + 10 : -1; if (d < 0 || d >= radix) break; value = value * uint32_t(radix) + uint32_t(d); }
        if (out_capacity != 0) { out[0] = char16_t(value & 0xFFFF); out[1] = char16_t(value >> 16); }
        return ok(2);
    }
    if (out_capacity == 0) return ok(length);
    if (length > out_capacity) return fail<int>(err::insufficient_buffer);
    std::copy(text, text + length, out);
    return ok(length);
}

Ret<int> TextApi::get_locale_info_a(uint32_t locale, uint32_t type, char* out, int out_capacity) const {
    if (out_capacity < 0 || (out_capacity > 0 && !out)) return fail<int>(err::invalid_parameter);
    char16_t wide[512];
    uint32_t wide_type = type;
    if (type & 0x20000000u) {   // a number: a DWORD in the buffer
        auto r = get_locale_info_w(locale, wide_type, wide, 512);
        if (!r) return r;
        if (out_capacity == 0) return ok(4);
        if (out_capacity < 4) return fail<int>(err::insufficient_buffer);
        const uint32_t value = uint32_t(wide[0]) | (uint32_t(wide[1]) << 16);
        std::memcpy(out, &value, 4);
        return ok(4);
    }
    auto r = get_locale_info_w(locale, wide_type, wide, 512);
    if (!r) return r;
    if ((type & 0x0FFFFFFF) == 0x58) {   // LOCALE_FONTSIGNATURE is binary: the ANSI variant returns the same bytes
        const int bytes = r.value * 2;
        if (out_capacity == 0) return ok(bytes);
        if (out_capacity < bytes) return fail<int>(err::insufficient_buffer);
        std::memcpy(out, wide, size_t(bytes));
        return ok(bytes);
    }
    return wide_to_multi_byte(cp::acp, 0, wide, r.value, out_capacity ? out : nullptr, out_capacity, nullptr, nullptr);
}

// ------------------------------------------------------------------------------------------------------------------------------ wsprintf
std::string wsprintf_format(const TextApi& text, std::string_view format, FormatArgs& args) {
    (void)text;
    std::string out;
    const size_t limit = 1024;
    auto put = [&](const std::string& s) { for (char c : s) { if (out.size() >= limit - 1) return; out.push_back(c); } };
    for (size_t i = 0; i < format.size() && out.size() < limit - 1; ++i) {
        if (format[i] != '%') { out.push_back(format[i]); continue; }
        ++i;
        if (i >= format.size()) break;
        if (format[i] == '%') { out.push_back('%'); continue; }
        // wsprintf is not printf: the only flags are '-', '#' and '0', and there is no '*' width or precision. Anything else after '%' that is
        // not a width, precision, length or type is an unknown conversion: the '%' is dropped and the character is copied.
        bool left = false, zero = false, alt = false;
        for (; i < format.size(); ++i) {
            if (format[i] == '-') left = true; else if (format[i] == '0') zero = true; else if (format[i] == '#') alt = true; else break;
        }
        const bool plus = false, space = false;
        int width = 0, precision = -1;
        while (i < format.size() && format[i] >= '0' && format[i] <= '9') width = width * 10 + (format[i++] - '0');
        if (i < format.size() && format[i] == '.') {
            ++i; precision = 0;
            while (i < format.size() && format[i] >= '0' && format[i] <= '9') precision = precision * 10 + (format[i++] - '0');
        }
        bool is_long = false, is_short = false;
        for (; i < format.size(); ++i) {
            if (format[i] == 'l' || format[i] == 'L') is_long = true; else if (format[i] == 'h') is_short = true;
            else if (format[i] == 'I' && i + 2 < format.size() && format[i + 1] == '6' && format[i + 2] == '4') { i += 2; } else break;
        }
        if (i >= format.size()) break;
        const char type = format[i];
        std::string body;
        bool numeric = false;
        switch (type) {
        case 'c': case 'C': { const uint32_t v = args.next_arg(); body = (type == 'C' && !is_short) || (type == 'c' && is_long) ? std::string(1, char(v & 0xFF)) : std::string(1, char(v & 0xFF)); break; }
        case 's': case 'S': {
            const uint32_t p = args.next_arg();
            const bool wide = (type == 'S' && !is_short) || (type == 's' && is_long);
            if (!p) body = "(null)";
            else if (wide) { const std::u16string w = args.read_wide_string ? args.read_wide_string(p) : std::u16string(); body = text.to_multi_byte(cp::acp, w); }
            else body = args.read_string ? args.read_string(p) : std::string();
            if (precision >= 0 && body.size() > size_t(precision)) body.resize(size_t(precision));
            break;
        }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'p': {
            numeric = true;
            uint32_t raw = args.next_arg();
            if (is_short) raw = (type == 'd' || type == 'i') ? uint32_t(int32_t(int16_t(raw))) : (raw & 0xFFFF);
            char digits[40];
            int len = 0;
            bool negative = false;
            uint32_t v = raw;
            if (type == 'd' || type == 'i') { negative = int32_t(raw) < 0; v = negative ? uint32_t(0) - raw : raw; }
            const bool hex = type == 'x' || type == 'X' || type == 'p';
            const char* alphabet = type == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (v == 0) digits[len++] = '0';
            while (v) { digits[len++] = alphabet[hex ? (v & 15) : (v % 10)]; v = hex ? (v >> 4) : (v / 10); }
            std::string d(digits, size_t(len));
            std::reverse(d.begin(), d.end());
            if (precision >= 0) while (int(d.size()) < precision) d.insert(d.begin(), '0');
            std::string sign;
            if (type == 'd' || type == 'i') sign = negative ? "-" : plus ? "+" : space ? " " : "";
            if (alt && hex && raw != 0) sign = type == 'X' ? "0X" : "0x";
            if (type == 'p') { while (d.size() < 8) d.insert(d.begin(), '0'); }
            if (zero && !left && precision < 0) while (int(sign.size() + d.size()) < width) d.insert(d.begin(), '0');
            body = sign + d;
            break;
        }
        default:   // an unknown conversion: the character itself, the '%' is dropped
            out.push_back(type);
            continue;
        }
        (void)numeric;
        if (int(body.size()) < width) { const std::string pad(size_t(width) - body.size(), ' '); body = left ? body + pad : pad + body; }
        put(body);
    }
    return out;
}

}  // namespace nue::legacy::win32
