// Device capability model: per-format flags, presets per platform class, bgfx import, text dump.
#include "internal.hpp"

using namespace nuetex;

namespace {

const char* kClassNames[NUE_TEX_DEVICE_COUNT] = {"software", "desktop_bc", "desktop_bc_etc2", "android_astc", "android_etc2", "android_etc2_bc", "apple_astc",
                                                 "apple_pvrtc", "apple_silicon_mac"};

constexpr uint32_t TEX = NUE_TEX_FCAP_SAMPLE | NUE_TEX_FCAP_FILTER | NUE_TEX_FCAP_CUBE | NUE_TEX_FCAP_3D;
constexpr uint32_t TEXS = TEX | NUE_TEX_FCAP_SRGB_SAMPLE;
constexpr uint32_t RT = NUE_TEX_FCAP_RENDER | NUE_TEX_FCAP_MIP_AUTOGEN | NUE_TEX_FCAP_MSAA;

void set(nue_tex_caps* c, Fmt f, uint32_t flags) { c->format_flags[f] = flags; }

void add_base(nue_tex_caps* c, bool mobile) {
    for (Fmt f : {NUE_TEX_RGBA8_UNORM, NUE_TEX_BGRA8_UNORM}) set(c, f, TEX | RT);
    for (Fmt f : {NUE_TEX_RGBA8_SRGB, NUE_TEX_BGRA8_SRGB}) set(c, f, TEXS | RT);
    for (Fmt f : {NUE_TEX_R8_UNORM, NUE_TEX_RG8_UNORM}) set(c, f, TEX | RT);
    set(c, NUE_TEX_A8_UNORM, TEX);
    for (Fmt f : {NUE_TEX_R8_SNORM, NUE_TEX_RG8_SNORM, NUE_TEX_RGBA8_SNORM}) set(c, f, TEX);
    for (Fmt f : {NUE_TEX_R16_UNORM, NUE_TEX_RG16_UNORM, NUE_TEX_RGBA16_UNORM}) set(c, f, TEX | (mobile ? 0u : RT));
    for (Fmt f : {NUE_TEX_R16_FLOAT, NUE_TEX_RG16_FLOAT, NUE_TEX_RGBA16_FLOAT}) set(c, f, TEX | RT);
    for (Fmt f : {NUE_TEX_R32_FLOAT, NUE_TEX_RG32_FLOAT, NUE_TEX_RGBA32_FLOAT}) set(c, f, mobile ? (NUE_TEX_FCAP_SAMPLE | NUE_TEX_FCAP_RENDER) : (TEX | RT));
    set(c, NUE_TEX_RGB565, TEX | NUE_TEX_FCAP_RENDER);
    set(c, NUE_TEX_RGBA4, TEX | NUE_TEX_FCAP_RENDER);
    set(c, NUE_TEX_RGB5A1, TEX | NUE_TEX_FCAP_RENDER);
    set(c, NUE_TEX_A2BGR10, TEX | RT);
    set(c, NUE_TEX_RG11B10_FLOAT, TEX | RT);
    set(c, NUE_TEX_RGB9E5_FLOAT, TEX);
    for (Fmt f : {NUE_TEX_D16_UNORM, NUE_TEX_D24_UNORM_S8_UINT, NUE_TEX_D32_FLOAT}) set(c, f, NUE_TEX_FCAP_SAMPLE | NUE_TEX_FCAP_RENDER);
}
void add_bc(nue_tex_caps* c, uint32_t extra = 0) {
    for (Fmt f : {NUE_TEX_BC1_UNORM, NUE_TEX_BC2_UNORM, NUE_TEX_BC3_UNORM, NUE_TEX_BC7_UNORM, NUE_TEX_BC2_PMA_UNORM, NUE_TEX_BC3_PMA_UNORM}) set(c, f, TEX | extra);
    for (Fmt f : {NUE_TEX_BC1_SRGB, NUE_TEX_BC2_SRGB, NUE_TEX_BC3_SRGB, NUE_TEX_BC7_SRGB}) set(c, f, TEXS | extra);
    for (Fmt f : {NUE_TEX_BC4_UNORM, NUE_TEX_BC4_SNORM, NUE_TEX_BC5_UNORM, NUE_TEX_BC5_SNORM, NUE_TEX_BC6H_UFLOAT, NUE_TEX_BC6H_SFLOAT}) set(c, f, TEX | extra);
}
void add_etc2(nue_tex_caps* c, uint32_t extra = 0) {
    for (Fmt f : {NUE_TEX_ETC1_RGB8, NUE_TEX_ETC2_RGB8, NUE_TEX_ETC2_RGB8A1, NUE_TEX_ETC2_RGBA8, NUE_TEX_EAC_R11_UNORM, NUE_TEX_EAC_R11_SNORM,
                  NUE_TEX_EAC_RG11_UNORM, NUE_TEX_EAC_RG11_SNORM}) set(c, f, (TEX & ~NUE_TEX_FCAP_3D) | extra);
    for (Fmt f : {NUE_TEX_ETC2_RGB8_SRGB, NUE_TEX_ETC2_RGB8A1_SRGB, NUE_TEX_ETC2_RGBA8_SRGB}) set(c, f, (TEXS & ~NUE_TEX_FCAP_3D) | extra);
}
void add_astc(nue_tex_caps* c, bool hdr) {
    for (int i = 0; i < 14; ++i) {
        set(c, Fmt(160 + i), (TEX & ~NUE_TEX_FCAP_3D));
        set(c, Fmt(176 + i), (TEXS & ~NUE_TEX_FCAP_3D));
        if (hdr) set(c, Fmt(192 + i), (TEX & ~NUE_TEX_FCAP_3D));
    }
    c->astc_hdr = hdr ? NUE_TRUE : NUE_FALSE;
}
void add_pvrtc(nue_tex_caps* c) {
    for (Fmt f : {NUE_TEX_PVRTC1_2BPP, NUE_TEX_PVRTC1_4BPP}) set(c, f, TEX & ~NUE_TEX_FCAP_3D);
    for (Fmt f : {NUE_TEX_PVRTC1_2BPP_SRGB, NUE_TEX_PVRTC1_4BPP_SRGB}) set(c, f, TEXS & ~NUE_TEX_FCAP_3D);
}

}  // namespace

extern "C" {

void NUE_CALL nue_tex_caps_clear(nue_tex_caps* caps) {
    if (!caps) return;
    std::memset(caps, 0, sizeof *caps);
    caps->struct_size = sizeof *caps;
    caps->device_class = NUE_TEX_DEVICE_SOFTWARE;
    for (Fmt f : {NUE_TEX_RGBA8_UNORM, NUE_TEX_BGRA8_UNORM}) caps->format_flags[f] = TEX | RT;
    caps->max_size_2d = caps->max_size_3d = caps->max_size_cube = 4096;
    caps->max_array_layers = 1;
    caps->max_anisotropy = 1;
    caps->npot_full = caps->npot_with_mips = NUE_TRUE;
}

nue_result NUE_CALL nue_tex_caps_for_class(nue_tex_device_class cls, nue_tex_caps* c) {
    if (!c || (uint32_t)cls >= NUE_TEX_DEVICE_COUNT) return NUE_ERR_INVALID_ARGUMENT;
    nue_tex_caps_clear(c);
    c->device_class = cls;
    const bool mobile = cls >= NUE_TEX_DEVICE_ANDROID_ASTC && cls <= NUE_TEX_DEVICE_APPLE_PVRTC;
    if (cls != NUE_TEX_DEVICE_SOFTWARE) add_base(c, mobile);
    c->max_size_2d = c->max_size_cube = 16384; c->max_size_3d = 2048; c->max_array_layers = 2048; c->max_anisotropy = 16;
    c->npot_full = c->npot_with_mips = NUE_TRUE;
    c->supports_texture_swizzle = NUE_TRUE;
    c->astc_ldr_full_profile = NUE_TRUE;
    switch (cls) {
    case NUE_TEX_DEVICE_SOFTWARE: c->max_anisotropy = 1; c->max_array_layers = 1; break;
    case NUE_TEX_DEVICE_DESKTOP_BC: add_bc(c); break;
    case NUE_TEX_DEVICE_DESKTOP_BC_ETC2: add_bc(c); add_etc2(c, NUE_TEX_FCAP_EMULATED); break;
    case NUE_TEX_DEVICE_ANDROID_ASTC: add_etc2(c); add_astc(c, false); c->max_size_2d = c->max_size_cube = 8192; c->max_size_3d = 512; c->max_anisotropy = 16; break;
    case NUE_TEX_DEVICE_ANDROID_ETC2: add_etc2(c); c->max_size_2d = c->max_size_cube = 4096; c->max_size_3d = 256; c->max_anisotropy = 4; c->supports_texture_swizzle = NUE_TRUE; break;
    case NUE_TEX_DEVICE_ANDROID_ETC2_BC: add_bc(c); add_etc2(c); add_astc(c, false); c->max_size_2d = c->max_size_cube = 16384; break;
    case NUE_TEX_DEVICE_APPLE_ASTC: add_etc2(c); add_astc(c, false); add_pvrtc(c); c->max_size_3d = 2048; c->max_array_layers = 2048; break;
    case NUE_TEX_DEVICE_APPLE_PVRTC: add_pvrtc(c); c->max_size_2d = c->max_size_cube = 4096; c->max_size_3d = 2048; break;
    case NUE_TEX_DEVICE_APPLE_SILICON_MAC: add_bc(c); add_etc2(c); add_astc(c, true); break;
    default: break;
    }
    if (cls == NUE_TEX_DEVICE_APPLE_PVRTC) {
        // PVRTC textures must be square and power of two on PowerVR: the planner checks NUE_TEX_FINFO_REQUIRES_*.
        c->npot_full = NUE_FALSE; c->npot_with_mips = NUE_FALSE;
    }
    return NUE_OK;
}

const char* NUE_CALL nue_tex_device_class_name(nue_tex_device_class c) { return (uint32_t)c < NUE_TEX_DEVICE_COUNT ? kClassNames[c] : "?"; }

nue_tex_device_class NUE_CALL nue_tex_device_class_from_name(const char* name) {
    if (name)
        for (int i = 0; i < NUE_TEX_DEVICE_COUNT; ++i)
            if (std::strcmp(kClassNames[i], name) == 0) return nue_tex_device_class(i);
    return NUE_TEX_DEVICE_SOFTWARE;
}

void NUE_CALL nue_tex_caps_set_format(nue_tex_caps* caps, nue_tex_format f, uint32_t flags) {
    if (caps && (uint32_t)f < NUE_TEX_FORMAT_COUNT) caps->format_flags[f] = flags;
}
uint32_t NUE_CALL nue_tex_caps_get_format(const nue_tex_caps* caps, nue_tex_format f) {
    return caps && (uint32_t)f < NUE_TEX_FORMAT_COUNT ? caps->format_flags[f] : 0;
}
nue_bool NUE_CALL nue_tex_caps_can_use(const nue_tex_caps* caps, nue_tex_format f) {
    const uint32_t fl = nue_tex_caps_get_format(caps, f);
    return (fl & NUE_TEX_FCAP_SAMPLE) && (fl & NUE_TEX_FCAP_FILTER) && !(fl & NUE_TEX_FCAP_EMULATED) ? NUE_TRUE : NUE_FALSE;
}

nue_result NUE_CALL nue_tex_caps_from_bgfx(nue_tex_caps* caps, const uint16_t* bg, uint32_t count, uint32_t max_texture_size) {
    if (!caps || !bg || count < NUE_TEX_BGFX_FORMAT_COUNT) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t keep_size = caps->struct_size ? caps->struct_size : (uint32_t)sizeof *caps;
    nue_tex_caps_clear(caps);
    caps->struct_size = keep_size;
    nue_tex_format list[NUE_TEX_FORMAT_COUNT];
    const uint32_t n = nue_tex_format_list(list, NUE_TEX_FORMAT_COUNT);
    for (uint32_t i = 0; i < n; ++i) {
        const Fmt f = list[i];
        nue_bool srgb = NUE_FALSE;
        uint32_t id = nue_tex_format_to_bgfx(f, &srgb);
        // formats bgfx cannot express: derive from the linear twin / base format
        const auto* fi = info(f);
        Fmt base = f;
        if (id >= NUE_TEX_BGFX_FORMAT_COUNT) {
            if (f == NUE_TEX_BC2_PMA_UNORM) base = NUE_TEX_BC2_UNORM;
            else if (f == NUE_TEX_BC3_PMA_UNORM) base = NUE_TEX_BC3_UNORM;
            else if (fi && fi->colorspace == NUE_TEX_COLORSPACE_SRGB) base = fi->linear_twin;
            else continue;
            id = nue_tex_format_to_bgfx(base, &srgb);
            if (id >= NUE_TEX_BGFX_FORMAT_COUNT) continue;
        }
        const uint16_t b = bg[id];
        uint32_t fl = 0;
        if (b & 0x1u) fl |= NUE_TEX_FCAP_SAMPLE | NUE_TEX_FCAP_FILTER;
        if (b & 0x4u) fl |= NUE_TEX_FCAP_EMULATED;
        if (b & 0x8u) fl |= NUE_TEX_FCAP_3D;
        if (b & 0x40u) fl |= NUE_TEX_FCAP_CUBE;
        if (b & 0x1000u) fl |= NUE_TEX_FCAP_RENDER;
        if (b & 0x2000u) fl |= NUE_TEX_FCAP_MSAA;
        if (b & 0x8000u) fl |= NUE_TEX_FCAP_MIP_AUTOGEN;
        if ((fi && fi->colorspace == NUE_TEX_COLORSPACE_SRGB) && !(b & 0x2u)) fl = 0;          // sRGB twin needs hardware sRGB decode
        else if (fi && fi->colorspace == NUE_TEX_COLORSPACE_SRGB) fl |= NUE_TEX_FCAP_SRGB_SAMPLE;
        caps->format_flags[f] = fl;
    }
    caps->max_size_2d = caps->max_size_cube = max_texture_size;
    caps->max_size_3d = std::min<uint32_t>(max_texture_size, 2048);
    caps->max_array_layers = 256;
    caps->max_anisotropy = 16;
    caps->npot_full = caps->npot_with_mips = NUE_TRUE;
    caps->supports_texture_swizzle = NUE_FALSE;       // bgfx exposes no sampler swizzle: the planner will not assume it
    caps->astc_ldr_full_profile = NUE_TRUE;
    caps->astc_hdr = caps->format_flags[NUE_TEX_ASTC_4x4_HDR] ? NUE_TRUE : NUE_FALSE;
    const bool bc = (caps->format_flags[NUE_TEX_BC1_UNORM] & NUE_TEX_FCAP_SAMPLE) != 0;
    const bool etc = (caps->format_flags[NUE_TEX_ETC2_RGB8] & NUE_TEX_FCAP_SAMPLE) != 0;
    const bool astc = (caps->format_flags[NUE_TEX_ASTC_4x4_UNORM] & NUE_TEX_FCAP_SAMPLE) != 0;
    const bool pvr = (caps->format_flags[NUE_TEX_PVRTC1_4BPP] & NUE_TEX_FCAP_SAMPLE) != 0;
    caps->device_class = bc ? (astc ? (etc ? NUE_TEX_DEVICE_ANDROID_ETC2_BC : NUE_TEX_DEVICE_APPLE_SILICON_MAC) : (etc ? NUE_TEX_DEVICE_DESKTOP_BC_ETC2 : NUE_TEX_DEVICE_DESKTOP_BC))
                            : astc ? (pvr ? NUE_TEX_DEVICE_APPLE_ASTC : NUE_TEX_DEVICE_ANDROID_ASTC)
                            : etc ? NUE_TEX_DEVICE_ANDROID_ETC2 : pvr ? NUE_TEX_DEVICE_APPLE_PVRTC : NUE_TEX_DEVICE_SOFTWARE;
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_caps_to_string(const nue_tex_caps* caps, char* buffer, size_t capacity, size_t* out_size) {
    if (!caps) return NUE_ERR_INVALID_ARGUMENT;
    std::string s = std::string("class ") + nue_tex_device_class_name(caps->device_class) + ", max 2D " + std::to_string(caps->max_size_2d) + ", astc_hdr " +
                    (caps->astc_hdr ? "yes" : "no") + "\n";
    for (int i = 1; i < NUE_TEX_FORMAT_COUNT; ++i) {
        const uint32_t fl = caps->format_flags[i];
        if (!fl) continue;
        s += std::string(nue_tex_format_name(nue_tex_format(i))) + ":";
        if (fl & NUE_TEX_FCAP_SAMPLE) s += " sample";
        if (fl & NUE_TEX_FCAP_FILTER) s += " filter";
        if (fl & NUE_TEX_FCAP_RENDER) s += " render";
        if (fl & NUE_TEX_FCAP_SRGB_SAMPLE) s += " srgb";
        if (fl & NUE_TEX_FCAP_EMULATED) s += " EMULATED";
        s += "\n";
    }
    if (out_size) *out_size = s.size() + 1;
    if (buffer && capacity) { const size_t n = std::min(capacity - 1, s.size()); std::memcpy(buffer, s.data(), n); buffer[n] = 0; }
    return NUE_OK;
}

}  // extern "C"
