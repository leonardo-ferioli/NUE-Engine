// Small shared helpers: logging, CRC32, SHA-256, colour curves, half floats, memory.
#include "internal.hpp"
#include <cstdarg>

namespace nuetex {

std::atomic<uint64_t> g_prof[P_COUNT];
std::atomic<uint64_t> g_enc_ns[NUE_TEX_FORMAT_COUNT], g_enc_px[NUE_TEX_FORMAT_COUNT];

namespace {
nue_tex_log_fn g_log_fn = nullptr;
void* g_log_user = nullptr;
}

void log(int level, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (g_log_fn) { g_log_fn(g_log_user, level, buf); return; }
    static const bool env = std::getenv("NUE_TEX_LOG") != nullptr;
    if (env) std::fprintf(stderr, "[texture:%d] %s\n", level, buf);
}

uint32_t crc32(uint32_t crc, const void* data, size_t size) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[i] = c; }
        return true;
    }();
    (void)init;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// SHA-256 (public domain algorithm, FIPS 180-4)
namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void compress(uint32_t s[8], const uint8_t* b) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t(b[i * 4]) << 24) | (uint32_t(b[i * 4 + 1]) << 16) | (uint32_t(b[i * 4 + 2]) << 8) | b[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s[0], bb = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22), mj = (a & bb) ^ (a & c) ^ (bb & c), t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = bb; bb = a; a = t1 + t2;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
}
}  // namespace

Sha256::Sha256() : len(0), used(0) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(s, init, sizeof s);
}
void Sha256::update(const void* data, size_t size) {
    g_prof[P_HASHED_BYTES].fetch_add(size, std::memory_order_relaxed);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    len += size;
    while (size) {
        if (used == 0 && size >= 64) { compress(s, p); p += 64; size -= 64; continue; }
        const size_t n = std::min<size_t>(64 - used, size);
        std::memcpy(buf + used, p, n);
        used += (uint32_t)n; p += n; size -= n;
        if (used == 64) { compress(s, buf); used = 0; }
    }
}
void Sha256::finish(uint8_t out[32]) {
    const uint64_t bits = len * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (used != 56) update(&zero, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (56 - 8 * i));
    update(l, 8);
    for (int i = 0; i < 8; ++i) { out[i * 4] = uint8_t(s[i] >> 24); out[i * 4 + 1] = uint8_t(s[i] >> 16); out[i * 4 + 2] = uint8_t(s[i] >> 8); out[i * 4 + 3] = uint8_t(s[i]); }
}
void sha256(const void* data, size_t size, uint8_t out[32]) { Sha256 h; h.update(data, size); h.finish(out); }

namespace {
inline uint64_t r64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline void mul128(uint64_t a, uint64_t b, uint64_t* hi, uint64_t* lo) {
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 p = (unsigned __int128)a * b;
    *lo = (uint64_t)p; *hi = (uint64_t)(p >> 64);
#else
    const uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    *lo = (p00 & 0xFFFFFFFFu) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}
inline uint64_t mix(uint64_t a, uint64_t b) { uint64_t hi, lo; mul128(a, b, &hi, &lo); return hi ^ lo; }
const uint64_t K0 = 0xa0761d6478bd642full, K1 = 0xe7037ed1a0b428dbull, K2 = 0x8ebc6af09c88c6e3ull, K3 = 0x589965cc75374cc3ull;
}  // namespace

Hash128 fast_hash128(const void* data, size_t size, uint64_t seed) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t s1 = seed ^ K0, s2 = ~seed ^ K1;
    size_t n = size;
    while (n >= 32) {
        s1 = mix(r64(p) ^ K1, r64(p + 8) ^ s1);
        s2 = mix(r64(p + 16) ^ K2, r64(p + 24) ^ s2);
        p += 32; n -= 32;
    }
    uint8_t tail[32] = {0};
    std::memcpy(tail, p, n);
    s1 = mix(r64(tail) ^ K1, r64(tail + 8) ^ s1 ^ uint64_t(size));
    s2 = mix(r64(tail + 16) ^ K2, r64(tail + 24) ^ s2);
    return Hash128{mix(s1 ^ K3, s2 ^ uint64_t(size)), mix(s2 ^ K0, s1 ^ K2)};
}

std::string hex128(const Hash128& h) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)h.a, (unsigned long long)h.b);
    return buf;
}

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

float half_to_float(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, man = h & 1023;
    uint32_t f;
    if (exp == 0) {
        if (man == 0) f = sign << 31;
        else { int e = -1; uint32_t m = man; do { ++e; m <<= 1; } while (!(m & 1024)); f = (sign << 31) | (uint32_t(127 - 15 - e) << 23) | ((m & 1023) << 13); }
    } else if (exp == 31) f = (sign << 31) | 0x7F800000u | (man << 13);
    else f = (sign << 31) | ((exp + 112) << 23) | (man << 13);
    float r; std::memcpy(&r, &f, 4); return r;
}
uint16_t float_to_half(float v) {
    uint32_t x; std::memcpy(&x, &v, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00u | (man ? 0x200u : 0));
    if (exp >= 31) return uint16_t(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);
        man |= 0x800000u;
        const uint32_t shift = uint32_t(14 - exp);
        uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1) ++h;
        return uint16_t(sign | h);
    }
    uint32_t h = (uint32_t(exp) << 10) | (man >> 13);
    if (man & 0x1000u) ++h;       // round to nearest (ties up; a carry correctly bumps the exponent)
    return uint16_t(sign | h);
}

}  // namespace nuetex

extern "C" {
void NUE_CALL nue_tex_profile_get(nue_tex_profile* out) {
    if (!out) return;
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    using namespace nuetex;
    out->alpha_scan_ns = g_prof[P_ALPHA]; out->plan_ns = g_prof[P_PLAN]; out->source_hash_ns = g_prof[P_SRCHASH]; out->cache_lookup_ns = g_prof[P_CACHE];
    out->drop_mips_ns = g_prof[P_DROP]; out->convert_ns = g_prof[P_CONVERT]; out->decode_ns = g_prof[P_DECODE]; out->encode_ns = g_prof[P_ENCODE]; out->mips_ns = g_prof[P_MIPS];
    out->level_hash_ns = g_prof[P_LEVELHASH]; out->store_ns = g_prof[P_STORE]; out->lookup_ns = g_prof[P_LOOKUP]; out->load_ns = g_prof[P_LOAD]; out->hashed_bytes = g_prof[P_HASHED_BYTES];
}
void NUE_CALL nue_tex_profile_reset(void) {
    for (auto& p : nuetex::g_prof) p = 0;
    for (auto& p : nuetex::g_enc_ns) p = 0;
    for (auto& p : nuetex::g_enc_px) p = 0;
}
void NUE_CALL nue_tex_profile_encode(nue_tex_format f, uint64_t* ns, uint64_t* px) {
    if ((uint32_t)f >= NUE_TEX_FORMAT_COUNT) return;
    if (ns) *ns = nuetex::g_enc_ns[f];
    if (px) *px = nuetex::g_enc_px[f];
}
void NUE_CALL nue_tex_set_log_callback(nue_tex_log_fn fn, void* userdata) {
    // single writer expected (startup); not synchronised with concurrent logging by design
    nuetex::g_log_fn = fn; nuetex::g_log_user = userdata;
}
void NUE_CALL nue_tex_free(void* p) { std::free(p); }
}
