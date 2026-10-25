// JDLZ codec (the LZ77 variant used by the Need for Speed games of the PC / console era): decompressor and compressor.
//
// Stream layout (all little endian), written from the observation of real files (docs/ASSET_FORMATS.md, "JDLZ"):
//   0   "JDLZ"
//   4   u8 version (2), u8 header size (0x10), u16 0
//   8   u32 size of the decoded data
//   12  u32 size of the whole stream including this header
//   16  u8 flags A (8 token bits), u8 flags B (8 match type bits), then tokens
// A token is a literal (one byte) or a match (two bytes). Flags A says which, least significant bit first: 0 literal, 1 match. Flags B
// has one bit per match: 1 = "near" match, 0 = "far" match. When the 8 bits of a flag byte are used up, the next flag byte sits in the stream right after the
// token that used the last bit (flags A first, then flags B when both ran out on the same token).
//   near match, bytes f s:  count = ((f & 0xF0) << 4 | s) + 3   (3..4098)   distance = (f & 0x0F) + 1            (1..16)
//   far match,  bytes f s:  count = (f & 0x1F) + 3              (3..34)     distance = ((f & 0xE0) << 3 | s) + 17 (17..2064)
// The distance is counted back from the next output byte, so a match may overlap its own output (a run).
#include "nue/assets.h"

#include "bytes.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using nuea::rd32;
using nuea::wr32;

constexpr size_t kHeader = 16;
constexpr uint32_t kMinMatch = 3;
constexpr uint32_t kNearMaxDist = 16, kNearMaxLen = 4098;
constexpr uint32_t kFarMinDist = 17, kFarMaxDist = 2064, kFarMaxLen = 34;
constexpr uint32_t kWindowMask = 4095;                 // chain ring: a power of two above kFarMaxDist

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_jdlz_identify(const void* data, size_t size, uint32_t* out_uncompressed, uint32_t* out_stream) {
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (!d || size < kHeader || std::memcmp(d, "JDLZ", 4) != 0 || d[4] != 2 || d[5] != 0x10) return NUE_ERR_UNSUPPORTED;
    if (out_uncompressed) *out_uncompressed = rd32(d + 8);
    if (out_stream) *out_stream = rd32(d + 12);
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_jdlz_decompress(const void* src, size_t src_size, void* dst, size_t dst_size, size_t* out_written) {
    if (out_written) *out_written = 0;
    uint32_t unc = 0, stream = 0;
    nue_result r = nue_jdlz_identify(src, src_size, &unc, &stream);
    if (r < 0) return r;
    if (stream < kHeader + 2 && unc != 0) return NUE_ERR_CORRUPT;
    if (stream > src_size) return NUE_ERR_CORRUPT;               // truncated input
    if (unc > dst_size) return NUE_ERR_BUFFER_TOO_SMALL;
    if (!dst && unc) return NUE_ERR_INVALID_ARGUMENT;
    if (unc == 0) return NUE_OK;
    const uint8_t* in = static_cast<const uint8_t*>(src);
    uint8_t* out = static_cast<uint8_t*>(dst);
    const size_t end = stream;
    size_t cur = kHeader + 2, o = 0;
    unsigned flags = in[kHeader] | 256u, kinds = in[kHeader + 1] | 256u;
    while (o < unc) {
        if (cur >= end) return NUE_ERR_CORRUPT;
        if (!(flags & 1u)) {
            out[o++] = in[cur++];
        } else {
            if (cur + 2 > end) return NUE_ERR_CORRUPT;
            const unsigned f = in[cur], s = in[cur + 1];
            cur += 2;
            size_t count, dist;
            if (kinds & 1u) { count = (((f & 0xF0u) << 4) | s) + kMinMatch; dist = (f & 15u) + 1; }
            else { count = (f & 31u) + kMinMatch; dist = (((f & 0xE0u) << 3) | s) + kFarMinDist; }
            kinds >>= 1;
            if (dist > o || count > unc - o) return NUE_ERR_CORRUPT;
            const uint8_t* from = out + o - dist;
            uint8_t* to = out + o;
            if (dist >= count) std::memcpy(to, from, count);
            else for (size_t i = 0; i < count; ++i) to[i] = from[i];      // overlapping run: byte by byte
            o += count;
        }
        if (o == unc) break;
        flags >>= 1;
        if (flags == 1u) { if (cur >= end) return NUE_ERR_CORRUPT; flags = in[cur++] | 256u; }
        if (kinds == 1u) { if (cur >= end) return NUE_ERR_CORRUPT; kinds = in[cur++] | 256u; }
    }
    if (out_written) *out_written = o;
    return NUE_OK;
}

NUE_ASSETS_API size_t NUE_CALL nue_jdlz_compress_bound(size_t n) { return kHeader + 2 + n + n / 8 + 4; }

namespace {

struct TokenWriter {
    uint8_t* out;
    size_t pos = kHeader + 2;
    size_t flags_pos = kHeader, kinds_pos = kHeader + 1;
    unsigned flags = 0, flag_bits = 0, kinds = 0, kind_bits = 0;
    size_t tokens = 0;

    explicit TokenWriter(uint8_t* o) : out(o) {}

    void after_token(bool was_match) {
        if (++flag_bits == 8) {                          // the decoder fetches the next flags byte right after this token
            out[flags_pos] = uint8_t(flags);
            flags_pos = pos++;
            flags = 0; flag_bits = 0;
        }
        if (was_match && kind_bits == 8) {
            out[kinds_pos] = uint8_t(kinds);
            kinds_pos = pos++;
            kinds = 0; kind_bits = 0;
        }
    }
    void literal(uint8_t b) { out[pos++] = b; after_token(false); }
    void match(uint32_t count, uint32_t dist) {
        uint32_t f, s;
        const bool near_match = dist <= kNearMaxDist;
        if (near_match) { f = (((count - kMinMatch) >> 8) << 4) | (dist - 1); s = (count - kMinMatch) & 0xFF; }
        else { f = (((dist - kFarMinDist) >> 8) << 5) | (count - kMinMatch); s = (dist - kFarMinDist) & 0xFF; }
        out[pos++] = uint8_t(f);
        out[pos++] = uint8_t(s);
        flags |= 1u << flag_bits;
        if (near_match) kinds |= 1u << kind_bits;
        ++kind_bits;
        after_token(true);
    }
    size_t finish() {
        out[flags_pos] = uint8_t(flags);
        out[kinds_pos] = uint8_t(kinds);
        // flag bytes are stored as the lowest bits; unused high bits stay 0
        return pos;
    }
};

struct Finder {
    const uint8_t* d;
    size_t n;
    std::vector<uint32_t> head;                          // 65536 entries: position + 1
    uint32_t prev[kWindowMask + 1];
    uint32_t depth;

    Finder(const uint8_t* data, size_t size, uint32_t chain) : d(data), n(size), head(65536, 0), depth(chain) { std::memset(prev, 0, sizeof(prev)); }

    static uint32_t hash3(const uint8_t* p) { return ((uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16)) * 2654435761u) >> 16; }

    void insert(size_t i) {
        if (i + 3 > n) return;
        const uint32_t h = hash3(d + i);
        prev[i & kWindowMask] = head[h];
        head[h] = uint32_t(i + 1);
    }

    // Candidates from the nearest to the farthest; each has its own longest length. Calls fn(len, dist).
    template <class Fn> void for_each(size_t i, size_t limit_end, Fn fn) {
        if (i + kMinMatch > limit_end) return;
        uint32_t p1 = head[hash3(d + i)];
        for (uint32_t steps = 0; p1 && steps < depth; ++steps) {
            const size_t p = p1 - 1;
            if (p >= i) break;
            const size_t dist = i - p;
            if (dist > kFarMaxDist) break;
            const size_t max_len = std::min<size_t>(limit_end - i, dist <= kNearMaxDist ? kNearMaxLen : kFarMaxLen);
            size_t len = 0;
            while (len < max_len && d[p + len] == d[i + len]) ++len;
            if (len >= kMinMatch) fn(uint32_t(len), uint32_t(dist));
            const uint32_t next = prev[p & kWindowMask];
            if (next == 0 || next - 1 >= p) break;
            p1 = next;
        }
    }
};

uint32_t chain_depth(int level) {
    static const uint32_t table[10] = {1, 4, 8, 16, 32, 64, 128, 256, 512, 1024};
    return table[level < 0 ? 0 : (level > 9 ? 9 : level)];
}

void parse_greedy(const uint8_t* d, size_t n, int level, TokenWriter& w) {
    Finder f(d, n, chain_depth(level));
    const bool lazy = level >= 3;
    size_t i = 0;
    auto best_at = [&](size_t pos, uint32_t& len, uint32_t& dist) {
        len = 0; dist = 0;
        f.for_each(pos, n, [&](uint32_t l, uint32_t ds) { if (l > len) { len = l; dist = ds; } });
    };
    while (i < n) {
        uint32_t len, dist;
        best_at(i, len, dist);
        if (len >= kMinMatch && lazy && i + 1 < n) {
            f.insert(i);
            uint32_t len2, dist2;
            best_at(i + 1, len2, dist2);
            if (len2 > len + 1) { w.literal(d[i]); ++i; continue; }      // a clearly longer match starts at the next byte
            // keep the first match; position i is already in the chains
            w.match(len, dist);
            for (uint32_t k = 1; k < len; ++k) f.insert(i + k);
            i += len;
            continue;
        }
        if (len >= kMinMatch) {
            w.match(len, dist);
            for (uint32_t k = 0; k < len; ++k) f.insert(i + k);
            i += len;
        } else {
            f.insert(i);
            w.literal(d[i]);
            ++i;
        }
    }
}

// Optimal parse inside blocks: literal = 9 bits, any match = 18 bits (flag bit + type bit + 16 bits).
void parse_optimal(const uint8_t* d, size_t n, int level, TokenWriter& w) {
    Finder f(d, n, chain_depth(level));
    constexpr size_t kBlock = 1u << 15;
    constexpr uint32_t kInf = 0xFFFFFFFFu;
    std::vector<uint32_t> cost;
    std::vector<uint16_t> blen, bdist;
    struct Tok { uint16_t len, dist; };
    std::vector<Tok> toks;
    for (size_t b = 0; b < n;) {
        const size_t e = std::min(n, b + kBlock), m = e - b;
        cost.assign(m + 1, kInf);
        blen.assign(m + 1, 0);
        bdist.assign(m + 1, 0);
        cost[0] = 0;
        for (size_t i = 0; i < m; ++i) {
            const size_t pos = b + i;
            const uint32_t base = cost[i];
            if (base + 9 < cost[i + 1]) { cost[i + 1] = base + 9; blen[i + 1] = 1; bdist[i + 1] = 0; }
            f.for_each(pos, e, [&](uint32_t len, uint32_t dist) {
                const uint32_t top = len <= kFarMaxLen ? len : len;
                for (uint32_t l = kMinMatch; l <= std::min<uint32_t>(top, kFarMaxLen); ++l)
                    if (base + 18 < cost[i + l]) { cost[i + l] = base + 18; blen[i + l] = uint16_t(l); bdist[i + l] = uint16_t(dist); }
                if (top > kFarMaxLen && base + 18 < cost[i + top]) { cost[i + top] = base + 18; blen[i + top] = uint16_t(top); bdist[i + top] = uint16_t(dist); }
            });
            f.insert(pos);
        }
        toks.clear();
        for (size_t i = m; i > 0;) {
            const uint16_t l = blen[i];
            toks.push_back({l, bdist[i]});
            i -= l;
        }
        for (size_t k = toks.size(); k-- > 0;) {
            if (toks[k].len == 1) w.literal(d[b++]);
            else { w.match(toks[k].len, toks[k].dist); b += toks[k].len; }
        }
    }
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_jdlz_compress(const void* src, size_t src_size, int32_t level, void* dst, size_t dst_capacity, size_t* out_written) {
    if (out_written) *out_written = 0;
    if ((!src && src_size) || !dst) return NUE_ERR_INVALID_ARGUMENT;
    if (src_size > 0xFFFFFFFFull) return NUE_ERR_OVERFLOW;
    if (dst_capacity < nue_jdlz_compress_bound(src_size)) return NUE_ERR_BUFFER_TOO_SMALL;
    if (level <= 0) level = level == 0 ? 0 : 6;
    uint8_t* out = static_cast<uint8_t*>(dst);
    std::memcpy(out, "JDLZ", 4);
    out[4] = 2; out[5] = 0x10; out[6] = 0; out[7] = 0;
    wr32(out + 8, uint32_t(src_size));
    TokenWriter w(out);
    const uint8_t* d = static_cast<const uint8_t*>(src);
    if (src_size > 0) {
        if (level >= 7) parse_optimal(d, src_size, level, w);
        else parse_greedy(d, src_size, level, w);
    }
    const size_t total = w.finish();
    wr32(out + 12, uint32_t(total));
    if (out_written) *out_written = total;
    return NUE_OK;
}
