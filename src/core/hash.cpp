// CRC-32 (IEEE, reflected, as zlib) and SHA-256 (FIPS 180-4), plus hex encoding.
#include "nue/core.h"

#include <cstring>

namespace {

uint32_t g_crc_table[256];
bool g_crc_ready = false;

void build_crc_table() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

struct Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t total;   // bytes
    uint32_t fill;
};
static_assert(sizeof(Sha256) <= sizeof(nue_sha256_state), "nue_sha256_state too small");

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha_block(Sha256& s, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}

}  // namespace

extern "C" {

NUE_API uint32_t NUE_CALL nue_crc32(uint32_t crc, const void* data, size_t size) {
    if (!g_crc_ready) build_crc_table();   // benign race: the table is idempotent
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = g_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

NUE_API void NUE_CALL nue_sha256_begin(nue_sha256_state* state) {
    Sha256& s = *reinterpret_cast<Sha256*>(state);
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(s.h, init, sizeof(init));
    s.total = 0;
    s.fill = 0;
}

NUE_API void NUE_CALL nue_sha256_update(nue_sha256_state* state, const void* data, size_t size) {
    Sha256& s = *reinterpret_cast<Sha256*>(state);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    s.total += size;
    if (s.fill) {
        size_t take = 64 - s.fill;
        if (take > size) take = size;
        std::memcpy(s.buf + s.fill, p, take);
        s.fill += uint32_t(take); p += take; size -= take;
        if (s.fill == 64) { sha_block(s, s.buf); s.fill = 0; }
    }
    while (size >= 64) { sha_block(s, p); p += 64; size -= 64; }
    if (size) { std::memcpy(s.buf, p, size); s.fill = uint32_t(size); }
}

NUE_API void NUE_CALL nue_sha256_end(nue_sha256_state* state, uint8_t out_digest[32]) {
    Sha256& s = *reinterpret_cast<Sha256*>(state);
    uint64_t bits = s.total * 8;
    uint8_t pad = 0x80;
    nue_sha256_update(state, &pad, 1);
    const uint8_t zero = 0;
    while (s.fill != 56) nue_sha256_update(state, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = uint8_t(bits >> (56 - 8 * i));
    nue_sha256_update(state, len, 8);
    for (int i = 0; i < 8; ++i) {
        out_digest[i * 4] = uint8_t(s.h[i] >> 24); out_digest[i * 4 + 1] = uint8_t(s.h[i] >> 16);
        out_digest[i * 4 + 2] = uint8_t(s.h[i] >> 8); out_digest[i * 4 + 3] = uint8_t(s.h[i]);
    }
}

NUE_API void NUE_CALL nue_sha256(const void* data, size_t size, uint8_t out_digest[32]) {
    nue_sha256_state st;
    nue_sha256_begin(&st);
    nue_sha256_update(&st, data, size);
    nue_sha256_end(&st, out_digest);
}

NUE_API void NUE_CALL nue_hex_encode(const uint8_t* data, size_t size, char* out, size_t capacity) {
    static const char digits[] = "0123456789abcdef";
    if (!out || !capacity) return;
    size_t n = 0;
    for (size_t i = 0; i < size && n + 2 < capacity; ++i) { out[n++] = digits[data[i] >> 4]; out[n++] = digits[data[i] & 15]; }
    out[n] = 0;
}

}  // extern "C"
