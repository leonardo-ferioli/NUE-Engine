// Transcode planner: source format + device caps + policy -> target format, deterministic, with an explanation.
#include "internal.hpp"
#include <cstdarg>

using namespace nuetex;

namespace {

enum class Family { Rgb, Rgba, Punch, R, Rg, Hdr, Uncompressed };

struct Ctx {
    const nue_tex_caps* caps;
    const nue_tex_plan_request* req;
    nue_tex_policy pol;
    Fmt src;
    Fmt base;               // sampling format of the source data (PMA variants -> their base)
    const nue_tex_format_info* si;
    bool want_srgb = false;
    uint32_t w, h;
    std::string log;
    uint32_t flags = 0;
    Fmt alternatives[6] = {};
    int nalt = 0;
};

void say(Ctx& c, const char* fmt, ...) NUE_PRINTF_LIKE(2, 3);
void say(Ctx& c, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); std::vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (!c.log.empty()) c.log += '\n';
    c.log += buf;
}

bool pot(uint32_t v) { return v && !(v & (v - 1)); }

bool usable(const Ctx& c, Fmt f) {
    if (!nue_tex_caps_can_use(c.caps, f)) return false;
    const auto* i = info(f);
    if (!i) return false;
    if ((i->flags & NUE_TEX_FINFO_REQUIRES_POT) && !(pot(c.w) && pot(c.h))) return false;
    if ((i->flags & NUE_TEX_FINFO_REQUIRES_SQUARE) && c.w != c.h) return false;
    if (i->kind == NUE_TEX_KIND_BLOCK && c.caps->block_size_must_align && (c.w % i->block_width || c.h % i->block_height)) return false;
    return true;
}

// Select the sRGB or linear twin of `f` according to the content colour space. Returns UNKNOWN if the GPU cannot do either.
Fmt with_colorspace(Ctx& c, Fmt f) {
    const auto* i = info(f);
    if (!i) return NUE_TEX_FORMAT_UNKNOWN;
    const Fmt lin = i->linear_twin, srgb = i->srgb_twin;
    const bool has_twin = lin != srgb;
    if (!has_twin) return usable(c, f) ? f : NUE_TEX_FORMAT_UNKNOWN;
    if (c.want_srgb) {
        if (usable(c, srgb) && (c.caps->format_flags[srgb] & NUE_TEX_FCAP_SRGB_SAMPLE)) return srgb;
        if (usable(c, lin)) { c.flags |= NUE_TEX_PLAN_SRGB_IN_SHADER; return lin; }
        return NUE_TEX_FORMAT_UNKNOWN;
    }
    return usable(c, lin) ? lin : NUE_TEX_FORMAT_UNKNOWN;
}

const float kAstcBpp[14] = {8.f, 6.4f, 5.12f, 4.267f, 3.556f, 3.2f, 2.667f, 2.0f, 2.56f, 2.133f, 1.6f, 1.28f, 1.067f, 0.889f};

Fmt astc_for_budget(Ctx& c, float budget_bpp, bool hdr, int max_block_index) {
    Fmt best = NUE_TEX_FORMAT_UNKNOWN;
    for (int i = 0; i <= max_block_index && i < 14; ++i) {
        if (kAstcBpp[i] > budget_bpp + 0.001f) continue;
        Fmt f = hdr ? Fmt(192 + i) : with_colorspace(c, Fmt(160 + i));
        if (hdr ? !usable(c, f) : f == NUE_TEX_FORMAT_UNKNOWN) continue;
        best = f; break;       // list is ordered by decreasing bits per pixel: first fit is the best quality inside the budget
    }
    if (best == NUE_TEX_FORMAT_UNKNOWN && budget_bpp < kAstcBpp[13]) {      // budget tighter than any block: the smallest we have
        for (int i = std::min(max_block_index, 13); i >= 0; --i) {
            Fmt f = hdr ? Fmt(192 + i) : with_colorspace(c, Fmt(160 + i));
            if (hdr ? usable(c, f) : f != NUE_TEX_FORMAT_UNKNOWN) { best = f; break; }
        }
    }
    return best;
}

Family family_of(const Ctx& c, nue_tex_alpha_info a) {
    const Fmt f = c.base;
    const auto* i = c.si;
    if (i->kind == NUE_TEX_KIND_UNCOMPRESSED) return (i->flags & NUE_TEX_FINFO_HDR) ? Family::Hdr : Family::Uncompressed;
    if (i->flags & NUE_TEX_FINFO_HDR) return Family::Hdr;
    switch (f) {
    case NUE_TEX_BC4_UNORM: case NUE_TEX_BC4_SNORM: case NUE_TEX_EAC_R11_UNORM: case NUE_TEX_EAC_R11_SNORM: return Family::R;
    case NUE_TEX_BC5_UNORM: case NUE_TEX_BC5_SNORM: case NUE_TEX_EAC_RG11_UNORM: case NUE_TEX_EAC_RG11_SNORM: return Family::Rg;
    default: break;
    }
    if (i->alpha == NUE_TEX_ALPHA_NONE) return Family::Rgb;
    if (a == NUE_TEX_ALPHA_INFO_OPAQUE) return Family::Rgb;
    if (a == NUE_TEX_ALPHA_INFO_UNKNOWN && i->alpha == NUE_TEX_ALPHA_PUNCHTHROUGH) return Family::Punch;
    if (a == NUE_TEX_ALPHA_INFO_BINARY) return Family::Punch;
    if (i->alpha == NUE_TEX_ALPHA_PUNCHTHROUGH && a != NUE_TEX_ALPHA_INFO_SMOOTH) return Family::Punch;
    return Family::Rgba;
}

float source_bpp(const Ctx& c) {
    return c.si->kind == NUE_TEX_KIND_BLOCK ? c.si->bits_per_pixel : 8.0f;
}

float tolerance(const Ctx& c) {
    if (c.pol.memory_tolerance > 0) return c.pol.memory_tolerance;
    switch (c.pol.priority) {
    case NUE_TEX_PRIORITY_QUALITY: return 2.0f;
    case NUE_TEX_PRIORITY_SPEED: return 2.0f;
    case NUE_TEX_PRIORITY_MEMORY: return 0.6f;
    default: return 1.1f;
    }
}

void add_candidate(std::vector<Fmt>& v, Fmt f) {
    if (f == NUE_TEX_FORMAT_UNKNOWN) return;
    for (Fmt x : v) if (x == f) return;
    v.push_back(f);
}

bool has_encoder(Fmt f) { const auto* i = info(f); return i && (i->flags & NUE_TEX_FINFO_HAS_ENCODER); }

// Candidate compressed targets in preference order for a family.
std::vector<Fmt> candidates(Ctx& c, Family fam, bool normal_map) {
    std::vector<Fmt> v;
    const auto P = c.pol.priority;
    const float tol = tolerance(c);
    float src_bpp = source_bpp(c);
    int max_block = 13;
    if (c.req->usage == NUE_TEX_USAGE_UI) max_block = 4;               // up to 6x6
    if (normal_map) max_block = std::min(max_block, 4);
    auto astc = [&](float bpp) { add_candidate(v, astc_for_budget(c, bpp, false, max_block)); };
    auto cand = [&](Fmt lin) { Fmt f = with_colorspace(c, lin); if (f != NUE_TEX_FORMAT_UNKNOWN && has_encoder(lin)) add_candidate(v, f); };
    if (c.pol.flags & NUE_TEX_POLICY_NO_LOSSY) return v;
    switch (fam) {
    case Family::Rgb:
        if (P == NUE_TEX_PRIORITY_QUALITY) { cand(NUE_TEX_BC7_UNORM); astc(std::max(8.0f, src_bpp * tol)); cand(NUE_TEX_ETC2_RGB8); cand(NUE_TEX_BC1_UNORM); }
        else if (P == NUE_TEX_PRIORITY_MEMORY) { astc(src_bpp * tol); cand(NUE_TEX_ETC2_RGB8); cand(NUE_TEX_BC1_UNORM); }
        else { cand(NUE_TEX_ETC2_RGB8); cand(NUE_TEX_BC1_UNORM); astc(src_bpp * tol); }
        cand(NUE_TEX_PVRTC1_4BPP);
        break;
    case Family::Rgba: case Family::Punch:
        if (P == NUE_TEX_PRIORITY_QUALITY) { cand(NUE_TEX_BC7_UNORM); astc(std::max(8.0f, src_bpp * tol)); cand(NUE_TEX_ETC2_RGBA8); cand(NUE_TEX_BC3_UNORM); }
        else if (P == NUE_TEX_PRIORITY_MEMORY) { astc(src_bpp * tol); cand(NUE_TEX_BC7_UNORM); cand(NUE_TEX_ETC2_RGBA8); cand(NUE_TEX_BC3_UNORM); }
        else if (P == NUE_TEX_PRIORITY_SPEED) { cand(NUE_TEX_ETC2_RGBA8); cand(NUE_TEX_BC3_UNORM); astc(src_bpp * tol); cand(NUE_TEX_BC7_UNORM); }
        else { astc(src_bpp * tol); cand(NUE_TEX_BC7_UNORM); cand(NUE_TEX_ETC2_RGBA8); cand(NUE_TEX_BC3_UNORM); }
        cand(NUE_TEX_PVRTC1_4BPP);
        break;
    case Family::R:
        cand(NUE_TEX_EAC_R11_UNORM); cand(NUE_TEX_BC4_UNORM); astc(src_bpp * tol);
        break;
    case Family::Rg:
        cand(NUE_TEX_EAC_RG11_UNORM); cand(NUE_TEX_BC5_UNORM);
        if (!(c.pol.flags & NUE_TEX_POLICY_NO_SHADER_SWIZZLE) && true) astc(std::max(src_bpp * tol, 0.0f));
        break;
    case Family::Hdr:
        if (c.caps->astc_hdr) add_candidate(v, astc_for_budget(c, std::max(src_bpp * tol, 3.5f), true, max_block));
        break;
    default: break;
    }
    return v;
}

// Uncompressed fallback chain.
Fmt uncompressed_fallback(Ctx& c, Family fam, bool alpha_needed, std::string& why) {
    const bool mem = c.pol.priority == NUE_TEX_PRIORITY_MEMORY && (c.pol.flags & NUE_TEX_POLICY_ALLOW_16BIT);
    std::vector<Fmt> order;
    if (fam == Family::Hdr) {
        order = {NUE_TEX_RG11B10_FLOAT, NUE_TEX_RGB9E5_FLOAT, NUE_TEX_RGBA16_FLOAT, NUE_TEX_RGBA8_UNORM};
        if (alpha_needed) order = {NUE_TEX_RGBA16_FLOAT, NUE_TEX_RGBA8_UNORM};
    } else if (fam == Family::R) order = {NUE_TEX_R8_UNORM, NUE_TEX_RG8_UNORM, NUE_TEX_RGBA8_UNORM};
    else if (fam == Family::Rg) order = {NUE_TEX_RG8_UNORM, NUE_TEX_RGBA8_UNORM};
    else if (mem && !alpha_needed) order = {NUE_TEX_RGB565, NUE_TEX_RGBA8_UNORM};
    else if (mem && c.req->alpha_info == NUE_TEX_ALPHA_INFO_BINARY) order = {NUE_TEX_RGB5A1, NUE_TEX_RGBA4, NUE_TEX_RGBA8_UNORM};
    else if (mem) order = {NUE_TEX_RGBA4, NUE_TEX_RGBA8_UNORM};
    else order = {NUE_TEX_RGBA8_UNORM, NUE_TEX_BGRA8_UNORM};
    for (Fmt f : order) {
        Fmt g = with_colorspace(c, f);
        if (g != NUE_TEX_FORMAT_UNKNOWN) { why = nue_tex_format_name(g); return g; }
        if (usable(c, f) && !info(f)->srgb_twin) { why = nue_tex_format_name(f); return f; }
    }
    // last resort: whatever uncompressed colour format the caps list
    for (Fmt f : {NUE_TEX_RGBA8_UNORM, NUE_TEX_BGRA8_UNORM}) if (usable(c, f)) return f;
    return NUE_TEX_FORMAT_UNKNOWN;
}

nue_tex_quality quality_for(const Ctx& c) {
    if (c.pol.quality) return nue_tex_quality(std::min<uint32_t>(c.pol.quality - 1, 4));
    switch (c.pol.priority) {
    case NUE_TEX_PRIORITY_QUALITY: return NUE_TEX_QUALITY_HIGH;
    case NUE_TEX_PRIORITY_SPEED: return NUE_TEX_QUALITY_FAST;
    default: return NUE_TEX_QUALITY_NORMAL;
    }
}

const char* family_name(Family f) {
    switch (f) {
    case Family::Rgb: return "opaque colour"; case Family::Rgba: return "colour with alpha"; case Family::Punch: return "colour with 1-bit alpha";
    case Family::R: return "single channel"; case Family::Rg: return "two channels"; case Family::Hdr: return "HDR"; default: return "uncompressed";
    }
}

void finish(Ctx& c, nue_tex_plan* out, Fmt target, nue_tex_action action, uint32_t skip, const char* swizzle) {
    out->source_format = c.src; out->target_format = target; out->action = action;
    out->skip_mips = skip;
    std::memcpy(out->shader_swizzle, swizzle, 5);
    if (std::strcmp(swizzle, "rgba") != 0) c.flags |= NUE_TEX_PLAN_NEEDS_SWIZZLE;
    if (skip) c.flags |= NUE_TEX_PLAN_MIPS_SKIPPED;
    out->flags = c.flags;
    const uint32_t layers = c.req->layers ? c.req->layers : 1, faces = c.req->faces ? c.req->faces : 1, depth = c.req->depth ? c.req->depth : 1;
    out->source_bytes = nue_tex_chain_size(c.src, c.req->width, c.req->height, depth, c.req->mip_levels, layers, faces);
    if (target != NUE_TEX_FORMAT_UNKNOWN) {
        const uint32_t levels = c.req->mip_levels ? c.req->mip_levels : nue_tex_mip_count(c.req->width, c.req->height, depth);
        uint64_t total = 0;
        for (uint32_t l = skip; l < levels; ++l)
            total += nue_tex_level_size(target, nue_tex_mip_extent(c.req->width, l), nue_tex_mip_extent(c.req->height, l), nue_tex_mip_extent(depth, l));
        out->target_bytes = total * layers * faces;
    }
    out->quality = quality_for(c);
    out->expected_psnr_db = (action == NUE_TEX_ACTION_UPLOAD_AS_IS || !(c.flags & NUE_TEX_PLAN_LOSSY)) ? 99.0f : 0.0f;
    for (int i = 0; i < c.nalt && i < 6; ++i) out->alternatives[i] = c.alternatives[i];
    std::snprintf(out->explanation, sizeof out->explanation, "%s", c.log.c_str());
}

}  // namespace

extern "C" {

void NUE_CALL nue_tex_policy_default(nue_tex_policy* p) {
    if (!p) return;
    std::memset(p, 0, sizeof *p);
    p->struct_size = sizeof *p;
    p->priority = NUE_TEX_PRIORITY_BALANCED;
    p->flags = NUE_TEX_POLICY_KEEP_SOURCE_FORMAT;
    p->min_recompress_dimension = 16;
}

const char* NUE_CALL nue_tex_action_name(nue_tex_action a) {
    switch (a) {
    case NUE_TEX_ACTION_UPLOAD_AS_IS: return "upload-as-is"; case NUE_TEX_ACTION_CONVERT_PIXELS: return "convert-pixels"; case NUE_TEX_ACTION_DECODE: return "decode";
    case NUE_TEX_ACTION_ENCODE: return "encode"; case NUE_TEX_ACTION_TRANSCODE: return "transcode"; default: return "unsupported";
    }
}

nue_result NUE_CALL nue_tex_make_plan(const nue_tex_caps* caps, const nue_tex_plan_request* req, nue_tex_plan* out) {
    if (!caps || !req || !out) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    std::memcpy(out->shader_swizzle, "rgba", 5);
    const auto* si = info(req->source_format);
    if (!si || si->kind == NUE_TEX_KIND_DEPTH || !req->width || !req->height) return NUE_ERR_INVALID_ARGUMENT;

    Ctx c;
    c.caps = caps; c.req = req; c.src = req->source_format; c.si = si;
    nue_tex_policy_default(&c.pol);
    if (req->policy) std::memcpy(&c.pol, req->policy, std::min<size_t>(req->policy->struct_size, sizeof c.pol));
    c.base = c.src == NUE_TEX_BC2_PMA_UNORM ? NUE_TEX_BC2_UNORM : c.src == NUE_TEX_BC3_PMA_UNORM ? NUE_TEX_BC3_UNORM : c.src;
    c.si = info(c.base);
    c.want_srgb = (req->flags & NUE_TEX_REQ_SRGB) || si->colorspace == NUE_TEX_COLORSPACE_SRGB;
    c.w = req->width; c.h = req->height;

    uint32_t skip = 0;
    if (c.pol.max_dimension && !(req->flags & NUE_TEX_REQ_NO_MIP_SKIP)) {
        const uint32_t levels = req->mip_levels ? req->mip_levels : nue_tex_mip_count(c.w, c.h, req->depth);
        while (skip + 1 < levels && std::max(c.w >> skip, c.h >> skip) > c.pol.max_dimension) ++skip;
        if (skip) say(c, "drops %u top mip levels so the longest side is at most %u", skip, c.pol.max_dimension);
    }
    const uint32_t cw = nue_tex_mip_extent(c.w, skip), ch = nue_tex_mip_extent(c.h, skip);
    c.w = cw; c.h = ch;       // usability checks (POT, alignment) are made on the level that becomes level 0

    const bool normal_map = req->usage == NUE_TEX_USAGE_NORMAL_MAP;
    const bool fixed_format = (req->flags & (NUE_TEX_REQ_RENDER_TARGET | NUE_TEX_REQ_DYNAMIC)) != 0;
    const Family fam = family_of(c, req->alpha_info);
    say(c, "source %s (%s, %.2f bpp), %ux%u, content: %s%s", nue_tex_format_name(c.src), c.si->kind == NUE_TEX_KIND_BLOCK ? "block compressed" : "uncompressed",
        c.si->bits_per_pixel, req->width, req->height, family_name(fam), c.want_srgb ? ", sRGB" : "");
    if (c.src != c.base) say(c, "premultiplied alpha is carried by the data; the GPU format is the same as %s", nue_tex_format_name(c.base));

    Ctx& cc = c;
    auto build_plan = [&](Fmt target, nue_tex_action action, const char* swizzle) { finish(cc, out, target, action, skip, swizzle); };

    // 1. forced uncompressed (reference rendering)
    if (c.pol.flags & NUE_TEX_POLICY_FORCE_UNCOMPRESSED) {
        std::string why;
        Fmt t = uncompressed_fallback(c, fam, fam == Family::Rgba || fam == Family::Punch, why);
        if (t == NUE_TEX_FORMAT_UNKNOWN) { say(c, "no uncompressed format is available"); build_plan(t, NUE_TEX_ACTION_UNSUPPORTED, "rgba"); return NUE_ERR_UNSUPPORTED; }
        say(c, "policy FORCE_UNCOMPRESSED: %s", why.c_str());
        build_plan(t, si->kind == NUE_TEX_KIND_BLOCK ? NUE_TEX_ACTION_DECODE : NUE_TEX_ACTION_CONVERT_PIXELS, "rgba");
        return NUE_OK;
    }

    // 2. the GPU can use the source as it is
    {
        Fmt direct = with_colorspace(c, c.base);
        const bool fixed_ok = !(fixed_format && si->kind == NUE_TEX_KIND_BLOCK);
        if (direct != NUE_TEX_FORMAT_UNKNOWN && fixed_ok && si->kind != NUE_TEX_KIND_DEPTH) {
            say(c, "%s is sampled and filtered natively by this device (%s): uploaded unchanged", nue_tex_format_name(direct), nue_tex_device_class_name(caps->device_class));
            if (skip) say(c, "top mips are skipped by dropping levels, no re-encode needed");
            build_plan(direct, NUE_TEX_ACTION_UPLOAD_AS_IS, "rgba");
            return NUE_OK;
        }
        if (direct == NUE_TEX_FORMAT_UNKNOWN) {
            const uint32_t fl = nue_tex_caps_get_format(caps, c.base);
            if (fl & NUE_TEX_FCAP_EMULATED) say(c, "%s is only emulated by the driver (decompressed at upload, no memory saved): not used", nue_tex_format_name(c.base));
            else say(c, "%s is not usable on %s", nue_tex_format_name(c.base), nue_tex_device_class_name(caps->device_class));
        }
    }

    // 3. uncompressed source the GPU cannot take as is, or that is worth compressing
    std::string why;
    const bool alpha_needed = fam == Family::Rgba || fam == Family::Punch || (fam == Family::Uncompressed && si->alpha != NUE_TEX_ALPHA_NONE && si->alpha != NUE_TEX_ALPHA_PADDING &&
                                                                                 req->alpha_info != NUE_TEX_ALPHA_INFO_OPAQUE);
    const bool small = std::max(c.w, c.h) < c.pol.min_recompress_dimension;
    if (fam == Family::Uncompressed || (fam == Family::Hdr && si->kind == NUE_TEX_KIND_UNCOMPRESSED)) {
        if ((c.pol.flags & NUE_TEX_POLICY_COMPRESS_RAW) && !fixed_format && !small && fam == Family::Uncompressed && !(c.pol.flags & NUE_TEX_POLICY_NO_LOSSY)) {
            Family cf = alpha_needed ? Family::Rgba : Family::Rgb;
            if (si->channels == 1) cf = Family::R; else if (si->channels == 2 && si->alpha == NUE_TEX_ALPHA_NONE) cf = Family::Rg;
            std::vector<Fmt> cands = candidates(c, cf, normal_map);
            if (!cands.empty()) {
                say(c, "COMPRESS_RAW: %s data is compressed to %s", nue_tex_format_name(c.src), nue_tex_format_name(cands[0]));
                for (size_t i = 1; i < cands.size() && c.nalt < 6; ++i) c.alternatives[c.nalt++] = cands[i];
                c.flags |= NUE_TEX_PLAN_LOSSY;
                if (cands[0] >= 160) c.flags |= NUE_TEX_PLAN_ENCODE_SLOW;
                const bool nm_astc = normal_map && is_astc(cands[0]);
                build_plan(cands[0], NUE_TEX_ACTION_ENCODE, nm_astc ? "rrrg" : "rgba");
                return NUE_OK;
            }
        }
        Fmt t = uncompressed_fallback(c, fam == Family::Hdr ? Family::Hdr : (si->channels == 1 ? Family::R : si->channels == 2 && si->alpha == NUE_TEX_ALPHA_NONE ? Family::Rg : Family::Rgba),
                                      alpha_needed, why);
        if (t == NUE_TEX_FORMAT_UNKNOWN) { say(c, "no sampleable uncompressed format"); build_plan(t, NUE_TEX_ACTION_UNSUPPORTED, "rgba"); return NUE_ERR_UNSUPPORTED; }
        say(c, "%s is not sampled directly: converted to %s", nue_tex_format_name(c.src), why.c_str());
        c.flags |= NUE_TEX_PLAN_FALLBACK;
        if (is_hdr(c.src) && !is_hdr(t)) c.flags |= NUE_TEX_PLAN_PRECISION_LOSS;
        if (si->bits_per_pixel > info(t)->bits_per_pixel) c.flags |= NUE_TEX_PLAN_PRECISION_LOSS;
        build_plan(t, NUE_TEX_ACTION_CONVERT_PIXELS, "rgba");
        return NUE_OK;
    }

    // 4. compressed source the GPU cannot use: transcode or decode
    if (fixed_format) say(c, "render target / dynamic texture: stays uncompressed");
    std::vector<Fmt> cands;
    if (!fixed_format && !small) cands = candidates(c, fam, normal_map);
    else if (small) say(c, "longest side %u < %u: not worth re-encoding", std::max(c.w, c.h), c.pol.min_recompress_dimension);
    if (c.pol.flags & NUE_TEX_POLICY_NO_LOSSY) say(c, "policy NO_LOSSY: no lossy re-encode");
    // snorm sources cannot go to the unsigned block formats
    if ((si->flags & NUE_TEX_FINFO_SIGNED) && !cands.empty()) { say(c, "signed data: unsigned block formats skipped"); cands.clear(); }
    // pick the first candidate; shader-swizzle only targets are dropped when the policy forbids swizzles
    for (Fmt t : cands) {
        const bool needs_swz = (fam == Family::Rg && is_astc(t));
        if (needs_swz && (c.pol.flags & NUE_TEX_POLICY_NO_SHADER_SWIZZLE)) { say(c, "%s skipped: needs a shader swizzle and the policy forbids it", nue_tex_format_name(t)); continue; }
        const auto* ti = info(t);
        say(c, "%s chosen for %s content (%.2f bpp vs %.2f, priority %d)", nue_tex_format_name(t), family_name(fam), ti->bits_per_pixel, c.si->bits_per_pixel, (int)c.pol.priority);
        for (Fmt o : cands) if (o != t && c.nalt < 6) c.alternatives[c.nalt++] = o;
        c.flags |= NUE_TEX_PLAN_LOSSY;
        if (fam == Family::Rgb && c.si->alpha == NUE_TEX_ALPHA_PUNCHTHROUGH && info(t)->alpha == NUE_TEX_ALPHA_NONE) {
            c.flags |= NUE_TEX_PLAN_ALPHA_DROPPED;
            say(c, "alpha dropped: the content scan found every texel opaque");
        }
        if (is_astc(t) && (t & 15) <= 5) c.flags |= NUE_TEX_PLAN_ENCODE_SLOW;
        if (t == NUE_TEX_BC7_UNORM || t == NUE_TEX_BC7_SRGB) c.flags |= NUE_TEX_PLAN_ENCODE_SLOW;
        const char* swz = "rgba";
        if (fam == Family::Rg && is_astc(t)) { swz = "rrrg"; say(c, "ASTC normal map layout: shader reads .ag (swizzle rrrg)"); }
        if (fam == Family::R && is_astc(t)) { swz = "rrr1"; say(c, "ASTC single channel: shader reads .r"); }
        if (c.base == NUE_TEX_BC1_UNORM && fam == Family::Punch && info(t)->alpha != NUE_TEX_ALPHA_NONE) say(c, "1-bit alpha is kept (target has alpha)");
        build_plan(t, NUE_TEX_ACTION_TRANSCODE, swz);
        return NUE_OK;
    }
    if (!cands.empty()) say(c, "no candidate fits the constraints");
    Fmt t = uncompressed_fallback(c, fam, fam == Family::Rgba || fam == Family::Punch, why);
    if (t == NUE_TEX_FORMAT_UNKNOWN) { say(c, "no uncompressed format is available"); build_plan(t, NUE_TEX_ACTION_UNSUPPORTED, "rgba"); return NUE_ERR_UNSUPPORTED; }
    say(c, "falls back to uncompressed %s (%.1fx the size of the source)", why.c_str(), info(t)->bits_per_pixel / std::max(0.01f, c.si->bits_per_pixel));
    c.flags |= NUE_TEX_PLAN_FALLBACK;
    if (is_hdr(c.src) && !is_hdr(t)) { c.flags |= NUE_TEX_PLAN_PRECISION_LOSS; say(c, "HDR range is lost (clamped to 0..1)"); }
    build_plan(t, NUE_TEX_ACTION_DECODE, "rgba");
    return NUE_OK;
}

}  // extern "C"
