// Adapter: nue_gpu_caps (engine GPU probe, gfx.h) -> nue_tex_caps. Reads the struct only; no engine library is needed to link it.
#include "internal.hpp"
#include "nue/gfx.h"

using namespace nuetex;

namespace {

struct Family { nue_format_family family; std::vector<nue_tex_format> formats; };

const std::vector<Family>& families() {
    static const std::vector<Family> f = [] {
        std::vector<Family> v;
        auto add = [&](nue_format_family fam, std::initializer_list<nue_tex_format> l) { v.push_back({fam, std::vector<nue_tex_format>(l)}); };
        add(NUE_FMT_RGBA8, {NUE_TEX_RGBA8_UNORM, NUE_TEX_RGBA8_SRGB, NUE_TEX_RGBX8_UNORM});
        add(NUE_FMT_BGRA8, {NUE_TEX_BGRA8_UNORM, NUE_TEX_BGRA8_SRGB, NUE_TEX_BGRX8_UNORM, NUE_TEX_BGRX8_SRGB});
        add(NUE_FMT_RGB565, {NUE_TEX_RGB565}); add(NUE_FMT_RGBA4444, {NUE_TEX_RGBA4}); add(NUE_FMT_RGB5A1, {NUE_TEX_RGB5A1});
        add(NUE_FMT_R8, {NUE_TEX_R8_UNORM, NUE_TEX_R8_SNORM, NUE_TEX_A8_UNORM}); add(NUE_FMT_RG8, {NUE_TEX_RG8_UNORM, NUE_TEX_RG8_SNORM});
        add(NUE_FMT_R16F, {NUE_TEX_R16_FLOAT, NUE_TEX_RG16_FLOAT}); add(NUE_FMT_RGBA16F, {NUE_TEX_RGBA16_FLOAT});
        add(NUE_FMT_RGBA32F, {NUE_TEX_R32_FLOAT, NUE_TEX_RG32_FLOAT, NUE_TEX_RGB32_FLOAT, NUE_TEX_RGBA32_FLOAT});
        add(NUE_FMT_RGB10A2, {NUE_TEX_A2BGR10});
        add(NUE_FMT_BC1, {NUE_TEX_BC1_UNORM, NUE_TEX_BC1_SRGB}); add(NUE_FMT_BC2, {NUE_TEX_BC2_UNORM, NUE_TEX_BC2_SRGB, NUE_TEX_BC2_PMA_UNORM});
        add(NUE_FMT_BC3, {NUE_TEX_BC3_UNORM, NUE_TEX_BC3_SRGB, NUE_TEX_BC3_PMA_UNORM}); add(NUE_FMT_BC4, {NUE_TEX_BC4_UNORM, NUE_TEX_BC4_SNORM});
        add(NUE_FMT_BC5, {NUE_TEX_BC5_UNORM, NUE_TEX_BC5_SNORM}); add(NUE_FMT_BC6H, {NUE_TEX_BC6H_UFLOAT, NUE_TEX_BC6H_SFLOAT});
        add(NUE_FMT_BC7, {NUE_TEX_BC7_UNORM, NUE_TEX_BC7_SRGB});
        add(NUE_FMT_ETC1, {NUE_TEX_ETC1_RGB8});
        add(NUE_FMT_ETC2_RGB, {NUE_TEX_ETC2_RGB8, NUE_TEX_ETC2_RGB8_SRGB, NUE_TEX_ETC2_RGB8A1, NUE_TEX_ETC2_RGB8A1_SRGB});
        add(NUE_FMT_ETC2_RGBA, {NUE_TEX_ETC2_RGBA8, NUE_TEX_ETC2_RGBA8_SRGB});
        add(NUE_FMT_EAC_R11, {NUE_TEX_EAC_R11_UNORM, NUE_TEX_EAC_R11_SNORM}); add(NUE_FMT_EAC_RG11, {NUE_TEX_EAC_RG11_UNORM, NUE_TEX_EAC_RG11_SNORM});
        Family astc{NUE_FMT_ASTC_LDR, {}}, astch{NUE_FMT_ASTC_HDR, {}};
        for (int i = 0; i < 14; ++i) { astc.formats.push_back(Fmt(160 + i)); astc.formats.push_back(Fmt(176 + i)); astch.formats.push_back(Fmt(192 + i)); }
        v.push_back(astc); v.push_back(astch);
        add(NUE_FMT_PVRTC1, {NUE_TEX_PVRTC1_2BPP, NUE_TEX_PVRTC1_2BPP_SRGB, NUE_TEX_PVRTC1_4BPP, NUE_TEX_PVRTC1_4BPP_SRGB});
        add(NUE_FMT_PVRTC2, {NUE_TEX_PVRTC2_2BPP, NUE_TEX_PVRTC2_2BPP_SRGB, NUE_TEX_PVRTC2_4BPP, NUE_TEX_PVRTC2_4BPP_SRGB});
        add(NUE_FMT_ATC, {NUE_TEX_ATC_RGB, NUE_TEX_ATC_RGBA_EXPLICIT, NUE_TEX_ATC_RGBA_INTERPOLATED});
        return v;
    }();
    return f;
}

}  // namespace

extern "C" nue_result NUE_CALL nue_tex_caps_from_gpu_caps(nue_tex_caps* caps, const struct nue_gpu_caps* gpu) {
    if (!caps || !gpu) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t keep = caps->struct_size ? caps->struct_size : (uint32_t)sizeof *caps;
    nue_tex_caps_clear(caps);
    caps->struct_size = keep;
    for (const Family& fam : families()) {
        const nue_format_support s = gpu->format[fam.family];
        for (Fmt f : fam.formats) {
            const auto* fi = info(f);
            uint32_t fl = 0;
            if (s & NUE_FMTSUP_SAMPLE) fl |= NUE_TEX_FCAP_SAMPLE;
            if (s & NUE_FMTSUP_FILTER) fl |= NUE_TEX_FCAP_FILTER;
            if (s & NUE_FMTSUP_MIPMAPS) fl |= NUE_TEX_FCAP_MIP_AUTOGEN;
            if (s & NUE_FMTSUP_RENDER_TARGET) fl |= NUE_TEX_FCAP_RENDER;
            if (s & NUE_FMTSUP_MSAA) fl |= NUE_TEX_FCAP_MSAA;
            if (s & NUE_FMTSUP_EMULATED) fl |= NUE_TEX_FCAP_EMULATED;
            if (fi->colorspace == NUE_TEX_COLORSPACE_SRGB) { if (s & NUE_FMTSUP_SRGB) fl |= NUE_TEX_FCAP_SRGB_SAMPLE; else fl = 0; }
            if (fi->kind == NUE_TEX_KIND_BLOCK) fl &= ~uint32_t(NUE_TEX_FCAP_RENDER | NUE_TEX_FCAP_MSAA);
            caps->format_flags[f] = fl;
        }
    }
    caps->max_size_2d = gpu->max_texture_2d; caps->max_size_3d = gpu->max_texture_3d; caps->max_size_cube = gpu->max_texture_cube;
    caps->max_array_layers = gpu->max_array_layers; caps->max_anisotropy = gpu->max_anisotropy;
    caps->npot_full = caps->npot_with_mips = gpu->npot_mipmaps;
    caps->astc_hdr = (gpu->format[NUE_FMT_ASTC_HDR] & NUE_FMTSUP_SAMPLE) ? NUE_TRUE : NUE_FALSE;
    caps->astc_ldr_full_profile = NUE_TRUE;
    caps->supports_texture_swizzle = NUE_TRUE;
    caps->available_memory_bytes = gpu->dedicated_memory_bytes;
    const bool bc = (caps->format_flags[NUE_TEX_BC1_UNORM] & NUE_TEX_FCAP_SAMPLE) != 0, etc = (caps->format_flags[NUE_TEX_ETC2_RGB8] & NUE_TEX_FCAP_SAMPLE) != 0;
    const bool astc = (caps->format_flags[NUE_TEX_ASTC_4x4_UNORM] & NUE_TEX_FCAP_SAMPLE) != 0, pvr = (caps->format_flags[NUE_TEX_PVRTC1_4BPP] & NUE_TEX_FCAP_SAMPLE) != 0;
    caps->device_class = bc ? (astc ? (etc ? NUE_TEX_DEVICE_ANDROID_ETC2_BC : NUE_TEX_DEVICE_APPLE_SILICON_MAC) : (etc ? NUE_TEX_DEVICE_DESKTOP_BC_ETC2 : NUE_TEX_DEVICE_DESKTOP_BC))
                            : astc ? (pvr ? NUE_TEX_DEVICE_APPLE_ASTC : NUE_TEX_DEVICE_ANDROID_ASTC) : etc ? NUE_TEX_DEVICE_ANDROID_ETC2 : pvr ? NUE_TEX_DEVICE_APPLE_PVRTC : NUE_TEX_DEVICE_SOFTWARE;
    return NUE_OK;
}
