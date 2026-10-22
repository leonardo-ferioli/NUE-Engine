// Format registry: one entry per nue_tex_format, geometry helpers, name lookup.
#include "internal.hpp"
#include <cctype>

namespace nuetex {
namespace {

const uint8_t kAstcW[14] = {4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12};
const uint8_t kAstcH[14] = {4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12};

struct Registry {
    nue_tex_format_info entries[NUE_TEX_FORMAT_COUNT];
    std::string names[NUE_TEX_FORMAT_COUNT];
    bool valid[NUE_TEX_FORMAT_COUNT];

    void set(Fmt id, const char* name, nue_tex_format_kind kind, nue_tex_numeric_type type, nue_tex_colorspace cs, nue_tex_alpha_mode alpha, uint32_t flags,
             int bw, int bh, int bytes, int minw, int minh, int channels, int r, int g, int b, int a, const char* layout) {
        nue_tex_format_info& e = entries[id];
        std::memset(&e, 0, sizeof e);
        e.struct_size = sizeof e;
        e.format = id;
        names[id] = name;
        e.name = names[id].c_str();
        e.kind = kind; e.type = type; e.colorspace = cs; e.alpha = alpha; e.flags = flags;
        e.block_width = uint8_t(bw); e.block_height = uint8_t(bh); e.block_bytes = uint8_t(bytes);
        e.min_width = uint8_t(minw ? minw : bw); e.min_height = uint8_t(minh ? minh : bh);
        e.channels = uint8_t(channels);
        e.channel_bits[0] = uint8_t(r); e.channel_bits[1] = uint8_t(g); e.channel_bits[2] = uint8_t(b); e.channel_bits[3] = uint8_t(a);
        std::snprintf(e.layout, sizeof e.layout, "%s", layout);
        e.bits_per_pixel = 8.0f * float(bytes) / float(bw * bh);
        e.srgb_twin = id; e.linear_twin = id;
        valid[id] = true;
    }
    void twin(Fmt linear, Fmt srgb) {
        entries[linear].srgb_twin = srgb; entries[srgb].linear_twin = linear;
        entries[srgb].srgb_twin = srgb; entries[linear].linear_twin = linear;
    }
    void unc(Fmt id, const char* name, int bytes, int channels, int r, int g, int b, int a, const char* layout, nue_tex_numeric_type type = NUE_TEX_TYPE_UNORM,
             nue_tex_alpha_mode alpha = NUE_TEX_ALPHA_STRAIGHT, uint32_t flags = 0) {
        const nue_tex_colorspace cs = type == NUE_TEX_TYPE_SRGB ? NUE_TEX_COLORSPACE_SRGB : NUE_TEX_COLORSPACE_LINEAR;
        if (type == NUE_TEX_TYPE_FLOAT) flags |= NUE_TEX_FINFO_HDR;
        if (type == NUE_TEX_TYPE_SNORM) flags |= NUE_TEX_FINFO_SIGNED;
        set(id, name, NUE_TEX_KIND_UNCOMPRESSED, type, cs, a ? alpha : NUE_TEX_ALPHA_NONE, flags | NUE_TEX_FINFO_HAS_DECODER | NUE_TEX_FINFO_HAS_ENCODER,
            1, 1, bytes, 1, 1, channels, r, g, b, a, layout);
    }
    void blk(Fmt id, const char* name, int bw, int bh, int bytes, int channels, nue_tex_numeric_type type, nue_tex_alpha_mode alpha, uint32_t flags,
             const char* layout, int minw = 0, int minh = 0) {
        const nue_tex_colorspace cs = type == NUE_TEX_TYPE_SRGB ? NUE_TEX_COLORSPACE_SRGB : NUE_TEX_COLORSPACE_LINEAR;
        if (type == NUE_TEX_TYPE_FLOAT) flags |= NUE_TEX_FINFO_HDR;
        if (type == NUE_TEX_TYPE_SNORM) flags |= NUE_TEX_FINFO_SIGNED;
        set(id, name, NUE_TEX_KIND_BLOCK, type, cs, alpha, flags | NUE_TEX_FINFO_COMPRESSED | NUE_TEX_FINFO_LOSSY, bw, bh, bytes, minw, minh, channels, 8, 8, 8,
            alpha == NUE_TEX_ALPHA_NONE ? 0 : 8, layout);
    }
    void dep(Fmt id, const char* name, int bytes, int channels, int d, int s) {
        set(id, name, NUE_TEX_KIND_DEPTH, NUE_TEX_TYPE_UNORM, NUE_TEX_COLORSPACE_LINEAR, NUE_TEX_ALPHA_NONE, 0, 1, 1, bytes, 1, 1, channels, d, s, 0, 0, s ? "DS" : "D");
    }

    Registry() {
        std::memset(valid, 0, sizeof valid);
        std::memset(entries, 0, sizeof entries);
        const auto U = NUE_TEX_TYPE_UNORM; const auto S = NUE_TEX_TYPE_SNORM; const auto F = NUE_TEX_TYPE_FLOAT; const auto SR = NUE_TEX_TYPE_SRGB;
        const auto PAD = NUE_TEX_ALPHA_PADDING;
        unc(NUE_TEX_R8_UNORM, "R8_UNORM", 1, 1, 8, 0, 0, 0, "R");
        unc(NUE_TEX_RG8_UNORM, "RG8_UNORM", 2, 2, 8, 8, 0, 0, "RG");
        unc(NUE_TEX_RGB8_UNORM, "RGB8_UNORM", 3, 3, 8, 8, 8, 0, "RGB");
        unc(NUE_TEX_BGR8_UNORM, "BGR8_UNORM", 3, 3, 8, 8, 8, 0, "BGR");
        unc(NUE_TEX_RGBA8_UNORM, "RGBA8_UNORM", 4, 4, 8, 8, 8, 8, "RGBA");
        unc(NUE_TEX_BGRA8_UNORM, "BGRA8_UNORM", 4, 4, 8, 8, 8, 8, "BGRA");
        unc(NUE_TEX_RGBX8_UNORM, "RGBX8_UNORM", 4, 3, 8, 8, 8, 0, "RGBX", U, PAD);
        unc(NUE_TEX_BGRX8_UNORM, "BGRX8_UNORM", 4, 3, 8, 8, 8, 0, "BGRX", U, PAD);
        unc(NUE_TEX_A8_UNORM, "A8_UNORM", 1, 1, 0, 0, 0, 8, "A");
        unc(NUE_TEX_L8_UNORM, "L8_UNORM", 1, 1, 8, 0, 0, 0, "L");
        unc(NUE_TEX_LA8_UNORM, "LA8_UNORM", 2, 2, 8, 0, 0, 8, "LA");
        unc(NUE_TEX_L16_UNORM, "L16_UNORM", 2, 1, 16, 0, 0, 0, "L");
        unc(NUE_TEX_LA4_UNORM, "LA4_UNORM", 1, 2, 4, 0, 0, 4, "AL");
        unc(NUE_TEX_R8_SRGB, "R8_SRGB", 1, 1, 8, 0, 0, 0, "R", SR);
        unc(NUE_TEX_RG8_SRGB, "RG8_SRGB", 2, 2, 8, 8, 0, 0, "RG", SR);
        unc(NUE_TEX_RGB8_SRGB, "RGB8_SRGB", 3, 3, 8, 8, 8, 0, "RGB", SR);
        unc(NUE_TEX_BGR8_SRGB, "BGR8_SRGB", 3, 3, 8, 8, 8, 0, "BGR", SR);
        unc(NUE_TEX_RGBA8_SRGB, "RGBA8_SRGB", 4, 4, 8, 8, 8, 8, "RGBA", SR);
        unc(NUE_TEX_BGRA8_SRGB, "BGRA8_SRGB", 4, 4, 8, 8, 8, 8, "BGRA", SR);
        unc(NUE_TEX_BGRX8_SRGB, "BGRX8_SRGB", 4, 3, 8, 8, 8, 0, "BGRX", SR, PAD);
        unc(NUE_TEX_R8_SNORM, "R8_SNORM", 1, 1, 8, 0, 0, 0, "R", S);
        unc(NUE_TEX_RG8_SNORM, "RG8_SNORM", 2, 2, 8, 8, 0, 0, "RG", S, NUE_TEX_ALPHA_STRAIGHT, NUE_TEX_FINFO_LEGACY);
        unc(NUE_TEX_RGBA8_SNORM, "RGBA8_SNORM", 4, 4, 8, 8, 8, 8, "RGBA", S, NUE_TEX_ALPHA_STRAIGHT, NUE_TEX_FINFO_LEGACY);
        unc(NUE_TEX_R16_UNORM, "R16_UNORM", 2, 1, 16, 0, 0, 0, "R");
        unc(NUE_TEX_RG16_UNORM, "RG16_UNORM", 4, 2, 16, 16, 0, 0, "RG");
        unc(NUE_TEX_RGBA16_UNORM, "RGBA16_UNORM", 8, 4, 16, 16, 16, 16, "RGBA");
        unc(NUE_TEX_R16_SNORM, "R16_SNORM", 2, 1, 16, 0, 0, 0, "R", S);
        unc(NUE_TEX_RG16_SNORM, "RG16_SNORM", 4, 2, 16, 16, 0, 0, "RG", S, NUE_TEX_ALPHA_STRAIGHT, NUE_TEX_FINFO_LEGACY);
        unc(NUE_TEX_RGBA16_SNORM, "RGBA16_SNORM", 8, 4, 16, 16, 16, 16, "RGBA", S);
        unc(NUE_TEX_R16_FLOAT, "R16_FLOAT", 2, 1, 16, 0, 0, 0, "R", F);
        unc(NUE_TEX_RG16_FLOAT, "RG16_FLOAT", 4, 2, 16, 16, 0, 0, "RG", F);
        unc(NUE_TEX_RGBA16_FLOAT, "RGBA16_FLOAT", 8, 4, 16, 16, 16, 16, "RGBA", F);
        unc(NUE_TEX_R32_FLOAT, "R32_FLOAT", 4, 1, 32, 0, 0, 0, "R", F);
        unc(NUE_TEX_RG32_FLOAT, "RG32_FLOAT", 8, 2, 32, 32, 0, 0, "RG", F);
        unc(NUE_TEX_RGB32_FLOAT, "RGB32_FLOAT", 12, 3, 32, 32, 32, 0, "RGB", F);
        unc(NUE_TEX_RGBA32_FLOAT, "RGBA32_FLOAT", 16, 4, 32, 32, 32, 32, "RGBA", F);
        unc(NUE_TEX_RGB565, "RGB565", 2, 3, 5, 6, 5, 0, "RGB565");
        unc(NUE_TEX_BGR565, "BGR565", 2, 3, 5, 6, 5, 0, "BGR565");
        unc(NUE_TEX_RGBA4, "RGBA4", 2, 4, 4, 4, 4, 4, "RGBA4");
        unc(NUE_TEX_BGRA4, "BGRA4", 2, 4, 4, 4, 4, 4, "BGRA4");
        unc(NUE_TEX_ARGB4, "ARGB4", 2, 4, 4, 4, 4, 4, "ARGB4");
        unc(NUE_TEX_XRGB4, "XRGB4", 2, 3, 4, 4, 4, 0, "XRGB4", U, PAD);
        unc(NUE_TEX_RGB5A1, "RGB5A1", 2, 4, 5, 5, 5, 1, "RGB5A1", U, NUE_TEX_ALPHA_PUNCHTHROUGH);
        unc(NUE_TEX_BGR5A1, "BGR5A1", 2, 4, 5, 5, 5, 1, "BGR5A1", U, NUE_TEX_ALPHA_PUNCHTHROUGH);
        unc(NUE_TEX_A1RGB5, "A1RGB5", 2, 4, 5, 5, 5, 1, "A1RGB5", U, NUE_TEX_ALPHA_PUNCHTHROUGH);
        unc(NUE_TEX_XRGB1555, "XRGB1555", 2, 3, 5, 5, 5, 0, "XRGB1555", U, PAD);
        unc(NUE_TEX_RGB332, "RGB332", 1, 3, 3, 3, 2, 0, "RGB332", U, NUE_TEX_ALPHA_NONE, NUE_TEX_FINFO_LEGACY);
        unc(NUE_TEX_A8RGB332, "A8RGB332", 2, 4, 3, 3, 2, 8, "ARGB8332", U, NUE_TEX_ALPHA_STRAIGHT, NUE_TEX_FINFO_LEGACY);
        unc(NUE_TEX_A2RGB10, "A2RGB10", 4, 4, 10, 10, 10, 2, "A2RGB10");
        unc(NUE_TEX_A2BGR10, "A2BGR10", 4, 4, 10, 10, 10, 2, "A2BGR10");
        unc(NUE_TEX_RG11B10_FLOAT, "RG11B10_FLOAT", 4, 3, 11, 11, 10, 0, "RGB", F);
        unc(NUE_TEX_RGB9E5_FLOAT, "RGB9E5_FLOAT", 4, 3, 9, 9, 9, 0, "RGB", F);
        twin(NUE_TEX_R8_UNORM, NUE_TEX_R8_SRGB); twin(NUE_TEX_RG8_UNORM, NUE_TEX_RG8_SRGB); twin(NUE_TEX_RGB8_UNORM, NUE_TEX_RGB8_SRGB);
        twin(NUE_TEX_BGR8_UNORM, NUE_TEX_BGR8_SRGB); twin(NUE_TEX_RGBA8_UNORM, NUE_TEX_RGBA8_SRGB); twin(NUE_TEX_BGRA8_UNORM, NUE_TEX_BGRA8_SRGB);
        twin(NUE_TEX_BGRX8_UNORM, NUE_TEX_BGRX8_SRGB);

        // depth / stencil
        dep(NUE_TEX_D16_UNORM, "D16_UNORM", 2, 1, 16, 0);
        dep(NUE_TEX_D24_UNORM_X8, "D24_UNORM_X8", 4, 1, 24, 0);
        dep(NUE_TEX_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT", 4, 2, 24, 8);
        dep(NUE_TEX_D32_FLOAT, "D32_FLOAT", 4, 1, 32, 0);
        dep(NUE_TEX_D32_FLOAT_S8_UINT, "D32_FLOAT_S8_UINT", 8, 2, 32, 8);
        dep(NUE_TEX_S8_UINT, "S8_UINT", 1, 1, 0, 8);

        // BC
        const uint32_t DE = NUE_TEX_FINFO_HAS_DECODER, EN = NUE_TEX_FINFO_HAS_ENCODER, DE_EN = DE | EN;
        const auto ST = NUE_TEX_ALPHA_STRAIGHT, PM = NUE_TEX_ALPHA_PREMULTIPLIED, PT = NUE_TEX_ALPHA_PUNCHTHROUGH, NO = NUE_TEX_ALPHA_NONE;
        blk(NUE_TEX_BC1_UNORM, "BC1_UNORM", 4, 4, 8, 4, U, PT, DE_EN, "RGBA");
        blk(NUE_TEX_BC1_SRGB, "BC1_SRGB", 4, 4, 8, 4, SR, PT, DE_EN, "RGBA");
        blk(NUE_TEX_BC2_UNORM, "BC2_UNORM", 4, 4, 16, 4, U, ST, DE, "RGBA");
        blk(NUE_TEX_BC2_SRGB, "BC2_SRGB", 4, 4, 16, 4, SR, ST, DE, "RGBA");
        blk(NUE_TEX_BC2_PMA_UNORM, "BC2_PMA_UNORM", 4, 4, 16, 4, U, PM, DE, "RGBA", 0, 0);
        blk(NUE_TEX_BC3_UNORM, "BC3_UNORM", 4, 4, 16, 4, U, ST, DE_EN, "RGBA");
        blk(NUE_TEX_BC3_SRGB, "BC3_SRGB", 4, 4, 16, 4, SR, ST, DE_EN, "RGBA");
        blk(NUE_TEX_BC3_PMA_UNORM, "BC3_PMA_UNORM", 4, 4, 16, 4, U, PM, DE_EN, "RGBA");
        blk(NUE_TEX_BC4_UNORM, "BC4_UNORM", 4, 4, 8, 1, U, NO, DE_EN, "R");
        blk(NUE_TEX_BC4_SNORM, "BC4_SNORM", 4, 4, 8, 1, S, NO, DE, "R");
        blk(NUE_TEX_BC5_UNORM, "BC5_UNORM", 4, 4, 16, 2, U, NO, DE_EN, "RG");
        blk(NUE_TEX_BC5_SNORM, "BC5_SNORM", 4, 4, 16, 2, S, NO, DE, "RG");
        blk(NUE_TEX_BC6H_UFLOAT, "BC6H_UFLOAT", 4, 4, 16, 3, F, NO, DE, "RGB");
        blk(NUE_TEX_BC6H_SFLOAT, "BC6H_SFLOAT", 4, 4, 16, 3, F, NO, DE, "RGB");
        entries[NUE_TEX_BC6H_SFLOAT].flags |= NUE_TEX_FINFO_SIGNED;
        blk(NUE_TEX_BC7_UNORM, "BC7_UNORM", 4, 4, 16, 4, U, ST, DE_EN, "RGBA");
        blk(NUE_TEX_BC7_SRGB, "BC7_SRGB", 4, 4, 16, 4, SR, ST, DE_EN, "RGBA");
        twin(NUE_TEX_BC1_UNORM, NUE_TEX_BC1_SRGB); twin(NUE_TEX_BC2_UNORM, NUE_TEX_BC2_SRGB); twin(NUE_TEX_BC3_UNORM, NUE_TEX_BC3_SRGB);
        twin(NUE_TEX_BC7_UNORM, NUE_TEX_BC7_SRGB);
        for (Fmt f : {NUE_TEX_BC2_PMA_UNORM, NUE_TEX_BC3_PMA_UNORM}) entries[f].flags |= NUE_TEX_FINFO_LEGACY;

        // ETC / EAC
        blk(NUE_TEX_ETC1_RGB8, "ETC1_RGB8", 4, 4, 8, 3, U, NO, DE_EN, "RGB");
        blk(NUE_TEX_ETC2_RGB8, "ETC2_RGB8", 4, 4, 8, 3, U, NO, DE_EN, "RGB");
        blk(NUE_TEX_ETC2_RGB8_SRGB, "ETC2_RGB8_SRGB", 4, 4, 8, 3, SR, NO, DE_EN, "RGB");
        blk(NUE_TEX_ETC2_RGB8A1, "ETC2_RGB8A1", 4, 4, 8, 4, U, PT, 0, "RGBA");          // no decoder yet
        blk(NUE_TEX_ETC2_RGB8A1_SRGB, "ETC2_RGB8A1_SRGB", 4, 4, 8, 4, SR, PT, 0, "RGBA");
        blk(NUE_TEX_ETC2_RGBA8, "ETC2_RGBA8", 4, 4, 16, 4, U, ST, DE_EN, "RGBA");
        blk(NUE_TEX_ETC2_RGBA8_SRGB, "ETC2_RGBA8_SRGB", 4, 4, 16, 4, SR, ST, DE_EN, "RGBA");
        blk(NUE_TEX_EAC_R11_UNORM, "EAC_R11_UNORM", 4, 4, 8, 1, U, NO, DE_EN, "R");
        blk(NUE_TEX_EAC_R11_SNORM, "EAC_R11_SNORM", 4, 4, 8, 1, S, NO, DE, "R");
        blk(NUE_TEX_EAC_RG11_UNORM, "EAC_RG11_UNORM", 4, 4, 16, 2, U, NO, DE_EN, "RG");
        blk(NUE_TEX_EAC_RG11_SNORM, "EAC_RG11_SNORM", 4, 4, 16, 2, S, NO, DE, "RG");
        twin(NUE_TEX_ETC2_RGB8, NUE_TEX_ETC2_RGB8_SRGB); twin(NUE_TEX_ETC2_RGB8A1, NUE_TEX_ETC2_RGB8A1_SRGB); twin(NUE_TEX_ETC2_RGBA8, NUE_TEX_ETC2_RGBA8_SRGB);

        // ASTC
        for (int i = 0; i < 14; ++i) {
            char n[40];
            std::snprintf(n, sizeof n, "ASTC_%dx%d_UNORM", kAstcW[i], kAstcH[i]);
            blk(Fmt(160 + i), n, kAstcW[i], kAstcH[i], 16, 4, U, ST, DE_EN, "RGBA");
            std::snprintf(n, sizeof n, "ASTC_%dx%d_SRGB", kAstcW[i], kAstcH[i]);
            blk(Fmt(176 + i), n, kAstcW[i], kAstcH[i], 16, 4, SR, ST, DE_EN, "RGBA");
            std::snprintf(n, sizeof n, "ASTC_%dx%d_HDR", kAstcW[i], kAstcH[i]);
            blk(Fmt(192 + i), n, kAstcW[i], kAstcH[i], 16, 4, F, ST, DE_EN, "RGBA");
            twin(Fmt(160 + i), Fmt(176 + i));
        }

        // PVRTC
        const uint32_t POT = NUE_TEX_FINFO_REQUIRES_POT | NUE_TEX_FINFO_REQUIRES_SQUARE;
        blk(NUE_TEX_PVRTC1_2BPP, "PVRTC1_2BPP", 8, 4, 8, 4, U, ST, POT, "RGBA", 16, 8);
        blk(NUE_TEX_PVRTC1_2BPP_SRGB, "PVRTC1_2BPP_SRGB", 8, 4, 8, 4, SR, ST, POT, "RGBA", 16, 8);
        blk(NUE_TEX_PVRTC1_4BPP, "PVRTC1_4BPP", 4, 4, 8, 4, U, ST, DE_EN | POT, "RGBA", 8, 8);
        blk(NUE_TEX_PVRTC1_4BPP_SRGB, "PVRTC1_4BPP_SRGB", 4, 4, 8, 4, SR, ST, DE_EN | POT, "RGBA", 8, 8);
        blk(NUE_TEX_PVRTC2_2BPP, "PVRTC2_2BPP", 8, 4, 8, 4, U, ST, POT | NUE_TEX_FINFO_LEGACY, "RGBA", 16, 8);
        blk(NUE_TEX_PVRTC2_2BPP_SRGB, "PVRTC2_2BPP_SRGB", 8, 4, 8, 4, SR, ST, POT | NUE_TEX_FINFO_LEGACY, "RGBA", 16, 8);
        blk(NUE_TEX_PVRTC2_4BPP, "PVRTC2_4BPP", 4, 4, 8, 4, U, ST, POT | NUE_TEX_FINFO_LEGACY, "RGBA", 8, 8);
        blk(NUE_TEX_PVRTC2_4BPP_SRGB, "PVRTC2_4BPP_SRGB", 4, 4, 8, 4, SR, ST, POT | NUE_TEX_FINFO_LEGACY, "RGBA", 8, 8);
        twin(NUE_TEX_PVRTC1_2BPP, NUE_TEX_PVRTC1_2BPP_SRGB); twin(NUE_TEX_PVRTC1_4BPP, NUE_TEX_PVRTC1_4BPP_SRGB);
        twin(NUE_TEX_PVRTC2_2BPP, NUE_TEX_PVRTC2_2BPP_SRGB); twin(NUE_TEX_PVRTC2_4BPP, NUE_TEX_PVRTC2_4BPP_SRGB);

        // ATC
        blk(NUE_TEX_ATC_RGB, "ATC_RGB", 4, 4, 8, 3, U, NO, NUE_TEX_FINFO_LEGACY, "RGB");
        blk(NUE_TEX_ATC_RGBA_EXPLICIT, "ATC_RGBA_EXPLICIT", 4, 4, 16, 4, U, ST, NUE_TEX_FINFO_LEGACY, "RGBA");
        blk(NUE_TEX_ATC_RGBA_INTERPOLATED, "ATC_RGBA_INTERPOLATED", 4, 4, 16, 4, U, ST, NUE_TEX_FINFO_LEGACY, "RGBA");
    }
};

const Registry& reg() { static const Registry r; return r; }

bool ieq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) return false;
    return *a == *b;
}

}  // namespace
}  // namespace nuetex

using namespace nuetex;

extern "C" {

const nue_tex_format_info* NUE_CALL nue_tex_get_format_info(nue_tex_format format) {
    if ((uint32_t)format >= NUE_TEX_FORMAT_COUNT) return nullptr;
    const Registry& r = reg();
    return r.valid[format] ? &r.entries[format] : nullptr;
}

const char* NUE_CALL nue_tex_format_name(nue_tex_format format) {
    const auto* i = nue_tex_get_format_info(format);
    return i ? i->name : "?";
}

nue_tex_format NUE_CALL nue_tex_format_from_name(const char* name) {
    if (!name) return NUE_TEX_FORMAT_UNKNOWN;
    const Registry& r = reg();
    for (int i = 1; i < NUE_TEX_FORMAT_COUNT; ++i)
        if (r.valid[i] && ieq(r.entries[i].name, name)) return nue_tex_format(i);
    return NUE_TEX_FORMAT_UNKNOWN;
}

uint32_t NUE_CALL nue_tex_format_list(nue_tex_format* out, uint32_t capacity) {
    const Registry& r = reg();
    uint32_t n = 0;
    for (int i = 1; i < NUE_TEX_FORMAT_COUNT; ++i)
        if (r.valid[i]) { if (out && n < capacity) out[n] = nue_tex_format(i); ++n; }
    return n;
}

nue_bool NUE_CALL nue_tex_format_is_compressed(nue_tex_format format) {
    const auto* i = nue_tex_get_format_info(format);
    return i && (i->flags & NUE_TEX_FINFO_COMPRESSED) ? NUE_TRUE : NUE_FALSE;
}

nue_tex_format NUE_CALL nue_tex_format_to_srgb(nue_tex_format format) {
    const auto* i = nue_tex_get_format_info(format);
    if (!i) return NUE_TEX_FORMAT_UNKNOWN;
    return i->srgb_twin != format || i->colorspace == NUE_TEX_COLORSPACE_SRGB ? i->srgb_twin : NUE_TEX_FORMAT_UNKNOWN;
}

nue_tex_format NUE_CALL nue_tex_format_to_linear(nue_tex_format format) {
    const auto* i = nue_tex_get_format_info(format);
    if (!i) return NUE_TEX_FORMAT_UNKNOWN;
    return i->linear_twin != format || i->colorspace == NUE_TEX_COLORSPACE_LINEAR ? i->linear_twin : NUE_TEX_FORMAT_UNKNOWN;
}

uint32_t NUE_CALL nue_tex_mip_extent(uint32_t base, uint32_t level) {
    const uint32_t v = level >= 32 ? 0u : (base >> level);
    return v ? v : 1u;
}

uint32_t NUE_CALL nue_tex_mip_count(uint32_t width, uint32_t height, uint32_t depth) {
    uint32_t m = std::max(width, std::max(height, depth ? depth : 1u)), n = 1;
    while (m > 1) { m >>= 1; ++n; }
    return n;
}

uint32_t NUE_CALL nue_tex_row_pitch(nue_tex_format format, uint32_t width) {
    const auto* i = nue_tex_get_format_info(format);
    if (!i || !width) return 0;
    const uint32_t w = std::max<uint32_t>(width, i->min_width);
    return ((w + i->block_width - 1) / i->block_width) * i->block_bytes;
}

uint32_t NUE_CALL nue_tex_rows(nue_tex_format format, uint32_t height) {
    const auto* i = nue_tex_get_format_info(format);
    if (!i || !height) return 0;
    const uint32_t h = std::max<uint32_t>(height, i->min_height);
    return (h + i->block_height - 1) / i->block_height;
}

uint64_t NUE_CALL nue_tex_level_size(nue_tex_format format, uint32_t width, uint32_t height, uint32_t depth) {
    return uint64_t(nue_tex_row_pitch(format, width)) * nue_tex_rows(format, height) * (depth ? depth : 1u);
}

uint64_t NUE_CALL nue_tex_chain_size(nue_tex_format format, uint32_t width, uint32_t height, uint32_t depth, uint32_t mip_levels, uint32_t layers, uint32_t faces) {
    if (!depth) depth = 1;
    if (!mip_levels) mip_levels = nue_tex_mip_count(width, height, depth);
    uint64_t total = 0;
    for (uint32_t l = 0; l < mip_levels; ++l)
        total += nue_tex_level_size(format, nue_tex_mip_extent(width, l), nue_tex_mip_extent(height, l), nue_tex_mip_extent(depth, l));
    return total * (layers ? layers : 1u) * (faces ? faces : 1u);
}

}  // extern "C"
