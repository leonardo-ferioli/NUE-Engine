// GPU capability probing seam: the platform layer answers with real driver data; without it a conservative per-platform table is returned
// and marked probed = false, so planners (nue_texture, quality policies) know they are guessing and can re-plan once a device exists.
#include "internal/core_internal.h"

#include <cstring>

namespace {

const char* const kFamilyNames[NUE_FMT_FAMILY_COUNT] = {
    "RGBA8", "BGRA8", "RGB565", "RGBA4444", "RGB5A1", "R8", "RG8", "R16F", "RGBA16F", "RGBA32F", "RGB10A2", "DEPTH16", "DEPTH24_STENCIL8", "DEPTH32F",
    "DEPTH32F_STENCIL8", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "ETC1", "ETC2_RGB", "ETC2_RGBA", "EAC_R11", "EAC_RG11", "ASTC_LDR", "ASTC_HDR",
    "PVRTC1", "PVRTC2", "ATC"};
static_assert(sizeof(kFamilyNames) / sizeof(kFamilyNames[0]) == NUE_FMT_FAMILY_COUNT, "format family name table out of sync");

void assume_defaults(nue_gpu_caps* c) {
    const nue_format_support basic = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_FILTER | NUE_FMTSUP_MIPMAPS;
    const nue_format_support color_rt = basic | NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_BLEND | NUE_FMTSUP_SRGB;
    for (nue_format_family f : {NUE_FMT_RGBA8, NUE_FMT_BGRA8, NUE_FMT_R8, NUE_FMT_RG8, NUE_FMT_RGB10A2}) c->format[f] = color_rt;
    for (nue_format_family f : {NUE_FMT_R16F, NUE_FMT_RGBA16F}) c->format[f] = basic | NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_BLEND;
    c->format[NUE_FMT_RGBA32F] = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET;
    c->format[NUE_FMT_RGB565] = basic | NUE_FMTSUP_RENDER_TARGET;
    c->format[NUE_FMT_RGBA4444] = basic | NUE_FMTSUP_RENDER_TARGET;
    c->format[NUE_FMT_RGB5A1] = basic | NUE_FMTSUP_RENDER_TARGET;
    c->format[NUE_FMT_DEPTH16] = NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_SAMPLE;
    c->format[NUE_FMT_DEPTH32F] = NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_SAMPLE;
    c->format[NUE_FMT_DEPTH32F_STENCIL8] = NUE_FMTSUP_RENDER_TARGET;
#if defined(NUE_PLATFORM_WINDOWS) || defined(NUE_PLATFORM_LINUX) || defined(NUE_PLATFORM_MACOS)
    // desktop GPUs: block-compressed BC formats are universal on Windows/Linux; ETC2 and ASTC are not assumed
    for (nue_format_family f : {NUE_FMT_BC1, NUE_FMT_BC2, NUE_FMT_BC3, NUE_FMT_BC4, NUE_FMT_BC5, NUE_FMT_BC6H, NUE_FMT_BC7}) c->format[f] = basic | NUE_FMTSUP_SRGB;
    c->format[NUE_FMT_DEPTH24_STENCIL8] = NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_SAMPLE;
    c->max_texture_2d = 8192; c->max_texture_3d = 2048; c->max_texture_cube = 8192; c->max_array_layers = 256; c->max_anisotropy = 16; c->max_render_targets = 8; c->max_msaa_samples = 4;
    c->compute = NUE_TRUE; c->instancing = NUE_TRUE; c->npot_mipmaps = NUE_TRUE; c->texture_compare = NUE_TRUE;
#elif defined(NUE_PLATFORM_ANDROID)
    // Vulkan / GLES 3: ETC2 and EAC are mandatory; ASTC is common but optional (needs a probe); BC is not assumed
    for (nue_format_family f : {NUE_FMT_ETC1, NUE_FMT_ETC2_RGB, NUE_FMT_ETC2_RGBA, NUE_FMT_EAC_R11, NUE_FMT_EAC_RG11}) c->format[f] = basic | NUE_FMTSUP_SRGB;
    c->max_texture_2d = 4096; c->max_texture_3d = 256; c->max_texture_cube = 4096; c->max_array_layers = 256; c->max_anisotropy = 1; c->max_render_targets = 4; c->max_msaa_samples = 4;
    c->unified_memory = NUE_TRUE; c->instancing = NUE_TRUE; c->npot_mipmaps = NUE_TRUE;
#elif defined(NUE_PLATFORM_IOS)
    // Apple GPUs: no BC. ASTC LDR, ETC2/EAC and PVRTC are supported on every device Metal runs on.
    for (nue_format_family f : {NUE_FMT_ETC1, NUE_FMT_ETC2_RGB, NUE_FMT_ETC2_RGBA, NUE_FMT_EAC_R11, NUE_FMT_EAC_RG11, NUE_FMT_ASTC_LDR, NUE_FMT_PVRTC1}) c->format[f] = basic | NUE_FMTSUP_SRGB;
    c->max_texture_2d = 8192; c->max_texture_3d = 2048; c->max_texture_cube = 8192; c->max_array_layers = 2048; c->max_anisotropy = 16; c->max_render_targets = 8; c->max_msaa_samples = 4;
    c->unified_memory = NUE_TRUE; c->compute = NUE_TRUE; c->instancing = NUE_TRUE; c->npot_mipmaps = NUE_TRUE; c->texture_compare = NUE_TRUE;
#endif
}

}  // namespace

extern "C" {

NUE_API const char* NUE_CALL nue_format_family_name(nue_format_family f) { return (f >= 0 && f < NUE_FMT_FAMILY_COUNT) ? kFamilyNames[f] : "UNKNOWN"; }

NUE_API nue_result NUE_CALL nue_gpu_probe(nue_gpu_caps* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    nue_gpu_caps c;
    std::memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    c.renderer = NUE_RENDERER_AUTO;
    assume_defaults(&c);
    nue_result r = NUE_OK_PARTIAL;   // partial = assumptions only
    const nue_platform_ops* p = nuei::platform();
    auto probe = p ? NUE_PLATFORM_ENTRY(p, gpu_probe) : nullptr;
    if (probe) {
        nue_gpu_caps real;
        std::memset(&real, 0, sizeof(real));
        real.struct_size = sizeof(real);
        if (NUE_SUCCEEDED(probe(&real)) && real.probed) { c = real; c.struct_size = sizeof(c); r = NUE_OK; }
    }
    uint32_t keep = out->struct_size;
    std::memcpy(out, &c, keep < sizeof(c) ? keep : sizeof(c));
    out->struct_size = keep;
    return r;
}

}  // extern "C"
