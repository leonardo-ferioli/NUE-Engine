// Direct3D 9 seam: what the nue_d3d9 layer calls at CreateTexture / UnlockRect / UpdateTexture time (docs/TEXTURE.md, "Integration").
#include "context.hpp"

namespace nuetex {
namespace {

struct D3D9Tex {
    Context* c;
    nue_tex_d3d9_texture_desc desc;
    Fmt src;
    nue_tex_plan plan;
    nue_tex_d3d9_texture_info info;
    uint32_t levels;
    nue_tex_encode_params ep;
};

inline D3D9Tex* T(nue_tex_d3d9_texture t) { return reinterpret_cast<D3D9Tex*>(t); }
constexpr uint32_t kUsageRenderTarget = 0x1, kUsageDepthStencil = 0x2, kUsageDynamic = 0x200;

}  // namespace
}  // namespace nuetex

using namespace nuetex;

extern "C" {

nue_result NUE_CALL nue_tex_d3d9_texture_create(nue_tex_context cx, const nue_tex_d3d9_texture_desc* d, nue_tex_d3d9_texture* out_tex, nue_tex_d3d9_texture_info* out_info, nue_tex_plan* out_plan) {
    if (!cx || !d || !out_tex || !d->width || !d->height) return NUE_ERR_INVALID_ARGUMENT;
    nue_bool exact = NUE_FALSE;
    const Fmt src = nue_tex_format_from_d3d9(d->d3d_format, &exact);
    if (src == NUE_TEX_FORMAT_UNKNOWN) return NUE_ERR_UNSUPPORTED;
    Context& c = *ctx(cx);
    auto t = std::make_unique<D3D9Tex>();
    t->c = &c; t->desc = *d; t->src = src;
    t->levels = d->levels ? d->levels : nue_tex_mip_count(d->width, d->height, 1);
    NUE_STRUCT_INIT(&t->plan);
    NUE_STRUCT_INIT(&t->info);
    t->info.source_format = src;
    std::memcpy(t->info.shader_swizzle, "rgba", 5);

    const auto* si = info(src);
    const bool no_gpu = d->pool == 2 || d->pool == 3;
    if (si->kind == NUE_TEX_KIND_DEPTH || no_gpu) {
        // depth surfaces and system-memory textures keep their format: the layer handles them itself
        t->plan.source_format = t->plan.target_format = src;
        t->plan.action = NUE_TEX_ACTION_UPLOAD_AS_IS;
        std::snprintf(t->plan.explanation, sizeof t->plan.explanation, "%s: %s, kept as is", nue_tex_format_name(src), no_gpu ? "system memory pool (no GPU copy)" : "depth/stencil surface");
    } else {
        nue_tex_caps caps;
        snapshot_caps(c, &caps);
        nue_tex_plan_request rq; std::memset(&rq, 0, sizeof rq);
        rq.struct_size = sizeof rq;
        rq.source_format = src; rq.width = d->width; rq.height = d->height; rq.depth = 1; rq.mip_levels = t->levels;
        rq.layers = 1; rq.faces = d->cube ? 6 : 1;
        rq.usage = d->content; rq.alpha_info = d->alpha_hint;
        rq.flags = (d->srgb ? NUE_TEX_REQ_SRGB : 0u) | (d->cube ? NUE_TEX_REQ_CUBE : 0u);
        if (d->usage & (kUsageRenderTarget | kUsageDepthStencil)) rq.flags |= NUE_TEX_REQ_RENDER_TARGET | NUE_TEX_REQ_NO_MIP_SKIP;
        if (d->usage & kUsageDynamic) rq.flags |= NUE_TEX_REQ_DYNAMIC;
        rq.policy = &c.policy;
        const nue_result r = nue_tex_make_plan(&caps, &rq, &t->plan);
        if (r < 0) { if (out_plan) *out_plan = t->plan; return r; }
    }
    const nue_tex_plan& p = t->plan;
    t->info.gpu_format = p.target_format;
    t->info.action = p.action;
    t->info.skip_mips = p.skip_mips;
    t->info.gpu_levels = t->levels - p.skip_mips;
    t->info.gpu_width = nue_tex_mip_extent(d->width, p.skip_mips);
    t->info.gpu_height = nue_tex_mip_extent(d->height, p.skip_mips);
    t->info.gpu_bytes = nue_tex_chain_size(p.target_format, t->info.gpu_width, t->info.gpu_height, 1, t->info.gpu_levels, 1, d->cube ? 6 : 1);
    t->info.needs_shader_swizzle = (p.flags & NUE_TEX_PLAN_NEEDS_SWIZZLE) ? NUE_TRUE : NUE_FALSE;
    std::memcpy(t->info.shader_swizzle, p.shader_swizzle, 5);
    t->info.defer_to_worker = (p.flags & NUE_TEX_PLAN_ENCODE_SLOW) && (p.action == NUE_TEX_ACTION_ENCODE || p.action == NUE_TEX_ACTION_TRANSCODE) ? NUE_TRUE : NUE_FALSE;
    t->ep = encode_params_for(c, p, d->content, d->srgb ? NUE_TEX_REQ_SRGB : 0u);
    if (out_info) { const uint32_t keep = out_info->struct_size; *out_info = t->info; out_info->struct_size = keep ? keep : (uint32_t)sizeof t->info; }
    if (out_plan) { const uint32_t keep = out_plan->struct_size; *out_plan = t->plan; out_plan->struct_size = keep ? keep : (uint32_t)sizeof t->plan; }
    *out_tex = reinterpret_cast<nue_tex_d3d9_texture>(t.release());
    return NUE_OK;
}

void NUE_CALL nue_tex_d3d9_texture_destroy(nue_tex_d3d9_texture t) { delete T(t); }

nue_result NUE_CALL nue_tex_d3d9_texture_info_get(nue_tex_d3d9_texture t, nue_tex_d3d9_texture_info* out) {
    if (!t || !out) return NUE_ERR_INVALID_HANDLE;
    const uint32_t keep = out->struct_size;
    *out = T(t)->info;
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    return NUE_OK;
}

static bool rect_of(const D3D9Tex& t, uint32_t level, const nue_rect_i* r, uint32_t& x, uint32_t& y, uint32_t& w, uint32_t& h) {
    const uint32_t lw = nue_tex_mip_extent(t.desc.width, level), lh = nue_tex_mip_extent(t.desc.height, level);
    x = y = 0; w = lw; h = lh;
    if (!r) return true;
    if (r->x < 0 || r->y < 0 || r->w <= 0 || r->h <= 0) return false;
    x = uint32_t(r->x); y = uint32_t(r->y);
    if (x >= lw || y >= lh) return false;
    w = std::min<uint32_t>(uint32_t(r->w), lw - x); h = std::min<uint32_t>(uint32_t(r->h), lh - y);
    return true;
}

size_t NUE_CALL nue_tex_d3d9_level_gpu_size(nue_tex_d3d9_texture th, uint32_t level, const nue_rect_i* rect) {
    if (!th) return 0;
    const D3D9Tex& t = *T(th);
    if (level < t.info.skip_mips || level >= t.levels) return 0;
    uint32_t x, y, w, h;
    if (!rect_of(t, level, rect, x, y, w, h)) return 0;
    return size_t(nue_tex_level_size(t.info.gpu_format, w, h, 1));
}

nue_result NUE_CALL nue_tex_d3d9_convert_level(nue_tex_d3d9_texture th, uint32_t level, uint32_t face, const nue_rect_i* rect, const void* src, size_t src_pitch, void* dst,
                                                size_t dst_capacity, uint32_t* out_pitch, size_t* out_size) {
    if (!th || !src || !dst) return NUE_ERR_INVALID_ARGUMENT;
    D3D9Tex& t = *T(th);
    if (level >= t.levels || face >= (t.desc.cube ? 6u : 1u)) return NUE_ERR_INVALID_ARGUMENT;
    if (level < t.info.skip_mips) { if (out_size) *out_size = 0; return NUE_OK_NOOP; }
    uint32_t x, y, w, h;
    if (!rect_of(t, level, rect, x, y, w, h)) return NUE_ERR_INVALID_ARGUMENT;
    const auto* si = info(t.src);
    const auto* di = info(t.info.gpu_format);
    // block alignment of the source rectangle (Direct3D 9 itself requires it for DXT locks)
    if (si->kind == NUE_TEX_KIND_BLOCK && ((x % si->block_width) || (y % si->block_height))) return NUE_ERR_INVALID_ARGUMENT;
    // the destination rectangle must start on a destination block boundary; partial updates into a different block size need the whole level
    if (di->kind == NUE_TEX_KIND_BLOCK && ((x % di->block_width) || (y % di->block_height))) return NUE_ERR_UNSUPPORTED;
    const size_t gpu_size = size_t(nue_tex_level_size(t.info.gpu_format, w, h, 1));
    if (dst_capacity < gpu_size) return NUE_ERR_BUFFER_TOO_SMALL;
    // gather the source rows (rows of blocks) into a packed buffer
    const size_t row_bytes = nue_tex_row_pitch(t.src, w);
    const uint32_t rows = nue_tex_rows(t.src, h);
    if (src_pitch < row_bytes && rows > 1) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<uint8_t> packed(row_bytes * rows);
    for (uint32_t r = 0; r < rows; ++r) std::memcpy(&packed[r * row_bytes], static_cast<const uint8_t*>(src) + r * src_pitch, row_bytes);
    Context& c = *t.c;
    const bool lossy = t.info.action == NUE_TEX_ACTION_ENCODE || t.info.action == NUE_TEX_ACTION_TRANSCODE;
    if (lossy && !(c.flags & NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE)) {
        // Cold conversion: nothing is encoded now. The level is found by the content of the source bytes in the cache the import cooked.
        const uint32_t lw = nue_tex_mip_extent(t.desc.width, level), lh = nue_tex_mip_extent(t.desc.height, level);
        std::vector<uint8_t> cooked;
        if (x == 0 && y == 0 && w == lw && h == lh && lookup_level(c, level_hash(t.src, w, h, packed.data(), packed.size()), t.info.gpu_format, cooked) && cooked.size() == gpu_size) {
            std::memcpy(dst, cooked.data(), gpu_size);
            ++c.level_hits;
            if (out_pitch) *out_pitch = nue_tex_row_pitch(t.info.gpu_format, w);
            if (out_size) *out_size = gpu_size;
            return NUE_OK;
        }
        const uint64_t n = ++c.level_misses;
        if (n <= 20 || (n & (n - 1)) == 0) log(4, "d3d9 texture %ux%u level %u (%s) is not in the cold cache (%llu so far): placeholder", t.desc.width, t.desc.height, level, nue_tex_format_name(t.src), (unsigned long long)n);
        std::memset(dst, 0, gpu_size);
        if (out_pitch) *out_pitch = nue_tex_row_pitch(t.info.gpu_format, w);
        if (out_size) *out_size = gpu_size;
        return NUE_OK_PARTIAL;
    }
    if (lossy) ++c.runtime_encodes;
    nue_result r;
    if (t.info.gpu_format == t.src) { std::memcpy(dst, packed.data(), packed.size()); r = NUE_OK; }
    else {
        nue_tex_encode_params ep = t.ep;
        ep.executor = reinterpret_cast<nue_tex_executor>(t.c->executor);
        r = nue_tex_convert_level(t.src, t.info.gpu_format, w, h, packed.data(), packed.size(), dst, dst_capacity, &ep);
    }
    if (r < 0) return r;
    if (out_pitch) *out_pitch = nue_tex_row_pitch(t.info.gpu_format, w);
    if (out_size) *out_size = gpu_size;
    return NUE_OK;
}

}  // extern "C"
