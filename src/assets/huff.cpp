// HUFF codec (EA's order-0 Huffman with a "clue" escape, magic "HUFF"): decompressor only. Some textures of the NFSU2 car packs use it.
//
// Stream: "HUFF", u8 version, u8 header size (0x10), u16 flags, u32 size of the decoded data, u32 size of the stream WITHOUT this header, then a bit stream
// read most significant bit first (equal to the bytes in order). The bit stream holds, in this order:
//   - a 16 bit type word (bit 15: 32 bit sizes follow, otherwise 24 bit; bit 8: two extra words to skip; the real files all carry 0x30FB), the decoded size
//     (24 or 32 bits) and an 8 bit clue symbol;
//   - the code length table: for lengths 1, 2, 3 ... a number (see getnum) saying how many symbols have that length, until the canonical code is complete;
//   - the symbol table: for each code in order, the symbol as a "leap" over the not yet used byte values (getnum + 1 free slots counted from the last symbol);
//   - the codes. The clue symbol escapes: getnum != 0 = repeat the previous byte that many times; 0 followed by a 1 bit = end; 0 followed by a 0 bit = a raw byte.
// getnum: p zero bits, a one bit, then p+2 bits of value v; the number is v + 2^(p+2) - 4.
// Types 0x32FB/0xB2FB and 0x34FB/0xB4FB ask for one or two running-sum passes over the output; the game files only use 0x30FB (none).
//
// The format knowledge follows the MIT/Apache-2.0 decoder of the PryHUB project (gizmo-nfs, see docs/THIRD_PARTY_ASSETS.md); this is an independent C++ rewrite.
#include "nue/assets.h"

#include "bytes.hpp"

#include <vector>

namespace {

constexpr int kMaxLen = 32;

class Bits {
public:
    Bits(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    // next n (<= 32) bits without consuming them; bits past the end read as zero
    uint32_t peek(unsigned n) const {
        uint64_t acc = 0;
        const size_t byte = size_t(pos_ >> 3);
        for (int i = 0; i < 5; ++i) acc = (acc << 8) | (byte + i < n_ ? d_[byte + i] : 0);
        acc <<= (pos_ & 7);
        return n == 0 ? 0 : uint32_t((acc >> (40 - n)) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1)));
    }
    void skip(unsigned n) { pos_ += n; }
    uint32_t get(unsigned n) { uint32_t v = peek(n); skip(n); return v; }
    bool past_end() const { return (pos_ >> 3) > n_ + 8; }
private:
    const uint8_t* d_;
    size_t n_;
    uint64_t pos_ = 0;
};

// The variable length number of the format; false when the stream is malformed (a zero run longer than any number).
bool getnum(Bits& b, int32_t* out) {
    unsigned p = 0;
    while (b.peek(1) == 0) {
        b.skip(1);
        if (++p > kMaxLen || b.past_end()) return false;
    }
    b.skip(1);                                           // the terminating one
    const unsigned n = p + 2;
    uint64_t raw = n > 16 ? (uint64_t(b.get(n - 16)) << 16) | b.get(16) : b.get(n);
    *out = int32_t(uint32_t(raw + (1ull << n) - 4));
    return true;
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_huff_identify(const void* data, size_t size, uint32_t* out_uncompressed, uint32_t* out_stream) {
    const uint8_t* d = static_cast<const uint8_t*>(data);
    if (!d || size < 16 || d[0] != 'H' || d[1] != 'U' || d[2] != 'F' || d[3] != 'F') return NUE_ERR_UNSUPPORTED;
    if (out_uncompressed) *out_uncompressed = nuea::rd32(d + 8);
    if (out_stream) *out_stream = nuea::rd32(d + 12) + 16;     // the stored size does not count the header
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_huff_decompress(const void* src, size_t src_size, void* dst, size_t dst_size, size_t* out_written) {
    if (out_written) *out_written = 0;
    uint32_t unc = 0, stream = 0;
    nue_result r = nue_huff_identify(src, src_size, &unc, &stream);
    if (r < 0) return r;
    if (unc > dst_size) return NUE_ERR_BUFFER_TOO_SMALL;
    if (!dst && unc) return NUE_ERR_INVALID_ARGUMENT;
    const uint8_t* in = static_cast<const uint8_t*>(src);
    const size_t avail = stream <= src_size ? stream : src_size;
    Bits b(in + 16, avail - 16);

    uint32_t type = b.get(16), ulen;
    if (type & 0x8000u) {
        if (type & 0x100u) { b.get(16); b.get(16); }
        type &= ~0x100u;
        const uint32_t hi = b.get(16), lo = b.get(16);
        ulen = (hi << 16) | lo;
    } else {
        if (type & 0x100u) { b.get(8); b.get(16); }
        type &= ~0x100u;
        const uint32_t hi = b.get(8), lo = b.get(16);
        ulen = (hi << 16) | lo;
    }
    const uint8_t clue = uint8_t(b.get(8));
    if (ulen != unc) return NUE_ERR_CORRUPT;

    // code lengths
    uint32_t delta[kMaxLen + 1] = {0}, cmp_tbl[kMaxLen + 2] = {0};
    int32_t numchars = 0;
    unsigned numbits = 1;
    uint32_t basecmp = 0;
    for (;;) {
        if (numbits > unsigned(kMaxLen)) return NUE_ERR_CORRUPT;
        basecmp <<= 1;
        delta[numbits] = basecmp - uint32_t(numchars);
        int32_t bn;
        if (!getnum(b, &bn) || bn < 0 || bn > 256) return NUE_ERR_CORRUPT;
        numchars += bn;
        basecmp += uint32_t(bn);
        const uint32_t cmp = bn != 0 ? ((basecmp << (16 - (numbits < 16 ? numbits : 16))) & 0xFFFFu) : 0;
        cmp_tbl[numbits] = cmp;
        ++numbits;
        if (bn == 0 || cmp != 0) continue;
        break;
    }
    const unsigned mostbits = numbits - 1;
    cmp_tbl[mostbits] = 0xFFFFFFFFu;
    if (numchars <= 0 || numchars > 256) return NUE_ERR_CORRUPT;

    // symbols
    uint8_t codetbl[256] = {0};
    bool used[256] = {false};
    uint8_t nextchar = 0xFF;
    for (int claimed = 0; claimed < numchars; ++claimed) {
        int32_t g;
        if (!getnum(b, &g)) return NUE_ERR_CORRUPT;
        const int64_t leap0 = int64_t(g) + 1;
        if (leap0 < 1 || leap0 > 256 - claimed) return NUE_ERR_CORRUPT;
        int64_t leap = leap0;
        for (;;) {
            nextchar = uint8_t(nextchar + 1);
            if (!used[nextchar]) --leap;
            if (leap == 0) break;
        }
        used[nextchar] = true;
        codetbl[claimed] = nextchar;
    }

    // codes
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t o = 0;
    uint64_t guard = 0;
    const uint64_t guard_max = uint64_t(ulen) * 4 + 4096;
    while (o < ulen) {
        if (++guard > guard_max || b.past_end()) return NUE_ERR_CORRUPT;
        const uint32_t cmp16 = b.peek(16);
        unsigned len = 1;
        while (len < mostbits && cmp16 >= cmp_tbl[len]) ++len;
        const uint32_t code_val = b.peek(len);
        b.skip(len);
        const uint32_t idx = code_val - delta[len];
        if (idx >= 256) return NUE_ERR_CORRUPT;
        const uint8_t code = codetbl[idx];
        if (code != clue) { out[o++] = code; continue; }
        int32_t run;
        if (!getnum(b, &run)) return NUE_ERR_CORRUPT;
        if (run != 0) {
            if (run < 0 || o == 0) return NUE_ERR_CORRUPT;
            const uint8_t prev = out[o - 1];
            for (int32_t i = 0; i < run && o < ulen; ++i) out[o++] = prev;
        } else if (b.get(1) != 0) {
            break;                                       // end of stream
        } else {
            out[o++] = uint8_t(b.get(8));
        }
    }
    if (o != ulen) return NUE_ERR_CORRUPT;
    if (type == 0x32FBu || type == 0xB2FBu) {
        uint32_t acc = 0;
        for (size_t i = 0; i < o; ++i) { acc += out[i]; out[i] = uint8_t(acc); }
    } else if (type == 0x34FBu || type == 0xB4FBu) {
        uint32_t acc = 0, acc2 = 0;
        for (size_t i = 0; i < o; ++i) { acc += out[i]; acc2 += acc; out[i] = uint8_t(acc2); }
    }
    if (out_written) *out_written = o;
    return NUE_OK;
}
