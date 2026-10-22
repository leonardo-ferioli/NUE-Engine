// Block codec glue: decoders (bcdec, etcpak, astcenc, Javelin PVRTC) and encoders (etcpak, bc7enc, astcenc, Javelin PVRTC) behind decode_blocks / encode_blocks.
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "bcdec.h"
#include "internal.hpp"
#include "astcenc.h"
#include "Decode.hpp"
#include "ProcessDxtc.hpp"
#include "ProcessRGB.hpp"
#include "bc7enc.h"
#include "PvrTcDecoder.h"
#include "PvrTcEncoder.h"
#include "RgbaBitmap.h"
#include <atomic>
#include <mutex>

void DecodeRG(const uint64_t* src, uint32_t* dst, int32_t width, int32_t height, bool isSigned);
void DecodeR(const uint64_t* src, uint32_t* dst, int32_t width, int32_t height, bool isSigned);

namespace nuetex {
namespace {

const uint8_t kAstcW[14] = {4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12};
const uint8_t kAstcH[14] = {4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12};

inline uint32_t round_up(uint32_t v, uint32_t m) { return (v + m - 1) / m * m; }
inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// ------------------------------------------------------------------------------------------------ ASTC contexts (one single-thread context per worker, cached)
struct AstcKey {
    int profile, bx, by; float quality; unsigned flags;
    bool operator==(const AstcKey& o) const { return profile == o.profile && bx == o.bx && by == o.by && quality == o.quality && flags == o.flags; }
};
struct AstcCache {
    struct Entry { AstcKey key; astcenc_context* ctx; uint64_t used; };
    std::vector<Entry> entries;
    uint64_t tick = 0;
    ~AstcCache() { for (auto& e : entries) astcenc_context_free(e.ctx); }
    astcenc_context* get(const AstcKey& k) {
        for (auto& e : entries) if (e.key == k) { e.used = ++tick; return e.ctx; }
        astcenc_config cfg;
        astcenc_error err = astcenc_config_init(astcenc_profile(k.profile), k.bx, k.by, 1, k.quality, k.flags, &cfg);
        if (err != ASTCENC_SUCCESS) { log(5, "astcenc_config_init %dx%d: %s", k.bx, k.by, astcenc_get_error_string(err)); return nullptr; }
        astcenc_context* ctx = nullptr;
        err = astcenc_context_alloc(&cfg, 1, &ctx, nullptr);
        if (err != ASTCENC_SUCCESS) { log(5, "astcenc_context_alloc: %s", astcenc_get_error_string(err)); return nullptr; }
        if (entries.size() >= 6) {
            size_t victim = 0;
            for (size_t i = 1; i < entries.size(); ++i) if (entries[i].used < entries[victim].used) victim = i;
            astcenc_context_free(entries[victim].ctx);
            entries.erase(entries.begin() + victim);
        }
        entries.push_back({k, ctx, ++tick});
        return ctx;
    }
};
AstcCache& astc_cache() { thread_local AstcCache c; return c; }

// astcenc effort levels: FASTEST/FAST are its presets; NORMAL is a 30 (between "fast" and "medium": "medium" costs ~8x the time of "fast" on this machine, see the
// benchmark in docs/TEXTURE.md), HIGH is "medium", BEST is "thorough".
float astc_quality(nue_tex_quality q) {
    switch (q) {
    case NUE_TEX_QUALITY_FASTEST: return ASTCENC_PRE_FASTEST;
    case NUE_TEX_QUALITY_FAST: return ASTCENC_PRE_FAST;
    case NUE_TEX_QUALITY_NORMAL: return 30.0f;
    case NUE_TEX_QUALITY_HIGH: return ASTCENC_PRE_MEDIUM;
    default: return ASTCENC_PRE_THOROUGH;
    }
}
astcenc_swz swz_of(char c) {
    switch (c) { case 'r': return ASTCENC_SWZ_R; case 'g': return ASTCENC_SWZ_G; case 'b': return ASTCENC_SWZ_B; case 'a': return ASTCENC_SWZ_A; case '0': return ASTCENC_SWZ_0; default: return ASTCENC_SWZ_1; }
}
bool native_float(Fmt f) { return f == NUE_TEX_BC6H_UFLOAT || f == NUE_TEX_BC6H_SFLOAT || (f >= 192 && f <= 205); }
bool is_bc123_7(Fmt f) {
    return f == NUE_TEX_BC1_UNORM || f == NUE_TEX_BC1_SRGB || f == NUE_TEX_BC2_UNORM || f == NUE_TEX_BC2_SRGB || f == NUE_TEX_BC2_PMA_UNORM || f == NUE_TEX_BC3_UNORM ||
           f == NUE_TEX_BC3_SRGB || f == NUE_TEX_BC3_PMA_UNORM || f == NUE_TEX_BC7_UNORM || f == NUE_TEX_BC7_SRGB;
}
bool is_pvrtc4(Fmt f) { return f == NUE_TEX_PVRTC1_4BPP || f == NUE_TEX_PVRTC1_4BPP_SRGB; }

// ------------------------------------------------------------------------------------------------ decoding
struct DecodeJob {
    Fmt f; uint32_t w, h, pw, bw, bh, bbytes, brow_bytes; const uint8_t* src; bool to_float; uint8_t* dst; size_t pitch;
    std::atomic<int> failed{0};
};

void store_band(DecodeJob& j, uint32_t y0, uint32_t band_h, const uint8_t* t8, const float* tf) {
    const bool srgb = is_srgb(j.f);
    for (uint32_t r = 0; r < band_h; ++r) {
        const uint32_t y = y0 + r;
        if (y >= j.h) break;
        if (t8) {
            const uint8_t* s = t8 + size_t(r) * j.pw * 4;
            if (!j.to_float) { std::memcpy(j.dst + y * j.pitch, s, size_t(j.w) * 4); continue; }
            float* o = reinterpret_cast<float*>(j.dst + y * j.pitch);
            for (uint32_t x = 0; x < j.w; ++x, o += 4) {
                for (int k = 0; k < 3; ++k) o[k] = srgb ? srgb_to_linear(s[x * 4 + k] / 255.0f) : s[x * 4 + k] / 255.0f;
                o[3] = s[x * 4 + 3] / 255.0f;
            }
        } else {
            const float* s = tf + size_t(r) * j.pw * 4;
            if (j.to_float) { std::memcpy(j.dst + y * j.pitch, s, size_t(j.w) * 16); continue; }
            uint8_t* o = j.dst + y * j.pitch;
            for (uint32_t x = 0; x < j.w * 4; ++x) { const float v = s[x]; o[x] = uint8_t((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f); }
        }
    }
}

void decode_band(DecodeJob& j, uint32_t r0, uint32_t r1) {
    const uint32_t rows = r1 - r0, band_h = rows * j.bh, bxn = j.pw / j.bw;
    const uint8_t* src = j.src + size_t(r0) * j.brow_bytes;
    const Fmt f = j.f;
    std::vector<uint8_t> tmp8;
    std::vector<float> tmpf;
    const bool nf = native_float(f);
    if (nf) tmpf.resize(size_t(j.pw) * band_h * 4); else tmp8.resize(size_t(j.pw) * band_h * 4);
    uint8_t* t8 = tmp8.data();
    float* tf = tmpf.data();
    const int pitch8 = int(j.pw * 4);

    if (is_bc123_7(f)) {
        for (uint32_t by = 0; by < rows; ++by)
            for (uint32_t bx = 0; bx < bxn; ++bx) {
                const void* blk = src + (size_t(by) * bxn + bx) * j.bbytes;
                uint8_t* d = t8 + size_t(by) * 4 * pitch8 + bx * 16;
                if (f == NUE_TEX_BC1_UNORM || f == NUE_TEX_BC1_SRGB) bcdec_bc1(blk, d, pitch8);
                else if (f == NUE_TEX_BC2_UNORM || f == NUE_TEX_BC2_SRGB || f == NUE_TEX_BC2_PMA_UNORM) bcdec_bc2(blk, d, pitch8);
                else if (f == NUE_TEX_BC7_UNORM || f == NUE_TEX_BC7_SRGB) bcdec_bc7(blk, d, pitch8);
                else bcdec_bc3(blk, d, pitch8);
            }
    } else if (f == NUE_TEX_BC4_UNORM || f == NUE_TEX_BC4_SNORM) {
        const bool sg = f == NUE_TEX_BC4_SNORM;
        for (uint32_t by = 0; by < rows; ++by)
            for (uint32_t bx = 0; bx < bxn; ++bx) {
                uint8_t t[16];
                bcdec_bc4(src + (size_t(by) * bxn + bx) * 8, t, 4, sg);
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        uint8_t* o = t8 + (size_t(by) * 4 + y) * pitch8 + (bx * 4 + x) * 4;
                        o[0] = sg ? uint8_t(int(int8_t(t[y * 4 + x])) + 128) : t[y * 4 + x]; o[1] = 0; o[2] = 0; o[3] = 255;
                    }
            }
    } else if (f == NUE_TEX_BC5_UNORM || f == NUE_TEX_BC5_SNORM) {
        const bool sg = f == NUE_TEX_BC5_SNORM;
        for (uint32_t by = 0; by < rows; ++by)
            for (uint32_t bx = 0; bx < bxn; ++bx) {
                uint8_t t[32];
                bcdec_bc5(src + (size_t(by) * bxn + bx) * 16, t, 8, sg);
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        uint8_t* o = t8 + (size_t(by) * 4 + y) * pitch8 + (bx * 4 + x) * 4;
                        const uint8_t* s = t + y * 8 + x * 2;
                        o[0] = sg ? uint8_t(int(int8_t(s[0])) + 128) : s[0]; o[1] = sg ? uint8_t(int(int8_t(s[1])) + 128) : s[1]; o[2] = 0; o[3] = 255;
                    }
            }
    } else if (f == NUE_TEX_BC6H_UFLOAT || f == NUE_TEX_BC6H_SFLOAT) {
        const bool sg = f == NUE_TEX_BC6H_SFLOAT;
        for (uint32_t by = 0; by < rows; ++by)
            for (uint32_t bx = 0; bx < bxn; ++bx) {
                float t[48];
                bcdec_bc6h_float(src + (size_t(by) * bxn + bx) * 16, t, 12, sg);      // pitch in floats: 4 pixels x 3
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        float* o = tf + ((size_t(by) * 4 + y) * j.pw + bx * 4 + x) * 4;
                        o[0] = t[(y * 4 + x) * 3]; o[1] = t[(y * 4 + x) * 3 + 1]; o[2] = t[(y * 4 + x) * 3 + 2]; o[3] = 1.0f;
                    }
            }
    } else if (f == NUE_TEX_ETC1_RGB8 || f == NUE_TEX_ETC2_RGB8 || f == NUE_TEX_ETC2_RGB8_SRGB) {
        DecodeRGB(reinterpret_cast<const uint64_t*>(src), reinterpret_cast<uint32_t*>(t8), int32_t(j.pw), int32_t(band_h));
    } else if (f == NUE_TEX_ETC2_RGBA8 || f == NUE_TEX_ETC2_RGBA8_SRGB) {
        DecodeRGBA(reinterpret_cast<const uint64_t*>(src), reinterpret_cast<uint32_t*>(t8), int32_t(j.pw), int32_t(band_h));
    } else if (f == NUE_TEX_EAC_R11_UNORM || f == NUE_TEX_EAC_R11_SNORM) {
        DecodeR(reinterpret_cast<const uint64_t*>(src), reinterpret_cast<uint32_t*>(t8), int32_t(j.pw), int32_t(band_h), f == NUE_TEX_EAC_R11_SNORM);
    } else if (f == NUE_TEX_EAC_RG11_UNORM || f == NUE_TEX_EAC_RG11_SNORM) {
        DecodeRG(reinterpret_cast<const uint64_t*>(src), reinterpret_cast<uint32_t*>(t8), int32_t(j.pw), int32_t(band_h), f == NUE_TEX_EAC_RG11_SNORM);
    } else if (is_astc(f)) {
        const int idx = astc_index(f);
        const bool hdr = f >= 192, sr = f >= 176 && f < 192;
        const AstcKey key{hdr ? ASTCENC_PRF_HDR : sr ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, kAstcW[idx], kAstcH[idx], ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY};
        astcenc_context* ctx = astc_cache().get(key);
        if (!ctx) { j.failed = 1; return; }
        astcenc_decompress_reset(ctx);
        void* slice = hdr ? static_cast<void*>(tf) : static_cast<void*>(t8);
        astcenc_image img{j.pw, band_h, 1, hdr ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8, &slice};
        const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        if (astcenc_decompress_image(ctx, src, size_t(rows) * j.brow_bytes, &img, &swz, 0) != ASTCENC_SUCCESS) { j.failed = 1; return; }
    } else {
        j.failed = 1;
        return;
    }
    store_band(j, r0 * j.bh, band_h, nf || (is_astc(f) && f >= 192) ? nullptr : t8, nf || (is_astc(f) && f >= 192) ? tf : nullptr);
}

nue_result decode_pvrtc4(Fmt f, uint32_t w, uint32_t h, const uint8_t* src, bool to_float, uint8_t* dst, size_t pitch) {
    const uint32_t pw = std::max(8u, round_up(w, 4)), ph = std::max(8u, round_up(h, 4));
    if ((pw & (pw - 1)) || (ph & (ph - 1)) || pw != ph) return NUE_ERR_UNSUPPORTED;      // the Javelin codec handles square power-of-two images
    std::vector<Javelin::ColorRgba<unsigned char>> px(size_t(pw) * ph);
    Javelin::PvrTcDecoder::DecodeRgba4Bpp(px.data(), Javelin::Point2<int>(int(pw), int(ph)), src);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* o8 = dst + y * pitch;
        float* of = reinterpret_cast<float*>(o8);
        for (uint32_t x = 0; x < w; ++x) {
            const auto& c = px[size_t(y) * pw + x];
            if (!to_float) { o8[x * 4] = c.r; o8[x * 4 + 1] = c.g; o8[x * 4 + 2] = c.b; o8[x * 4 + 3] = c.a; }
            else {
                const bool sr = is_srgb(f);
                of[x * 4] = sr ? srgb_to_linear(c.r / 255.f) : c.r / 255.f; of[x * 4 + 1] = sr ? srgb_to_linear(c.g / 255.f) : c.g / 255.f;
                of[x * 4 + 2] = sr ? srgb_to_linear(c.b / 255.f) : c.b / 255.f; of[x * 4 + 3] = c.a / 255.f;
            }
        }
    }
    return NUE_OK;
}

// ------------------------------------------------------------------------------------------------ encoding
void bc7_init_once() { static std::once_flag once; std::call_once(once, [] { bc7enc_compress_block_init(); }); }

struct EncodeJob {
    Fmt f; uint32_t pw, ph, bw, bh, bbytes, brow_bytes;
    const uint8_t* rgba8;           // padded pw x ph, R in the low byte (non-HDR targets)
    const float* rgbaf;             // padded float source for ASTC HDR
    uint8_t* dst;
    const nue_tex_encode_params* p;
    bc7enc_compress_block_params bc7p;
    int swizzle_astc[4];
    unsigned astc_flags;
    std::atomic<int> failed{0};
};

void swap_rb(uint32_t* p, size_t n) { for (size_t i = 0; i < n; ++i) { const uint32_t v = p[i]; p[i] = (v & 0xFF00FF00u) | ((v & 0xFFu) << 16) | ((v >> 16) & 0xFFu); } }

void encode_band(EncodeJob& j, uint32_t r0, uint32_t r1) {
    const uint32_t rows = r1 - r0, bxn = j.pw / j.bw, blocks = rows * bxn;
    uint8_t* dst = j.dst + size_t(r0) * j.brow_bytes;
    const Fmt f = j.f;
    if (is_astc(f)) {
        const int idx = astc_index(f);
        const bool hdr = f >= 192, sr = f >= 176 && f < 192;
        const AstcKey key{hdr ? ASTCENC_PRF_HDR : sr ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, kAstcW[idx], kAstcH[idx], astc_quality(j.p->quality), j.astc_flags};
        astcenc_context* ctx = astc_cache().get(key);
        if (!ctx) { j.failed = 1; return; }
        astcenc_compress_reset(ctx);
        const size_t off = size_t(r0) * j.bh * j.pw;
        void* slice = hdr ? static_cast<void*>(const_cast<float*>(j.rgbaf) + off * 4) : static_cast<void*>(const_cast<uint8_t*>(j.rgba8) + off * 4);
        astcenc_image img{j.pw, rows * j.bh, 1, hdr ? ASTCENC_TYPE_F32 : ASTCENC_TYPE_U8, &slice};
        const astcenc_swizzle swz{astcenc_swz(j.swizzle_astc[0]), astcenc_swz(j.swizzle_astc[1]), astcenc_swz(j.swizzle_astc[2]), astcenc_swz(j.swizzle_astc[3])};
        const astcenc_error err = astcenc_compress_image(ctx, &img, &swz, dst, size_t(rows) * j.brow_bytes, 0);
        if (err != ASTCENC_SUCCESS) { log(5, "astcenc_compress_image: %s", astcenc_get_error_string(err)); j.failed = 1; }
        return;
    }
    // etcpak encoders work on a whole image row of blocks at a time: give each band a private copy (also needed for the red/blue swap)
    std::vector<uint32_t> band(size_t(j.pw) * rows * 4);
    std::memcpy(band.data(), j.rgba8 + size_t(r0) * 4 * j.pw * 4, band.size() * 4);
    uint64_t* d = reinterpret_cast<uint64_t*>(dst);
    // etcpak's `useHeuristics` switches the T/H/planar mode search on: slower and clearly better (measured, see docs/TEXTURE.md), so it is the NORMAL+ setting
    const bool fast = j.p->quality >= NUE_TEX_QUALITY_NORMAL;
    switch (f) {
    case NUE_TEX_BC1_UNORM: case NUE_TEX_BC1_SRGB:
        if (j.p->flags & NUE_TEX_ENC_DITHER) CompressBc1Dither(band.data(), d, blocks, j.pw); else CompressBc1(band.data(), d, blocks, j.pw);
        break;
    case NUE_TEX_BC3_UNORM: case NUE_TEX_BC3_SRGB: case NUE_TEX_BC3_PMA_UNORM:
        CompressBc3(band.data(), d, blocks, j.pw);
        break;
    case NUE_TEX_BC4_UNORM: CompressBc4(band.data(), d, blocks, j.pw); break;
    case NUE_TEX_BC5_UNORM: CompressBc5(band.data(), d, blocks, j.pw); break;
    case NUE_TEX_BC7_UNORM: case NUE_TEX_BC7_SRGB: CompressBc7(band.data(), d, blocks, j.pw, &j.bc7p); break;
    case NUE_TEX_ETC1_RGB8:
        swap_rb(band.data(), band.size());
        if (j.p->flags & NUE_TEX_ENC_DITHER) CompressEtc1RgbDither(band.data(), d, blocks, j.pw); else CompressEtc1Rgb(band.data(), d, blocks, j.pw);
        break;
    case NUE_TEX_ETC2_RGB8: case NUE_TEX_ETC2_RGB8_SRGB: swap_rb(band.data(), band.size()); CompressEtc2Rgb(band.data(), d, blocks, j.pw, fast); break;
    case NUE_TEX_ETC2_RGBA8: case NUE_TEX_ETC2_RGBA8_SRGB: swap_rb(band.data(), band.size()); CompressEtc2Rgba(band.data(), d, blocks, j.pw, fast); break;
    case NUE_TEX_EAC_R11_UNORM: swap_rb(band.data(), band.size()); CompressEacR(band.data(), d, blocks, j.pw); break;
    case NUE_TEX_EAC_RG11_UNORM: swap_rb(band.data(), band.size()); CompressEacRg(band.data(), d, blocks, j.pw); break;
    default: j.failed = 1; break;
    }
}

}  // namespace

bool can_decode(Fmt f) {
    if (is_bc123_7(f) || f == NUE_TEX_BC4_UNORM || f == NUE_TEX_BC4_SNORM || f == NUE_TEX_BC5_UNORM || f == NUE_TEX_BC5_SNORM || f == NUE_TEX_BC6H_UFLOAT || f == NUE_TEX_BC6H_SFLOAT) return true;
    if (f == NUE_TEX_ETC1_RGB8 || f == NUE_TEX_ETC2_RGB8 || f == NUE_TEX_ETC2_RGB8_SRGB || f == NUE_TEX_ETC2_RGBA8 || f == NUE_TEX_ETC2_RGBA8_SRGB) return true;
    if (f >= NUE_TEX_EAC_R11_UNORM && f <= NUE_TEX_EAC_RG11_SNORM) return true;
    return is_astc(f) || is_pvrtc4(f);
}

bool can_encode(Fmt f) {
    switch (f) {
    case NUE_TEX_BC1_UNORM: case NUE_TEX_BC1_SRGB: case NUE_TEX_BC3_UNORM: case NUE_TEX_BC3_SRGB: case NUE_TEX_BC3_PMA_UNORM: case NUE_TEX_BC4_UNORM:
    case NUE_TEX_BC5_UNORM: case NUE_TEX_BC7_UNORM: case NUE_TEX_BC7_SRGB: case NUE_TEX_ETC1_RGB8: case NUE_TEX_ETC2_RGB8: case NUE_TEX_ETC2_RGB8_SRGB:
    case NUE_TEX_ETC2_RGBA8: case NUE_TEX_ETC2_RGBA8_SRGB: case NUE_TEX_EAC_R11_UNORM: case NUE_TEX_EAC_RG11_UNORM: case NUE_TEX_PVRTC1_4BPP: case NUE_TEX_PVRTC1_4BPP_SRGB:
        return true;
    default: return is_astc(f);
    }
}

nue_result decode_blocks(Fmt f, uint32_t w, uint32_t h, const void* src, bool to_float, void* dst, size_t dst_pitch, nue_tex_executor ex) {
    const auto* fi = info(f);
    if (!fi || !can_decode(f) || !w || !h) return NUE_ERR_UNSUPPORTED;
    if (is_pvrtc4(f)) return decode_pvrtc4(f, w, h, static_cast<const uint8_t*>(src), to_float, static_cast<uint8_t*>(dst), dst_pitch);
    DecodeJob j;
    j.f = f; j.w = w; j.h = h; j.bw = fi->block_width; j.bh = fi->block_height; j.bbytes = fi->block_bytes;
    j.pw = round_up(w, j.bw);
    j.brow_bytes = (j.pw / j.bw) * j.bbytes;
    j.src = static_cast<const uint8_t*>(src); j.to_float = to_float; j.dst = static_cast<uint8_t*>(dst); j.pitch = dst_pitch;
    const uint32_t brows = (h + j.bh - 1) / j.bh;
    const uint32_t chunk = std::max(1u, 65536u / (j.pw * j.bh));
    parallel_for(ex, brows, chunk, 0, [&](uint32_t b, uint32_t e) { decode_band(j, b, e); });
    return j.failed ? NUE_ERR_FAILED : NUE_OK;
}

nue_result encode_blocks(Fmt f, uint32_t w, uint32_t h, bool src_float, const void* src, size_t src_pitch, void* dst, const nue_tex_encode_params& p) {
    const auto* fi = info(f);
    if (!fi || !can_encode(f) || !w || !h) return NUE_ERR_UNSUPPORTED;
    const uint32_t bw = fi->block_width, bh = fi->block_height;
    uint32_t pw = round_up(w, bw), ph = round_up(h, bh);
    if (is_pvrtc4(f)) { pw = std::max(8u, pw); ph = std::max(8u, ph); }
    const bool hdr_target = f >= 192 && f <= 205;
    const bool srgb_target = is_srgb(f);

    // padded source, edge replicated
    std::vector<uint8_t> rgba8;
    std::vector<float> rgbaf;
    if (hdr_target) {
        rgbaf.resize(size_t(pw) * ph * 4);
        for (uint32_t y = 0; y < ph; ++y) {
            const uint32_t sy = std::min(y, h - 1);
            float* o = &rgbaf[size_t(y) * pw * 4];
            if (src_float) { const float* s = reinterpret_cast<const float*>(static_cast<const uint8_t*>(src) + sy * src_pitch); for (uint32_t x = 0; x < pw; ++x) std::memcpy(o + x * 4, s + std::min(x, w - 1) * 4, 16); }
            else { const uint8_t* s = static_cast<const uint8_t*>(src) + sy * src_pitch; for (uint32_t x = 0; x < pw; ++x) for (int k = 0; k < 4; ++k) o[x * 4 + k] = s[std::min(x, w - 1) * 4 + k] / 255.0f; }
        }
    } else {
        rgba8.resize(size_t(pw) * ph * 4);
        for (uint32_t y = 0; y < ph; ++y) {
            const uint32_t sy = std::min(y, h - 1);
            uint8_t* o = &rgba8[size_t(y) * pw * 4];
            if (src_float) {
                const float* s = reinterpret_cast<const float*>(static_cast<const uint8_t*>(src) + sy * src_pitch);
                for (uint32_t x = 0; x < pw; ++x)
                    for (int k = 0; k < 4; ++k) {
                        float v = s[std::min(x, w - 1) * 4 + k];
                        v = v < 0 ? 0 : v > 1 ? 1 : v;
                        if (srgb_target && k < 3) v = linear_to_srgb(v);
                        o[x * 4 + k] = uint8_t(v * 255.0f + 0.5f);
                    }
            } else {
                const uint8_t* s = static_cast<const uint8_t*>(src) + sy * src_pitch;
                std::memcpy(o, s, size_t(w) * 4);
                for (uint32_t x = w; x < pw; ++x) std::memcpy(o + x * 4, s + (w - 1) * 4, 4);
            }
        }
    }

    // channel swizzle (everything but ASTC, which swizzles inside the codec)
    char swz[5] = {p.swizzle[0] ? p.swizzle[0] : 'r', p.swizzle[1] ? p.swizzle[1] : 'g', p.swizzle[2] ? p.swizzle[2] : 'b', p.swizzle[3] ? p.swizzle[3] : 'a', 0};
    const bool user_swz = std::memcmp(swz, "rgba", 4) != 0;
    if (user_swz && !is_astc(f) && !rgba8.empty()) {
        for (size_t i = 0; i < size_t(pw) * ph; ++i) {
            uint8_t* px = &rgba8[i * 4];
            const uint8_t in[6] = {px[0], px[1], px[2], px[3], 0, 255};
            for (int k = 0; k < 4; ++k) { const char c = swz[k]; px[k] = in[c == 'r' ? 0 : c == 'g' ? 1 : c == 'b' ? 2 : c == 'a' ? 3 : c == '0' ? 4 : 5]; }
        }
    }

    if (is_pvrtc4(f)) {
        if ((pw & (pw - 1)) || (ph & (ph - 1)) || pw != ph) return NUE_ERR_UNSUPPORTED;
        Javelin::RgbaBitmap bmp{int(pw), int(ph)};
        auto* d = bmp.GetData();
        for (size_t i = 0; i < size_t(pw) * ph; ++i) { d[i].r = rgba8[i * 4]; d[i].g = rgba8[i * 4 + 1]; d[i].b = rgba8[i * 4 + 2]; d[i].a = rgba8[i * 4 + 3]; }
        Javelin::PvrTcEncoder::EncodeRgba4Bpp(dst, bmp);
        return NUE_OK;
    }

    EncodeJob j;
    j.f = f; j.pw = pw; j.ph = ph; j.bw = bw; j.bh = bh; j.bbytes = fi->block_bytes; j.brow_bytes = (pw / bw) * fi->block_bytes;
    j.rgba8 = rgba8.data(); j.rgbaf = rgbaf.data(); j.dst = static_cast<uint8_t*>(dst); j.p = &p;
    j.astc_flags = 0;
    if (is_astc(f)) {
        const bool normal = (p.flags & NUE_TEX_ENC_NORMAL_MAP) != 0;
        if (normal) { j.astc_flags |= ASTCENC_FLG_MAP_NORMAL; std::memcpy(swz, user_swz ? swz : "rrrg", 4); }
        else if (p.flags & NUE_TEX_ENC_PERCEPTUAL) j.astc_flags |= ASTCENC_FLG_USE_PERCEPTUAL;
        if (p.flags & NUE_TEX_ENC_ALPHA_WEIGHT) j.astc_flags |= ASTCENC_FLG_USE_ALPHA_WEIGHT;
        if (!hdr_target) j.astc_flags |= ASTCENC_FLG_USE_DECODE_UNORM8;
        for (int k = 0; k < 4; ++k) j.swizzle_astc[k] = swz_of(swz[k]);
    }
    if (f == NUE_TEX_BC7_UNORM || f == NUE_TEX_BC7_SRGB) {
        bc7_init_once();
        bc7enc_compress_block_params_init(&j.bc7p);
        if (p.flags & NUE_TEX_ENC_PERCEPTUAL) bc7enc_compress_block_params_init_perceptual_weights(&j.bc7p); else bc7enc_compress_block_params_init_linear_weights(&j.bc7p);
        switch (p.quality) {
        case NUE_TEX_QUALITY_FASTEST: j.bc7p.m_uber_level = 0; j.bc7p.m_max_partitions = 0; j.bc7p.m_try_least_squares = false; break;
        case NUE_TEX_QUALITY_FAST: j.bc7p.m_uber_level = 0; j.bc7p.m_max_partitions = 16; break;
        case NUE_TEX_QUALITY_NORMAL: j.bc7p.m_uber_level = 1; j.bc7p.m_max_partitions = 32; break;
        case NUE_TEX_QUALITY_HIGH: j.bc7p.m_uber_level = 2; j.bc7p.m_max_partitions = 64; break;
        default: j.bc7p.m_uber_level = 4; j.bc7p.m_max_partitions = 64; break;
        }
    }
    const uint32_t brows = ph / bh;
    const uint32_t chunk = std::max(1u, (is_astc(f) ? 4096u : 32768u) / (pw * bh));
    parallel_for(p.executor, brows, chunk, p.max_threads, [&](uint32_t b, uint32_t e) { encode_band(j, b, e); });
    return j.failed ? NUE_ERR_FAILED : NUE_OK;
}

}  // namespace nuetex
