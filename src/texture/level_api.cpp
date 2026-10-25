// Level-level API: decode / encode / convert one 2D level, content scan, codec info.
#define BCDEC_BC4BC5_PRECISE
#include "internal.hpp"
#include "bcdec.h"

using namespace nuetex;

namespace {

bool wide(Fmt f) {            // needs more than 8 bits per channel or is HDR/float: use the float intermediate
    const auto* i = info(f);
    if (!i) return false;
    if (i->flags & NUE_TEX_FINFO_HDR) return true;
    for (int k = 0; k < 4; ++k) if (i->channel_bits[k] > 8) return true;
    return false;
}

void default_params(nue_tex_encode_params& p) {
    nue_tex_encode_params_default(&p);
}

nue_tex_encode_params get_params(const nue_tex_encode_params* in) {
    nue_tex_encode_params p;
    default_params(p);
    if (in) std::memcpy(&p, in, std::min<size_t>(in->struct_size, sizeof p));
    return p;
}

}  // namespace

extern "C" {

void NUE_CALL nue_tex_encode_params_default(nue_tex_encode_params* p) {
    if (!p) return;
    std::memset(p, 0, sizeof *p);
    p->struct_size = sizeof *p;
    p->quality = NUE_TEX_QUALITY_NORMAL;
    p->alpha_threshold = 128;
    std::memcpy(p->swizzle, "rgba", 5);
}

nue_result NUE_CALL nue_tex_decode_level(nue_tex_format format, uint32_t w, uint32_t h, const void* src, size_t src_size, nue_tex_format dst_format, void* dst, size_t dst_size,
                                          nue_tex_executor ex) {
    const auto* fi = nue_tex_get_format_info(format);
    if (!fi || !src || !dst || !w || !h) return NUE_ERR_INVALID_ARGUMENT;
    if (dst_format != NUE_TEX_RGBA8_UNORM && dst_format != NUE_TEX_RGBA32_FLOAT) return NUE_ERR_INVALID_ARGUMENT;
    const bool fl = dst_format == NUE_TEX_RGBA32_FLOAT;
    const size_t px = fl ? 16 : 4;
    if (src_size < nue_tex_level_size(format, w, h, 1) || dst_size < size_t(w) * h * px) return NUE_ERR_BUFFER_TOO_SMALL;
    if (fi->kind == NUE_TEX_KIND_UNCOMPRESSED) {
        const size_t rp = nue_tex_row_pitch(format, w);
        for (uint32_t y = 0; y < h; ++y) {
            const nue_result r = unpack_row(format, static_cast<const uint8_t*>(src) + y * rp, w, fl, static_cast<uint8_t*>(dst) + y * w * px);
            if (r < 0) return r;
        }
        return NUE_OK;
    }
    if (fi->kind != NUE_TEX_KIND_BLOCK) return NUE_ERR_UNSUPPORTED;
    return decode_blocks(format, w, h, src, fl, dst, w * px, ex);
}

nue_result NUE_CALL nue_tex_encode_level(nue_tex_format format, uint32_t w, uint32_t h, nue_tex_format src_format, const void* src, size_t src_pitch, void* dst, size_t dst_size,
                                          const nue_tex_encode_params* params) {
    const auto* fi = nue_tex_get_format_info(format);
    if (!fi || !src || !dst || !w || !h) return NUE_ERR_INVALID_ARGUMENT;
    if (src_format != NUE_TEX_RGBA8_UNORM && src_format != NUE_TEX_RGBA32_FLOAT) return NUE_ERR_INVALID_ARGUMENT;
    const bool fl = src_format == NUE_TEX_RGBA32_FLOAT;
    if (dst_size < nue_tex_level_size(format, w, h, 1)) return NUE_ERR_BUFFER_TOO_SMALL;
    if (!src_pitch) src_pitch = size_t(w) * (fl ? 16 : 4);
    if (fi->kind == NUE_TEX_KIND_UNCOMPRESSED) {
        const size_t rp = nue_tex_row_pitch(format, w);
        for (uint32_t y = 0; y < h; ++y) {
            const nue_result r = pack_row(format, static_cast<const uint8_t*>(src) + y * src_pitch, w, fl, static_cast<uint8_t*>(dst) + y * rp);
            if (r < 0) return r;
        }
        return NUE_OK;
    }
    if (fi->kind != NUE_TEX_KIND_BLOCK) return NUE_ERR_UNSUPPORTED;
    const nue_tex_encode_params p = get_params(params);
    const auto t0 = std::chrono::steady_clock::now();
    const nue_result r = encode_blocks(format, w, h, fl, src, src_pitch, dst, p);
    g_enc_ns[format] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_enc_px[format] += uint64_t(w) * h;
    return r;
}

nue_result NUE_CALL nue_tex_convert_level(nue_tex_format sf, nue_tex_format df, uint32_t w, uint32_t h, const void* src, size_t src_size, void* dst, size_t dst_size,
                                           const nue_tex_encode_params* params) {
    const auto* si = nue_tex_get_format_info(sf);
    const auto* di = nue_tex_get_format_info(df);
    if (!si || !di || !src || !dst || !w || !h) return NUE_ERR_INVALID_ARGUMENT;
    const uint64_t ssz = nue_tex_level_size(sf, w, h, 1), dsz = nue_tex_level_size(df, w, h, 1);
    if (src_size < ssz || dst_size < dsz) return NUE_ERR_BUFFER_TOO_SMALL;
    if (sf == df) { std::memcpy(dst, src, size_t(ssz)); return NUE_OK; }
    if (si->kind == NUE_TEX_KIND_DEPTH || di->kind == NUE_TEX_KIND_DEPTH) return NUE_ERR_UNSUPPORTED;
    const nue_tex_encode_params p = get_params(params);
    const bool fl = wide(sf) || wide(df) || (di->flags & NUE_TEX_FINFO_HDR);
    const Fmt mid = fl ? NUE_TEX_RGBA32_FLOAT : NUE_TEX_RGBA8_UNORM;
    // the two formats differ only by the colour-space tag (BC1_UNORM <-> BC1_SRGB): the bytes are the same
    if (si->kind == di->kind && si->linear_twin == di->linear_twin && si->block_bytes == di->block_bytes) { std::memcpy(dst, src, size_t(ssz)); return NUE_OK; }
    const size_t pxs = fl ? 16 : 4;
    std::vector<uint8_t> tmp(size_t(w) * h * pxs);
    nue_result r;
    { ProfScope pd(P_DECODE); r = nue_tex_decode_level(sf, w, h, src, src_size, mid, tmp.data(), tmp.size(), p.executor); }
    if (r < 0) return r;
    ProfScope pe(P_ENCODE);
    return nue_tex_encode_level(df, w, h, mid, tmp.data(), size_t(w) * pxs, dst, dst_size, &p);
}

nue_result NUE_CALL nue_tex_scan_level(nue_tex_format format, uint32_t w, uint32_t h, const void* src, size_t src_size, nue_tex_scan_result* out) {
    const auto* fi = nue_tex_get_format_info(format);
    if (!fi || !src || !out || !w || !h) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    if (src_size < nue_tex_level_size(format, w, h, 1)) return NUE_ERR_BUFFER_TOO_SMALL;
    uint8_t mn = 255, mx = 0;
    bool binary = true, punch = false, any_alpha_info = true;
    auto alpha_byte = [&](uint8_t a) { mn = std::min(mn, a); mx = std::max(mx, a); if (a != 0 && a != 255) binary = false; };
    if (fi->alpha == NUE_TEX_ALPHA_NONE || fi->alpha == NUE_TEX_ALPHA_PADDING) {
        out->alpha_info = NUE_TEX_ALPHA_INFO_OPAQUE; out->min_alpha = out->max_alpha = 255;
        any_alpha_info = false;
    } else if (format == NUE_TEX_BC1_UNORM || format == NUE_TEX_BC1_SRGB) {
        const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
        const uint8_t* p = static_cast<const uint8_t*>(src);
        for (uint32_t i = 0; i < bx * by; ++i, p += 8) {
            uint16_t c0, c1; std::memcpy(&c0, p, 2); std::memcpy(&c1, p + 2, 2);
            if (c0 > c1) continue;                   // 4-colour mode: opaque
            uint32_t idx; std::memcpy(&idx, p + 4, 4);
            // is index 3 (transparent) used by a texel inside the image?
            const uint32_t x0 = (i % bx) * 4, y0 = (i / bx) * 4;
            for (uint32_t t = 0; t < 16; ++t)
                if (((idx >> (2 * t)) & 3) == 3 && x0 + (t & 3) < w && y0 + (t >> 2) < h) { punch = true; break; }
            if (punch) break;
        }
        alpha_byte(255); if (punch) alpha_byte(0);
    } else if (format == NUE_TEX_BC2_UNORM || format == NUE_TEX_BC2_SRGB || format == NUE_TEX_BC2_PMA_UNORM) {
        const uint32_t n = ((w + 3) / 4) * ((h + 3) / 4);
        const uint8_t* p = static_cast<const uint8_t*>(src);
        for (uint32_t i = 0; i < n; ++i, p += 16)
            for (int k = 0; k < 8; ++k) { alpha_byte(uint8_t((p[k] & 15) * 17)); alpha_byte(uint8_t((p[k] >> 4) * 17)); }
    } else if (format == NUE_TEX_BC3_UNORM || format == NUE_TEX_BC3_SRGB || format == NUE_TEX_BC3_PMA_UNORM) {
        const uint32_t n = ((w + 3) / 4) * ((h + 3) / 4);
        const uint8_t* p = static_cast<const uint8_t*>(src);
        for (uint32_t i = 0; i < n; ++i, p += 16) {
            uint8_t t[16];
            bcdec_bc4(p, t, 4, 0);
            for (int k = 0; k < 16; ++k) alpha_byte(t[k]);
        }
    } else {
        const size_t px = size_t(w) * h;
        std::vector<uint8_t> tmp(px * 4);
        const nue_result r = nue_tex_decode_level(format, w, h, src, src_size, NUE_TEX_RGBA8_UNORM, tmp.data(), tmp.size(), nullptr);
        if (r < 0) return r;
        for (size_t i = 0; i < px; ++i) alpha_byte(tmp[i * 4 + 3]);
        if (fi->alpha == NUE_TEX_ALPHA_PUNCHTHROUGH && mn == 0) punch = true;
    }
    if (any_alpha_info) {
        out->min_alpha = mn; out->max_alpha = mx;
        out->alpha_info = mn == 255 ? NUE_TEX_ALPHA_INFO_OPAQUE : binary ? NUE_TEX_ALPHA_INFO_BINARY : NUE_TEX_ALPHA_INFO_SMOOTH;
    }
    out->uses_punchthrough = punch ? NUE_TRUE : NUE_FALSE;
    if (fi->kind == NUE_TEX_KIND_UNCOMPRESSED) {
        std::vector<uint8_t> tmp(size_t(w) * h * 4);
        if (nue_tex_decode_level(format, w, h, src, src_size, NUE_TEX_RGBA8_UNORM, tmp.data(), tmp.size(), nullptr) >= 0) {
            bool same = true;
            for (size_t i = 4; i < tmp.size() && same; i += 4) same = std::memcmp(&tmp[i], tmp.data(), 4) == 0;
            out->constant_color = same ? NUE_TRUE : NUE_FALSE;
        }
    }
    return NUE_OK;
}

const char* NUE_CALL nue_tex_codec_info(void) {
    return "astc-encoder 5.7.0, etcpak 3d716e1, bcdec 0.985 (+bc7enc), pvrtc-javelin (bimg 9bfbd64), stb_image 2.30; simd " NUE_TEX_SIMD_NAME;
}

}  // extern "C"
