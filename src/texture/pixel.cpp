// Pixel pack / unpack for every uncompressed format, to and from RGBA8 (bytes as stored, sRGB stays encoded) and RGBA32F (linear).
#include "internal.hpp"

namespace nuetex {
namespace {

enum Kind : uint8_t { U8, S8, U16, S16, F16, F32, PK, SP_11_11_10, SP_9_9_9_5, NONE };

struct PD {
    Kind kind = NONE;
    uint8_t bytes = 0;
    int8_t c[4] = {-1, -1, -1, -1};      // array kinds: component index in memory for R,G,B,A (-1 = absent)
    uint8_t sh[4] = {0, 0, 0, 0}, bits[4] = {0, 0, 0, 0};   // packed kinds
    bool lum = false, srgb = false;
};

PD arr(Kind k, int bytes, int r, int g, int b, int a, bool srgb = false) {
    PD d; d.kind = k; d.bytes = uint8_t(bytes); d.c[0] = int8_t(r); d.c[1] = int8_t(g); d.c[2] = int8_t(b); d.c[3] = int8_t(a); d.srgb = srgb;
    d.lum = (r == g && g == b && r >= 0);
    return d;
}
PD pk(int bytes, int rs, int rb, int gs, int gb, int bs, int bb, int as, int ab) {
    PD d; d.kind = PK; d.bytes = uint8_t(bytes);
    d.sh[0] = uint8_t(rs); d.bits[0] = uint8_t(rb); d.sh[1] = uint8_t(gs); d.bits[1] = uint8_t(gb);
    d.sh[2] = uint8_t(bs); d.bits[2] = uint8_t(bb); d.sh[3] = uint8_t(as); d.bits[3] = uint8_t(ab);
    d.lum = d.sh[0] == d.sh[1] && d.sh[1] == d.sh[2] && d.bits[0] && d.bits[0] == d.bits[1];
    return d;
}

bool desc(Fmt f, PD& d) {
    switch (f) {
    case NUE_TEX_R8_UNORM: d = arr(U8, 1, 0, -1, -1, -1); return true;
    case NUE_TEX_RG8_UNORM: d = arr(U8, 2, 0, 1, -1, -1); return true;
    case NUE_TEX_RGB8_UNORM: d = arr(U8, 3, 0, 1, 2, -1); return true;
    case NUE_TEX_BGR8_UNORM: d = arr(U8, 3, 2, 1, 0, -1); return true;
    case NUE_TEX_RGBA8_UNORM: d = arr(U8, 4, 0, 1, 2, 3); return true;
    case NUE_TEX_BGRA8_UNORM: d = arr(U8, 4, 2, 1, 0, 3); return true;
    case NUE_TEX_RGBX8_UNORM: d = arr(U8, 4, 0, 1, 2, -1); return true;
    case NUE_TEX_BGRX8_UNORM: d = arr(U8, 4, 2, 1, 0, -1); return true;
    case NUE_TEX_A8_UNORM: d = arr(U8, 1, -1, -1, -1, 0); return true;
    case NUE_TEX_L8_UNORM: d = arr(U8, 1, 0, 0, 0, -1); return true;
    case NUE_TEX_LA8_UNORM: d = arr(U8, 2, 0, 0, 0, 1); return true;
    case NUE_TEX_L16_UNORM: d = arr(U16, 2, 0, 0, 0, -1); return true;
    case NUE_TEX_LA4_UNORM: d = pk(1, 0, 4, 0, 4, 0, 4, 4, 4); return true;
    case NUE_TEX_R8_SRGB: d = arr(U8, 1, 0, -1, -1, -1, true); return true;
    case NUE_TEX_RG8_SRGB: d = arr(U8, 2, 0, 1, -1, -1, true); return true;
    case NUE_TEX_RGB8_SRGB: d = arr(U8, 3, 0, 1, 2, -1, true); return true;
    case NUE_TEX_BGR8_SRGB: d = arr(U8, 3, 2, 1, 0, -1, true); return true;
    case NUE_TEX_RGBA8_SRGB: d = arr(U8, 4, 0, 1, 2, 3, true); return true;
    case NUE_TEX_BGRA8_SRGB: d = arr(U8, 4, 2, 1, 0, 3, true); return true;
    case NUE_TEX_BGRX8_SRGB: d = arr(U8, 4, 2, 1, 0, -1, true); return true;
    case NUE_TEX_R8_SNORM: d = arr(S8, 1, 0, -1, -1, -1); return true;
    case NUE_TEX_RG8_SNORM: d = arr(S8, 2, 0, 1, -1, -1); return true;
    case NUE_TEX_RGBA8_SNORM: d = arr(S8, 4, 0, 1, 2, 3); return true;
    case NUE_TEX_R16_UNORM: d = arr(U16, 2, 0, -1, -1, -1); return true;
    case NUE_TEX_RG16_UNORM: d = arr(U16, 4, 0, 1, -1, -1); return true;
    case NUE_TEX_RGBA16_UNORM: d = arr(U16, 8, 0, 1, 2, 3); return true;
    case NUE_TEX_R16_SNORM: d = arr(S16, 2, 0, -1, -1, -1); return true;
    case NUE_TEX_RG16_SNORM: d = arr(S16, 4, 0, 1, -1, -1); return true;
    case NUE_TEX_RGBA16_SNORM: d = arr(S16, 8, 0, 1, 2, 3); return true;
    case NUE_TEX_R16_FLOAT: d = arr(F16, 2, 0, -1, -1, -1); return true;
    case NUE_TEX_RG16_FLOAT: d = arr(F16, 4, 0, 1, -1, -1); return true;
    case NUE_TEX_RGBA16_FLOAT: d = arr(F16, 8, 0, 1, 2, 3); return true;
    case NUE_TEX_R32_FLOAT: d = arr(F32, 4, 0, -1, -1, -1); return true;
    case NUE_TEX_RG32_FLOAT: d = arr(F32, 8, 0, 1, -1, -1); return true;
    case NUE_TEX_RGB32_FLOAT: d = arr(F32, 12, 0, 1, 2, -1); return true;
    case NUE_TEX_RGBA32_FLOAT: d = arr(F32, 16, 0, 1, 2, 3); return true;
    case NUE_TEX_RGB565: d = pk(2, 11, 5, 5, 6, 0, 5, 0, 0); return true;
    case NUE_TEX_BGR565: d = pk(2, 0, 5, 5, 6, 11, 5, 0, 0); return true;
    case NUE_TEX_RGBA4: d = pk(2, 12, 4, 8, 4, 4, 4, 0, 4); return true;
    case NUE_TEX_BGRA4: d = pk(2, 4, 4, 8, 4, 12, 4, 0, 4); return true;
    case NUE_TEX_ARGB4: d = pk(2, 8, 4, 4, 4, 0, 4, 12, 4); return true;
    case NUE_TEX_XRGB4: d = pk(2, 8, 4, 4, 4, 0, 4, 0, 0); return true;
    case NUE_TEX_RGB5A1: d = pk(2, 11, 5, 6, 5, 1, 5, 0, 1); return true;
    case NUE_TEX_BGR5A1: d = pk(2, 1, 5, 6, 5, 11, 5, 0, 1); return true;
    case NUE_TEX_A1RGB5: d = pk(2, 10, 5, 5, 5, 0, 5, 15, 1); return true;
    case NUE_TEX_XRGB1555: d = pk(2, 10, 5, 5, 5, 0, 5, 0, 0); return true;
    case NUE_TEX_RGB332: d = pk(1, 5, 3, 2, 3, 0, 2, 0, 0); return true;
    case NUE_TEX_A8RGB332: d = pk(2, 5, 3, 2, 3, 0, 2, 8, 8); return true;
    case NUE_TEX_A2RGB10: d = pk(4, 20, 10, 10, 10, 0, 10, 30, 2); return true;
    case NUE_TEX_A2BGR10: d = pk(4, 0, 10, 10, 10, 20, 10, 30, 2); return true;
    case NUE_TEX_RG11B10_FLOAT: d = PD(); d.kind = SP_11_11_10; d.bytes = 4; return true;
    case NUE_TEX_RGB9E5_FLOAT: d = PD(); d.kind = SP_9_9_9_5; d.bytes = 4; return true;
    default: return false;
    }
}

inline float defv(int k) { return k == 3 ? 1.0f : 0.0f; }
inline uint32_t rd(const uint8_t* p, int bytes) { uint32_t v = 0; std::memcpy(&v, p, bytes); return v; }       // little endian hosts only (all targets)
inline void wr(uint8_t* p, int bytes, uint32_t v) { std::memcpy(p, &v, bytes); }
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
inline float clampm1(float v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

float f11_to_f(uint32_t v, int mant_bits) { return half_to_float(uint16_t(v << (10 - mant_bits))); }
uint32_t f_to_f11(float f, int mant_bits) {
    if (!(f > 0)) return 0;
    const uint16_t h = float_to_half(f);
    return std::min<uint32_t>(h >> (10 - mant_bits), (1u << (5 + mant_bits)) - 1);
}

void decode_px(const PD& d, const uint8_t* p, float v[4], bool linearize) {
    switch (d.kind) {
    case U8: case S8: case U16: case S16: case F16: case F32: {
        for (int k = 0; k < 4; ++k) {
            if (d.c[k] < 0) { v[k] = defv(k); continue; }
            const int i = d.c[k];
            switch (d.kind) {
            case U8: v[k] = p[i] * (1.0f / 255.0f); break;
            case S8: v[k] = clampm1(int8_t(p[i]) * (1.0f / 127.0f)); break;
            case U16: { uint16_t x; std::memcpy(&x, p + i * 2, 2); v[k] = x * (1.0f / 65535.0f); break; }
            case S16: { int16_t x; std::memcpy(&x, p + i * 2, 2); v[k] = clampm1(x * (1.0f / 32767.0f)); break; }
            case F16: { uint16_t x; std::memcpy(&x, p + i * 2, 2); v[k] = half_to_float(x); break; }
            default: std::memcpy(&v[k], p + i * 4, 4); break;
            }
        }
        break;
    }
    case PK: {
        const uint32_t w = rd(p, d.bytes);
        for (int k = 0; k < 4; ++k) {
            if (!d.bits[k]) { v[k] = defv(k); continue; }
            const uint32_t mask = (1u << d.bits[k]) - 1;
            v[k] = float((w >> d.sh[k]) & mask) / float(mask);
        }
        break;
    }
    case SP_11_11_10: { const uint32_t w = rd(p, 4); v[0] = f11_to_f(w & 0x7FF, 6); v[1] = f11_to_f((w >> 11) & 0x7FF, 6); v[2] = f11_to_f((w >> 22) & 0x3FF, 5); v[3] = 1; break; }
    case SP_9_9_9_5: {
        const uint32_t w = rd(p, 4);
        const float scale = std::ldexp(1.0f, int((w >> 27) & 31) - 24);
        v[0] = float(w & 511) * scale; v[1] = float((w >> 9) & 511) * scale; v[2] = float((w >> 18) & 511) * scale; v[3] = 1;
        break;
    }
    default: v[0] = v[1] = v[2] = 0; v[3] = 1; break;
    }
    if (d.lum && d.kind != PK) { /* c[] already replicates */ }
    if (d.srgb && linearize) for (int k = 0; k < 3; ++k) v[k] = srgb_to_linear(clamp01(v[k]));
}

void encode_px(const PD& d, const float in[4], uint8_t* p, bool from_linear) {
    float v[4] = {in[0], in[1], in[2], in[3]};
    if (d.srgb && from_linear) for (int k = 0; k < 3; ++k) v[k] = linear_to_srgb(clamp01(v[k]));
    switch (d.kind) {
    case U8: case S8: case U16: case S16: case F16: case F32: {
        float lumv = 0;
        if (d.lum) lumv = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        for (int k = 0; k < 4; ++k) {
            if (d.c[k] < 0) continue;
            if (d.lum && k < 3 && k > 0) continue;
            const float x = (d.lum && k == 0) ? lumv : v[k];
            const int i = d.c[k];
            switch (d.kind) {
            case U8: p[i] = uint8_t(clamp01(x) * 255.0f + 0.5f); break;
            case S8: p[i] = uint8_t(int8_t(std::lround(clampm1(x) * 127.0f))); break;
            case U16: { uint16_t o = uint16_t(clamp01(x) * 65535.0f + 0.5f); std::memcpy(p + i * 2, &o, 2); break; }
            case S16: { int16_t o = int16_t(std::lround(clampm1(x) * 32767.0f)); std::memcpy(p + i * 2, &o, 2); break; }
            case F16: { uint16_t o = float_to_half(x); std::memcpy(p + i * 2, &o, 2); break; }
            default: std::memcpy(p + i * 4, &x, 4); break;
            }
        }
        break;
    }
    case PK: {
        uint32_t w = 0;
        const float lumv = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        for (int k = 0; k < 4; ++k) {
            if (!d.bits[k]) continue;
            if (d.lum && k == 1) continue;
            if (d.lum && k == 2) continue;
            const uint32_t mask = (1u << d.bits[k]) - 1;
            const float x = (d.lum && k == 0) ? lumv : v[k];
            const uint32_t q = d.bits[k] == 1 && k == 3 ? (x >= 0.5f ? 1u : 0u) : uint32_t(clamp01(x) * float(mask) + 0.5f);
            w |= q << d.sh[k];
            if (d.lum && k == 0) { /* lum replicated: R,G,B share the same field */ }
        }
        wr(p, d.bytes, w);
        break;
    }
    case SP_11_11_10: wr(p, 4, f_to_f11(v[0], 6) | (f_to_f11(v[1], 6) << 11) | (f_to_f11(v[2], 5) << 22)); break;
    case SP_9_9_9_5: {
        const float maxv = 65408.0f;
        float r = std::min(std::max(v[0], 0.0f), maxv), g = std::min(std::max(v[1], 0.0f), maxv), b = std::min(std::max(v[2], 0.0f), maxv);
        const float m = std::max(r, std::max(g, b));
        int e = m > 0 ? std::max(-16, int(std::floor(std::log2(m)))) + 1 + 15 : 0;
        float denom = std::ldexp(1.0f, e - 15 - 9);
        int mm = int(std::floor(m / denom + 0.5f));
        if (mm == 512) { ++e; denom *= 2; }
        wr(p, 4, (uint32_t(std::floor(r / denom + 0.5f)) & 511) | ((uint32_t(std::floor(g / denom + 0.5f)) & 511) << 9) | ((uint32_t(std::floor(b / denom + 0.5f)) & 511) << 18) | (uint32_t(e) << 27));
        break;
    }
    default: break;
    }
}

}  // namespace

bool channel_layout(Fmt f, ChanInfo* out, int* n, bool* is_float, bool* is_signed, int* type_size) {
    PD d;
    if (!desc(f, d)) return false;
    *n = 0; *is_float = d.kind == F16 || d.kind == F32; *is_signed = d.kind == S8 || d.kind == S16; *type_size = 1;
    const int ids[4] = {0, 1, 2, 15};
    if (d.kind == PK) {
        *type_size = d.bytes;
        struct E { int shift, bits, id; } e[4]; int m = 0;
        if (d.lum) { e[m++] = {d.sh[0], d.bits[0], 0}; if (d.bits[3]) e[m++] = {d.sh[3], d.bits[3], 15}; }
        else for (int k = 0; k < 4; ++k) if (d.bits[k]) e[m++] = {d.sh[k], d.bits[k], ids[k]};
        std::sort(e, e + m, [](const E& a, const E& b) { return a.shift < b.shift; });
        for (int i = 0; i < m; ++i) out[(*n)++] = {uint8_t(e[i].id), uint8_t(e[i].shift), uint8_t(e[i].bits)};
        return true;
    }
    if (d.kind == SP_11_11_10) { out[(*n)++] = {0, 0, 11}; out[(*n)++] = {1, 11, 11}; out[(*n)++] = {2, 22, 10}; *type_size = 4; *is_float = true; return true; }
    if (d.kind == SP_9_9_9_5) { *type_size = 4; *is_float = true; return true; }
    const int comp_bytes = d.kind == U8 || d.kind == S8 ? 1 : d.kind == U16 || d.kind == S16 || d.kind == F16 ? 2 : 4;
    *type_size = comp_bytes;
    const int comps = d.bytes / comp_bytes;
    for (int i = 0; i < comps; ++i) {
        int id = -1;
        for (int k = 0; k < 4; ++k) if (d.c[k] == i) { id = ids[k]; break; }
        if (id < 0) continue;
        out[(*n)++] = {uint8_t(id), uint8_t(i * comp_bytes * 8), uint8_t(comp_bytes * 8)};
    }
    return true;
}

nue_result unpack_row(Fmt f, const void* src, uint32_t count, bool to_float, void* dst) {
    PD d;
    if (!desc(f, d)) return NUE_ERR_UNSUPPORTED;
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (to_float) {
        float* o = static_cast<float*>(dst);
        for (uint32_t i = 0; i < count; ++i, s += d.bytes, o += 4) decode_px(d, s, o, true);
        return NUE_OK;
    }
    uint8_t* o = static_cast<uint8_t*>(dst);
    if (d.kind == U8) {      // byte permutation fast path
        const int8_t c0 = d.c[0], c1 = d.c[1], c2 = d.c[2], c3 = d.c[3];
        for (uint32_t i = 0; i < count; ++i, s += d.bytes, o += 4) {
            o[0] = c0 >= 0 ? s[c0] : 0; o[1] = c1 >= 0 ? s[c1] : 0; o[2] = c2 >= 0 ? s[c2] : 0; o[3] = c3 >= 0 ? s[c3] : 255;
        }
        return NUE_OK;
    }
    for (uint32_t i = 0; i < count; ++i, s += d.bytes, o += 4) {
        float v[4];
        decode_px(d, s, v, false);
        if (d.kind == S8 || d.kind == S16)
            for (int k = 0; k < 4; ++k) v[k] = v[k] * 0.5f + 0.5f;        // signed data is stored biased in the RGBA8 intermediate
        for (int k = 0; k < 4; ++k) o[k] = uint8_t(clamp01(v[k]) * 255.0f + 0.5f);
    }
    return NUE_OK;
}

nue_result pack_row(Fmt f, const void* src, uint32_t count, bool from_float, void* dst) {
    PD d;
    if (!desc(f, d)) return NUE_ERR_UNSUPPORTED;
    uint8_t* o = static_cast<uint8_t*>(dst);
    if (from_float) {
        const float* s = static_cast<const float*>(src);
        for (uint32_t i = 0; i < count; ++i, s += 4, o += d.bytes) encode_px(d, s, o, true);
        return NUE_OK;
    }
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d.kind == U8 && !d.lum) {
        for (uint32_t i = 0; i < count; ++i, s += 4, o += d.bytes)
            for (int k = 0; k < 4; ++k) if (d.c[k] >= 0) o[d.c[k]] = s[k];
        return NUE_OK;
    }
    for (uint32_t i = 0; i < count; ++i, s += 4, o += d.bytes) {
        float v[4];
        for (int k = 0; k < 4; ++k) v[k] = s[k] * (1.0f / 255.0f);
        if (d.kind == S8 || d.kind == S16) for (int k = 0; k < 4; ++k) v[k] = v[k] * 2.0f - 1.0f;
        encode_px(d, v, o, false);         // bytes are taken as already encoded for the destination (sRGB bytes into sRGB formats)
    }
    return NUE_OK;
}

}  // namespace nuetex

extern "C" {

nue_result NUE_CALL nue_tex_unpack_pixels(nue_tex_format format, const void* src, uint32_t count, nue_tex_format dst_format, void* dst) {
    if (!src || !dst) return NUE_ERR_INVALID_ARGUMENT;
    if (dst_format != NUE_TEX_RGBA8_UNORM && dst_format != NUE_TEX_RGBA32_FLOAT) return NUE_ERR_INVALID_ARGUMENT;
    return nuetex::unpack_row(format, src, count, dst_format == NUE_TEX_RGBA32_FLOAT, dst);
}
nue_result NUE_CALL nue_tex_pack_pixels(nue_tex_format format, const void* src, uint32_t count, nue_tex_format src_format, void* dst) {
    if (!src || !dst) return NUE_ERR_INVALID_ARGUMENT;
    if (src_format != NUE_TEX_RGBA8_UNORM && src_format != NUE_TEX_RGBA32_FLOAT) return NUE_ERR_INVALID_ARGUMENT;
    return nuetex::pack_row(format, src, count, src_format == NUE_TEX_RGBA32_FLOAT, dst);
}

}
