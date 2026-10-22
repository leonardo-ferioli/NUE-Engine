// nue_tex_image: storage, level access, conversion between formats with mip preservation / generation.
#include "image.hpp"
#include <memory>
#include <new>

namespace nuetex {

namespace {
bool wide(Fmt f) {
    const auto* i = info(f);
    if (!i) return false;
    if (i->flags & NUE_TEX_FINFO_HDR) return true;
    for (int k = 0; k < 4; ++k) if (i->channel_bits[k] > 8) return true;
    return false;
}
}  // namespace

nue_result image_create(const nue_tex_image_desc& in, Image** out) {
    const auto* fi = info(in.format);
    if (!fi || fi->kind == NUE_TEX_KIND_DEPTH || !in.width || !in.height) return NUE_ERR_INVALID_ARGUMENT;
    auto im = std::make_unique<Image>();
    nue_tex_image_desc d = in;
    d.struct_size = sizeof d;
    if (!d.depth) d.depth = 1;
    if (!d.layers) d.layers = 1;
    if (!d.faces) d.faces = (d.flags & NUE_TEX_IMAGE_CUBE) ? 6 : 1;
    if (d.faces != 1 && d.faces != 6) return NUE_ERR_INVALID_ARGUMENT;
    if (d.faces == 6) d.flags |= NUE_TEX_IMAGE_CUBE;
    if (d.depth > 1 && (d.layers > 1 || d.faces > 1)) return NUE_ERR_INVALID_ARGUMENT;
    if (d.width > 65536 || d.height > 65536 || d.depth > 16384 || d.layers > 65536) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t full = nue_tex_mip_count(d.width, d.height, d.depth);
    if (!d.mip_levels) d.mip_levels = full;
    if (d.mip_levels > full) return NUE_ERR_INVALID_ARGUMENT;
    im->desc = d;
    const size_t n = size_t(d.layers) * d.faces * d.mip_levels;
    im->offsets.resize(n); im->sizes.resize(n);
    uint64_t total = 0;
    for (uint32_t layer = 0; layer < d.layers; ++layer)
        for (uint32_t face = 0; face < d.faces; ++face)
            for (uint32_t l = 0; l < d.mip_levels; ++l) {
                const uint64_t sz = nue_tex_level_size(d.format, im->w(l), im->h(l), im->d(l));
                im->offsets[im->index(l, layer, face)] = total;
                im->sizes[im->index(l, layer, face)] = sz;
                total += sz;
                if (total > (uint64_t(1) << 33)) return NUE_ERR_OUT_OF_MEMORY;
            }
    try { im->data.assign(size_t(total), 0); } catch (...) { return NUE_ERR_OUT_OF_MEMORY; }
    *out = im.release();
    return NUE_OK;
}

namespace {

// Tag-neutral float conversion of one slice: LDR sRGB-tagged formats keep their encoded values (see the colour space rules in docs/TEXTURE.md).
nue_result decode_slice_float(Fmt f, uint32_t w, uint32_t h, const void* data, size_t size, std::vector<float>& out, nue_tex_executor ex) {
    out.resize(size_t(w) * h * 4);
    nue_result r = nue_tex_decode_level(f, w, h, data, size, NUE_TEX_RGBA32_FLOAT, out.data(), out.size() * 4, ex);
    if (r < 0) return r;
    if (is_srgb(f) && !is_hdr(f)) for (size_t i = 0; i < size_t(w) * h; ++i) for (int k = 0; k < 3; ++k) out[i * 4 + k] = linear_to_srgb(std::min(1.f, std::max(0.f, out[i * 4 + k])));
    return NUE_OK;
}
nue_result encode_slice_float(Fmt f, uint32_t w, uint32_t h, std::vector<float>& px, void* dst, size_t dst_size, const nue_tex_encode_params& p) {
    if (is_srgb(f) && !is_hdr(f)) for (size_t i = 0; i < size_t(w) * h; ++i) for (int k = 0; k < 3; ++k) px[i * 4 + k] = srgb_to_linear(std::min(1.f, std::max(0.f, px[i * 4 + k])));
    return nue_tex_encode_level(f, w, h, NUE_TEX_RGBA32_FLOAT, px.data(), size_t(w) * 16, dst, dst_size, &p);
}

}  // namespace

nue_result image_convert(const Image& src, Fmt dst_format, uint32_t dst_levels, const nue_tex_encode_params* enc, const nue_tex_mip_params* mips, Image** out) {
    const auto* di = info(dst_format);
    if (!di || di->kind == NUE_TEX_KIND_DEPTH || info(src.desc.format)->kind == NUE_TEX_KIND_DEPTH) return NUE_ERR_UNSUPPORTED;
    nue_tex_encode_params ep;
    nue_tex_encode_params_default(&ep);
    if (enc) std::memcpy(&ep, enc, std::min<size_t>(enc->struct_size, sizeof ep));
    nue_tex_mip_params mp;
    nue_tex_mip_params_default(&mp);
    if (mips) std::memcpy(&mp, mips, std::min<size_t>(mips->struct_size, sizeof mp));
    if (!mp.executor) mp.executor = ep.executor;
    const uint32_t full = nue_tex_mip_count(src.desc.width, src.desc.height, src.desc.depth);
    uint32_t L = dst_levels;
    if (!L) L = src.levels() == 1 ? full : src.levels();
    L = std::min(L, full);
    nue_tex_image_desc d = src.desc;
    d.format = dst_format; d.mip_levels = L;
    Image* dst = nullptr;
    nue_result r = image_create(d, &dst);
    if (r < 0) return r;
    std::unique_ptr<Image> guard(dst);
    const bool fl_path = wide(src.desc.format) || wide(dst_format);
    (void)fl_path;
    const bool srgb_filter = (mp.flags & NUE_TEX_MIP_SRGB) != 0;
    const uint32_t existing = std::min(src.levels(), L);

    for (uint32_t layer = 0; layer < src.layers(); ++layer)
        for (uint32_t face = 0; face < src.faces(); ++face) {
            // 1. levels the source already has: converted one by one (source mips are preserved)
            for (uint32_t l = 0; l < existing; ++l) {
                const uint32_t w = src.w(l), h = src.h(l), dp = src.d(l);
                const uint64_t ss = nue_tex_level_size(src.desc.format, w, h, 1), ds = nue_tex_level_size(dst_format, w, h, 1);
                for (uint32_t z = 0; z < dp; ++z) {
                    r = nue_tex_convert_level(src.desc.format, dst_format, w, h, src.level_ptr(l, layer, face) + z * ss, size_t(ss), dst->level_ptr(l, layer, face) + z * ds, size_t(ds), &ep);
                    if (r < 0) return r;
                }
            }
            if (L <= existing) continue;
            // 2. missing levels are filtered from the last existing one
            FloatLevel prev;
            {
                const uint32_t l = existing - 1, w = src.w(l), h = src.h(l), dp = src.d(l);
                const uint64_t ss = nue_tex_level_size(src.desc.format, w, h, 1);
                prev.w = w; prev.h = h; prev.d = dp;
                prev.px.resize(size_t(w) * h * dp * 4);
                for (uint32_t z = 0; z < dp; ++z) {
                    std::vector<float> slice;
                    r = decode_slice_float(src.desc.format, w, h, src.level_ptr(l, layer, face) + z * ss, size_t(ss), slice, ep.executor);
                    if (r < 0) return r;
                    std::memcpy(&prev.px[size_t(z) * w * h * 4], slice.data(), slice.size() * 4);
                }
            }
            float coverage = -1.f;
            if (mp.flags & NUE_TEX_MIP_ALPHA_COVERAGE) {
                FloatLevel base;
                const uint32_t w = src.w(0), h = src.h(0);
                base.w = w; base.h = h; base.d = 1;
                std::vector<float> slice;
                r = decode_slice_float(src.desc.format, w, h, src.level_ptr(0, layer, face), size_t(src.level_size(0, layer, face)), slice, ep.executor);
                if (r < 0) return r;
                base.px.swap(slice);
                coverage = alpha_coverage(base, 0.5f);
            }
            for (uint32_t l = existing; l < L; ++l) {
                FloatLevel next;
                const uint32_t w = dst->w(l), h = dst->h(l), dp = dst->d(l);
                mip_downsample(prev, w, h, dp, mp, srgb_filter, next);
                if (coverage >= 0) mip_fix_alpha_coverage(next, coverage);
                const uint64_t ds = nue_tex_level_size(dst_format, w, h, 1);
                for (uint32_t z = 0; z < dp; ++z) {
                    std::vector<float> slice(next.px.begin() + size_t(z) * w * h * 4, next.px.begin() + size_t(z + 1) * w * h * 4);
                    r = encode_slice_float(dst_format, w, h, slice, dst->level_ptr(l, layer, face) + z * ds, size_t(ds), ep);
                    if (r < 0) return r;
                }
                prev = std::move(next);
            }
        }
    *out = guard.release();
    return NUE_OK;
}

}  // namespace nuetex

using namespace nuetex;

extern "C" {

nue_result NUE_CALL nue_tex_image_create(const nue_tex_image_desc* desc, nue_tex_image* out) {
    if (!desc || !out) return NUE_ERR_INVALID_ARGUMENT;
    Image* im = nullptr;
    const nue_result r = image_create(*desc, &im);
    *out = r >= 0 ? handle(im) : nullptr;
    return r;
}
void NUE_CALL nue_tex_image_destroy(nue_tex_image image) { delete img(image); }
nue_result NUE_CALL nue_tex_image_get_desc(nue_tex_image image, nue_tex_image_desc* out) {
    if (!image || !out) return NUE_ERR_INVALID_HANDLE;
    const uint32_t keep = out->struct_size;
    *out = img(image)->desc;
    out->struct_size = keep ? std::min<uint32_t>(keep, sizeof *out) : (uint32_t)sizeof *out;
    return NUE_OK;
}
nue_result NUE_CALL nue_tex_image_get_level(nue_tex_image image, uint32_t level, uint32_t layer, uint32_t face, nue_tex_level_view* out) {
    if (!image || !out) return NUE_ERR_INVALID_HANDLE;
    Image* im = img(image);
    if (level >= im->levels() || layer >= im->layers() || face >= im->faces()) return NUE_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->data = im->level_ptr(level, layer, face);
    out->size = im->level_size(level, layer, face);
    out->width = im->w(level); out->height = im->h(level); out->depth = im->d(level);
    const auto* fi = info(im->desc.format);
    out->blocks_x = (std::max<uint32_t>(out->width, fi->min_width) + fi->block_width - 1) / fi->block_width;
    out->blocks_y = nue_tex_rows(im->desc.format, out->height);
    out->row_pitch = nue_tex_row_pitch(im->desc.format, out->width);
    out->slice_pitch = uint64_t(out->row_pitch) * out->blocks_y;
    return NUE_OK;
}
uint64_t NUE_CALL nue_tex_image_total_size(nue_tex_image image) { return image ? img(image)->data.size() : 0; }
nue_result NUE_CALL nue_tex_image_clone(nue_tex_image image, nue_tex_image* out) {
    if (!image || !out) return NUE_ERR_INVALID_HANDLE;
    Image* c = new (std::nothrow) Image(*img(image));
    if (!c) return NUE_ERR_OUT_OF_MEMORY;
    *out = handle(c);
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_image_convert(nue_tex_image src, nue_tex_format dst_format, uint32_t dst_levels, const nue_tex_encode_params* encode, const nue_tex_mip_params* mips,
                                           nue_tex_image* out) {
    if (!src || !out) return NUE_ERR_INVALID_HANDLE;
    Image* o = nullptr;
    const nue_result r = image_convert(*img(src), dst_format, dst_levels, encode, mips, &o);
    *out = r >= 0 ? handle(o) : nullptr;
    return r;
}

nue_result NUE_CALL nue_tex_generate_mips(nue_tex_image src, const nue_tex_mip_params* params, nue_tex_image* out) {
    if (!src || !out) return NUE_ERR_INVALID_HANDLE;
    const Image& s = *img(src);
    if (info(s.desc.format)->kind != NUE_TEX_KIND_UNCOMPRESSED) return NUE_ERR_UNSUPPORTED;
    Image* o = nullptr;
    const uint32_t full = nue_tex_mip_count(s.desc.width, s.desc.height, s.desc.depth);
    const nue_result r = image_convert(s, s.desc.format, full, nullptr, params, &o);
    *out = r >= 0 ? handle(o) : nullptr;
    return r;
}

nue_result NUE_CALL nue_tex_image_drop_top_mips(nue_tex_image src, uint32_t count, nue_tex_image* out) {
    if (!src || !out) return NUE_ERR_INVALID_HANDLE;
    const Image& s = *img(src);
    if (count >= s.levels()) return NUE_ERR_INVALID_ARGUMENT;
    nue_tex_image_desc d = s.desc;
    d.width = s.w(count); d.height = s.h(count); d.depth = s.d(count); d.mip_levels = s.levels() - count;
    Image* o = nullptr;
    nue_result r = image_create(d, &o);
    if (r < 0) return r;
    for (uint32_t layer = 0; layer < s.layers(); ++layer)
        for (uint32_t face = 0; face < s.faces(); ++face)
            for (uint32_t l = 0; l < d.mip_levels; ++l)
                std::memcpy(o->level_ptr(l, layer, face), s.level_ptr(l + count, layer, face), size_t(s.level_size(l + count, layer, face)));
    *out = handle(o);
    return NUE_OK;
}

}  // extern "C"
