// Mappings between nue_tex_format and the identifiers of other APIs (D3D9, DXGI, Vulkan/KTX2, Metal, OpenGL(ES), bgfx).
// The numbers were written from the public headers; platform sessions must cross-check them against their SDK on first use
// (tests/texture/test_formats.cpp checks internal consistency: round trips and sizes, not the SDK values themselves).
#include "internal.hpp"

namespace nuetex {
namespace {

constexpr uint32_t NONE = 0;
constexpr uint32_t BGFX_NONE = 0xFFFFu;

struct Row {
    Fmt fmt;
    uint32_t d3d9, dxgi, vk, mtl, gl_internal, gl_format, gl_type;
    uint32_t bgfx;       // bgfx::TextureFormat id, BGFX_NONE if none
    bool bgfx_srgb;
};

#define FOURCC(a, b, c, d) (uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24))

// GL enums
enum : uint32_t {
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT = 0x1403, GL_FLOAT = 0x1406, GL_HALF_FLOAT = 0x140B,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034, GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368,
    GL_UNSIGNED_INT_10F_11F_11F_REV = 0x8C3B, GL_UNSIGNED_INT_5_9_9_9_REV = 0x8C3E,
    GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A, GL_RED = 0x1903, GL_RG = 0x8227,
    GL_BGR = 0x80E0, GL_BGRA = 0x80E1
};
// bgfx ids (order of bgfx::TextureFormat / bimg::TextureFormat)
enum : uint32_t {
    B_BC1 = 0, B_BC2, B_BC3, B_BC4, B_BC4S, B_BC5, B_BC5S, B_BC6H, B_BC6HU, B_BC7, B_ETC1, B_ETC2, B_ETC2A, B_ETC2A1, B_EACR11, B_EACR11S, B_EACRG11, B_EACRG11S,
    B_PTC12, B_PTC14, B_PTC12A, B_PTC14A, B_PTC22, B_PTC24, B_ATC, B_ATCE, B_ATCI, B_ASTC4x4 /* 27 .. 40 */, B_Unknown = 41, B_R1, B_A8, B_R8, B_R8I, B_R8U, B_R8S,
    B_R16, B_R16I, B_R16U, B_R16F, B_R16S, B_R32I, B_R32U, B_R32F, B_RG8, B_RG8I, B_RG8U, B_RG8S, B_RG16, B_RG16I, B_RG16U, B_RG16F, B_RG16S, B_RG32I, B_RG32U,
    B_RG32F, B_RGB8, B_RGB8I, B_RGB8U, B_RGB8S, B_RGB9E5F, B_BGRA8, B_RGBA8, B_RGBA8I, B_RGBA8U, B_RGBA8S, B_RGBA16, B_RGBA16I, B_RGBA16U, B_RGBA16F, B_RGBA16S,
    B_RGBA32I, B_RGBA32U, B_RGBA32F, B_B5G6R5, B_R5G6B5, B_BGRA4, B_RGBA4, B_BGR5A1, B_RGB5A1, B_RGB10A2, B_RGB10A2U, B_RG11B10F, B_UnknownDepth, B_D16, B_D24, B_D24S8,
    B_D32, B_D16F, B_D24F, B_D32F, B_D32FS8, B_D0S8, B_Count
};
static_assert(B_Count == NUE_TEX_BGFX_FORMAT_COUNT, "bgfx::TextureFormat::Count");

// Vulkan helper ids
enum : uint32_t { VK_ASTC_4x4_UNORM = 157, VK_PVRTC_BASE = 1000054000, VK_ASTC_HDR_BASE = 1000066000 };

const uint32_t kMetalAstcSrgb[14] = {186, 187, 188, 189, 190, 192, 193, 194, 195, 196, 197, 198, 199, 200};
const uint32_t kMetalAstcLdr[14] = {204, 205, 206, 207, 208, 210, 211, 212, 213, 214, 215, 216, 217, 218};
const uint32_t kMetalAstcHdr[14] = {222, 223, 224, 225, 226, 228, 229, 230, 231, 232, 233, 234, 235, 236};

std::vector<Row> build_rows() {
    std::vector<Row> r;
    auto add = [&](Fmt f, uint32_t d3d9, uint32_t dxgi, uint32_t vk, uint32_t mtl, uint32_t gli, uint32_t glf, uint32_t glt, uint32_t bgfx, bool bs = false) {
        r.push_back(Row{f, d3d9, dxgi, vk, mtl, gli, glf, glt, bgfx, bs});
    };
    // ---- uncompressed byte arrays ----  (d3d9, dxgi, vk, metal, gl internal/format/type, bgfx)
    add(NUE_TEX_R8_UNORM, NONE, 61, 9, 10, 0x8229, GL_RED, GL_UNSIGNED_BYTE, B_R8);
    add(NUE_TEX_RG8_UNORM, NONE, 49, 16, 30, 0x822B, GL_RG, GL_UNSIGNED_BYTE, B_RG8);
    add(NUE_TEX_RGB8_UNORM, NONE, NONE, 23, NONE, 0x8051, GL_RGB, GL_UNSIGNED_BYTE, B_RGB8);
    add(NUE_TEX_BGR8_UNORM, 20, NONE, 30, NONE, 0x8051, GL_BGR, GL_UNSIGNED_BYTE, BGFX_NONE);
    add(NUE_TEX_RGBA8_UNORM, 32, 28, 37, 70, 0x8058, GL_RGBA, GL_UNSIGNED_BYTE, B_RGBA8);
    add(NUE_TEX_BGRA8_UNORM, 21, 87, 44, 80, 0x8058, GL_BGRA, GL_UNSIGNED_BYTE, B_BGRA8);
    add(NUE_TEX_RGBX8_UNORM, 33, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_BGRX8_UNORM, 22, 88, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_A8_UNORM, 28, 65, 1000470001u, 1, 0x803C, GL_ALPHA, GL_UNSIGNED_BYTE, B_A8);
    add(NUE_TEX_L8_UNORM, 50, NONE, NONE, NONE, 0x8040, GL_LUMINANCE, GL_UNSIGNED_BYTE, BGFX_NONE);
    add(NUE_TEX_LA8_UNORM, 51, NONE, NONE, NONE, 0x8045, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, BGFX_NONE);
    add(NUE_TEX_L16_UNORM, 81, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_LA4_UNORM, 52, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_R8_SRGB, NONE, NONE, 15, 11, 0x8FBD /* GL_SR8_EXT */, GL_RED, GL_UNSIGNED_BYTE, B_R8, true);
    add(NUE_TEX_RG8_SRGB, NONE, NONE, 22, 31, 0x8FBE /* GL_SRG8_EXT */, GL_RG, GL_UNSIGNED_BYTE, B_RG8, true);
    add(NUE_TEX_RGB8_SRGB, NONE, NONE, 29, NONE, 0x8C41, GL_RGB, GL_UNSIGNED_BYTE, B_RGB8, true);
    add(NUE_TEX_BGR8_SRGB, NONE, NONE, 36, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_RGBA8_SRGB, NONE, 29, 43, 71, 0x8C43, GL_RGBA, GL_UNSIGNED_BYTE, B_RGBA8, true);
    add(NUE_TEX_BGRA8_SRGB, NONE, 91, 50, 81, 0x8C43, GL_BGRA, GL_UNSIGNED_BYTE, B_BGRA8, true);
    add(NUE_TEX_BGRX8_SRGB, NONE, 93, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_R8_SNORM, NONE, 63, 10, 12, 0x8F94, GL_RED, GL_BYTE, B_R8S);
    add(NUE_TEX_RG8_SNORM, 60, 51, 17, 32, 0x8F95, GL_RG, GL_BYTE, B_RG8S);
    add(NUE_TEX_RGBA8_SNORM, 63, 31, 38, 72, 0x8F97, GL_RGBA, GL_BYTE, B_RGBA8S);
    add(NUE_TEX_R16_UNORM, NONE, 56, 70, 20, 0x822A, GL_RED, GL_UNSIGNED_SHORT, B_R16);
    add(NUE_TEX_RG16_UNORM, 34, 35, 77, 60, 0x822C, GL_RG, GL_UNSIGNED_SHORT, B_RG16);
    add(NUE_TEX_RGBA16_UNORM, 36, 11, 91, 110, 0x805B, GL_RGBA, GL_UNSIGNED_SHORT, B_RGBA16);
    add(NUE_TEX_R16_SNORM, NONE, 58, 71, 22, 0x8F98, GL_RED, GL_SHORT, B_R16S);
    add(NUE_TEX_RG16_SNORM, 64, 37, 78, 62, 0x8F99, GL_RG, GL_SHORT, B_RG16S);
    add(NUE_TEX_RGBA16_SNORM, NONE, 13, 92, 112, 0x8F9B, GL_RGBA, GL_SHORT, B_RGBA16S);
    add(NUE_TEX_R16_FLOAT, 111, 54, 76, 25, 0x822D, GL_RED, GL_HALF_FLOAT, B_R16F);
    add(NUE_TEX_RG16_FLOAT, 112, 34, 83, 65, 0x822F, GL_RG, GL_HALF_FLOAT, B_RG16F);
    add(NUE_TEX_RGBA16_FLOAT, 113, 10, 97, 115, 0x881A, GL_RGBA, GL_HALF_FLOAT, B_RGBA16F);
    add(NUE_TEX_R32_FLOAT, 114, 41, 100, 55, 0x822E, GL_RED, GL_FLOAT, B_R32F);
    add(NUE_TEX_RG32_FLOAT, 115, 16, 103, 105, 0x8230, GL_RG, GL_FLOAT, B_RG32F);
    add(NUE_TEX_RGB32_FLOAT, NONE, 6, 106, NONE, 0x8815, GL_RGB, GL_FLOAT, BGFX_NONE);
    add(NUE_TEX_RGBA32_FLOAT, 116, 2, 109, 125, 0x8814, GL_RGBA, GL_FLOAT, B_RGBA32F);
    // ---- packed ----
    add(NUE_TEX_RGB565, 23, 85, 4, 40, 0x8D62, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, B_B5G6R5);
    add(NUE_TEX_BGR565, NONE, NONE, 5, NONE, NONE, 0, 0, B_R5G6B5);
    add(NUE_TEX_RGBA4, NONE, NONE, 2, NONE, 0x8056, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, BGFX_NONE);
    add(NUE_TEX_BGRA4, NONE, NONE, 3, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_ARGB4, 26, 115, 1000340000u, NONE, NONE, 0, 0, B_BGRA4);
    add(NUE_TEX_XRGB4, 30, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_RGB5A1, NONE, NONE, 6, NONE, 0x8057, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, BGFX_NONE);
    add(NUE_TEX_BGR5A1, NONE, NONE, 7, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_A1RGB5, 25, 86, 8, 43, NONE, 0, 0, B_BGR5A1);
    add(NUE_TEX_XRGB1555, 24, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_RGB332, 27, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_A8RGB332, 29, NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_A2RGB10, 35, NONE, 58, NONE, 0x8059, GL_BGRA, GL_UNSIGNED_INT_2_10_10_10_REV, BGFX_NONE);
    add(NUE_TEX_A2BGR10, 31, 24, 64, 90, 0x8059, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, B_RGB10A2);
    add(NUE_TEX_RG11B10_FLOAT, NONE, 26, 122, 92, 0x8C3A, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, B_RG11B10F);
    add(NUE_TEX_RGB9E5_FLOAT, NONE, 67, 123, 93, 0x8C3D, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, B_RGB9E5F);
    // ---- depth ----
    add(NUE_TEX_D16_UNORM, 80, 55, 124, 250, 0x81A5, 0, 0, B_D16);
    add(NUE_TEX_D24_UNORM_X8, 77, NONE, 125, NONE, 0x81A6, 0, 0, B_D24);
    add(NUE_TEX_D24_UNORM_S8_UINT, 75, 45, 129, 255, 0x88F0, 0, 0, B_D24S8);
    add(NUE_TEX_D32_FLOAT, 82, 40, 126, 252, 0x8CAC, 0, 0, B_D32F);
    add(NUE_TEX_D32_FLOAT_S8_UINT, NONE, 20, 130, 260, 0x8CAD, 0, 0, B_D32FS8);
    add(NUE_TEX_S8_UINT, NONE, NONE, 127, 253, 0x8D48, 0, 0, B_D0S8);
    // ---- BC ----
    add(NUE_TEX_BC1_UNORM, FOURCC('D', 'X', 'T', '1'), 71, 133, 130, 0x83F1, 0, 0, B_BC1);
    add(NUE_TEX_BC1_SRGB, NONE, 72, 134, 131, 0x8C4D, 0, 0, B_BC1, true);
    add(NUE_TEX_BC2_UNORM, FOURCC('D', 'X', 'T', '3'), 74, 135, 132, 0x83F2, 0, 0, B_BC2);
    add(NUE_TEX_BC2_SRGB, NONE, 75, 136, 133, 0x8C4E, 0, 0, B_BC2, true);
    add(NUE_TEX_BC2_PMA_UNORM, FOURCC('D', 'X', 'T', '2'), NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_BC3_UNORM, FOURCC('D', 'X', 'T', '5'), 77, 137, 134, 0x83F3, 0, 0, B_BC3);
    add(NUE_TEX_BC3_SRGB, NONE, 78, 138, 135, 0x8C4F, 0, 0, B_BC3, true);
    add(NUE_TEX_BC3_PMA_UNORM, FOURCC('D', 'X', 'T', '4'), NONE, NONE, NONE, NONE, 0, 0, BGFX_NONE);
    add(NUE_TEX_BC4_UNORM, FOURCC('A', 'T', 'I', '1'), 80, 139, 140, 0x8DBB, 0, 0, B_BC4);
    add(NUE_TEX_BC4_SNORM, FOURCC('B', 'C', '4', 'S'), 81, 140, 141, 0x8DBC, 0, 0, B_BC4S);
    add(NUE_TEX_BC5_UNORM, FOURCC('A', 'T', 'I', '2'), 83, 141, 142, 0x8DBD, 0, 0, B_BC5);
    add(NUE_TEX_BC5_SNORM, FOURCC('B', 'C', '5', 'S'), 84, 142, 143, 0x8DBE, 0, 0, B_BC5S);
    add(NUE_TEX_BC6H_UFLOAT, NONE, 95, 143, 151, 0x8E8F, 0, 0, B_BC6HU);
    add(NUE_TEX_BC6H_SFLOAT, NONE, 96, 144, 150, 0x8E8E, 0, 0, B_BC6H);
    add(NUE_TEX_BC7_UNORM, NONE, 98, 145, 152, 0x8E8C, 0, 0, B_BC7);
    add(NUE_TEX_BC7_SRGB, NONE, 99, 146, 153, 0x8E8D, 0, 0, B_BC7, true);
    // ---- ETC / EAC ----
    add(NUE_TEX_ETC1_RGB8, NONE, NONE, NONE, NONE, 0x8D64, 0, 0, B_ETC1);
    add(NUE_TEX_ETC2_RGB8, NONE, NONE, 147, 180, 0x9274, 0, 0, B_ETC2);
    add(NUE_TEX_ETC2_RGB8_SRGB, NONE, NONE, 148, 181, 0x9275, 0, 0, B_ETC2, true);
    add(NUE_TEX_ETC2_RGB8A1, NONE, NONE, 149, 182, 0x9276, 0, 0, B_ETC2A1);
    add(NUE_TEX_ETC2_RGB8A1_SRGB, NONE, NONE, 150, 183, 0x9277, 0, 0, B_ETC2A1, true);
    add(NUE_TEX_ETC2_RGBA8, NONE, NONE, 151, 178, 0x9278, 0, 0, B_ETC2A);
    add(NUE_TEX_ETC2_RGBA8_SRGB, NONE, NONE, 152, 179, 0x9279, 0, 0, B_ETC2A, true);
    add(NUE_TEX_EAC_R11_UNORM, NONE, NONE, 153, 170, 0x9270, 0, 0, B_EACR11);
    add(NUE_TEX_EAC_R11_SNORM, NONE, NONE, 154, 172, 0x9271, 0, 0, B_EACR11S);
    add(NUE_TEX_EAC_RG11_UNORM, NONE, NONE, 155, 174, 0x9272, 0, 0, B_EACRG11);
    add(NUE_TEX_EAC_RG11_SNORM, NONE, NONE, 156, 176, 0x9273, 0, 0, B_EACRG11S);
    // ---- ASTC ----
    for (int i = 0; i < 14; ++i) {
        add(Fmt(160 + i), NONE, NONE, VK_ASTC_4x4_UNORM + 2 * i, kMetalAstcLdr[i], 0x93B0 + i, 0, 0, B_ASTC4x4 + i);
        add(Fmt(176 + i), NONE, NONE, VK_ASTC_4x4_UNORM + 2 * i + 1, kMetalAstcSrgb[i], 0x93D0 + i, 0, 0, B_ASTC4x4 + i, true);
        add(Fmt(192 + i), NONE, NONE, VK_ASTC_HDR_BASE + i, kMetalAstcHdr[i], NONE, 0, 0, BGFX_NONE);
    }
    // ---- PVRTC ----
    add(NUE_TEX_PVRTC1_2BPP, NONE, NONE, VK_PVRTC_BASE + 0, 164, 0x8C03, 0, 0, B_PTC12A);
    add(NUE_TEX_PVRTC1_2BPP_SRGB, NONE, NONE, VK_PVRTC_BASE + 4, 165, 0x8A56, 0, 0, B_PTC12A, true);
    add(NUE_TEX_PVRTC1_4BPP, NONE, NONE, VK_PVRTC_BASE + 1, 166, 0x8C02, 0, 0, B_PTC14A);
    add(NUE_TEX_PVRTC1_4BPP_SRGB, NONE, NONE, VK_PVRTC_BASE + 5, 167, 0x8A57, 0, 0, B_PTC14A, true);
    add(NUE_TEX_PVRTC2_2BPP, NONE, NONE, VK_PVRTC_BASE + 2, NONE, 0x9137, 0, 0, B_PTC22);
    add(NUE_TEX_PVRTC2_2BPP_SRGB, NONE, NONE, VK_PVRTC_BASE + 6, NONE, NONE, 0, 0, B_PTC22, true);
    add(NUE_TEX_PVRTC2_4BPP, NONE, NONE, VK_PVRTC_BASE + 3, NONE, 0x9138, 0, 0, B_PTC24);
    add(NUE_TEX_PVRTC2_4BPP_SRGB, NONE, NONE, VK_PVRTC_BASE + 7, NONE, NONE, 0, 0, B_PTC24, true);
    // ---- ATC ----
    add(NUE_TEX_ATC_RGB, NONE, NONE, NONE, NONE, 0x8C92, 0, 0, B_ATC);
    add(NUE_TEX_ATC_RGBA_EXPLICIT, NONE, NONE, NONE, NONE, 0x8C93, 0, 0, B_ATCE);
    add(NUE_TEX_ATC_RGBA_INTERPOLATED, NONE, NONE, NONE, NONE, 0x87EE, 0, 0, B_ATCI);
    return r;
}

const std::vector<Row>& rows() { static const std::vector<Row> r = build_rows(); return r; }

struct Alias { int column; uint32_t value; Fmt fmt; };   // column: 0 d3d9, 1 dxgi, 2 vk, 3 mtl, 4 gl
const Alias kAliases[] = {
    {2, 131, NUE_TEX_BC1_UNORM}, {2, 132, NUE_TEX_BC1_SRGB},                                   // VK_FORMAT_BC1_RGB_*
    {4, 0x83F0, NUE_TEX_BC1_UNORM}, {4, 0x8C4C, NUE_TEX_BC1_SRGB},                              // GL DXT1 RGB
    {4, 0x8C00, NUE_TEX_PVRTC1_4BPP}, {4, 0x8C01, NUE_TEX_PVRTC1_2BPP},                         // GL PVRTC RGB
    {4, 0x8A54, NUE_TEX_PVRTC1_2BPP_SRGB}, {4, 0x8A55, NUE_TEX_PVRTC1_4BPP_SRGB},
    {3, 160, NUE_TEX_PVRTC1_2BPP}, {3, 161, NUE_TEX_PVRTC1_2BPP_SRGB}, {3, 162, NUE_TEX_PVRTC1_4BPP}, {3, 163, NUE_TEX_PVRTC1_4BPP_SRGB},  // Metal PVRTC RGB
    {1, 71, NUE_TEX_BC1_UNORM},
    {0, FOURCC('B', 'C', '4', 'U'), NUE_TEX_BC4_UNORM}, {0, FOURCC('B', 'C', '5', 'U'), NUE_TEX_BC5_UNORM},
    {0, FOURCC('D', 'X', 'T', '4'), NUE_TEX_BC3_PMA_UNORM},
    {0, 70, NUE_TEX_D16_UNORM}, {0, 71, NUE_TEX_D32_FLOAT}, {0, 73, NUE_TEX_D24_UNORM_S8_UINT}, {0, 79, NUE_TEX_D24_UNORM_S8_UINT}, {0, 83, NUE_TEX_D24_UNORM_S8_UINT},
};

}  // namespace
}  // namespace nuetex

using namespace nuetex;

namespace {
// D3D9 legacy table, as data
struct D3dName { uint32_t id; const char* name; };
const D3dName kD3dNames[] = {
    {20, "D3DFMT_R8G8B8"}, {21, "D3DFMT_A8R8G8B8"}, {22, "D3DFMT_X8R8G8B8"}, {23, "D3DFMT_R5G6B5"}, {24, "D3DFMT_X1R5G5B5"}, {25, "D3DFMT_A1R5G5B5"},
    {26, "D3DFMT_A4R4G4B4"}, {27, "D3DFMT_R3G3B2"}, {28, "D3DFMT_A8"}, {29, "D3DFMT_A8R3G3B2"}, {30, "D3DFMT_X4R4G4B4"}, {31, "D3DFMT_A2B10G10R10"},
    {32, "D3DFMT_A8B8G8R8"}, {33, "D3DFMT_X8B8G8R8"}, {34, "D3DFMT_G16R16"}, {35, "D3DFMT_A2R10G10B10"}, {36, "D3DFMT_A16B16G16R16"},
    {50, "D3DFMT_L8"}, {51, "D3DFMT_A8L8"}, {52, "D3DFMT_A4L4"}, {60, "D3DFMT_V8U8"}, {63, "D3DFMT_Q8W8V8U8"}, {64, "D3DFMT_V16U16"},
    {70, "D3DFMT_D16_LOCKABLE"}, {71, "D3DFMT_D32"}, {73, "D3DFMT_D15S1"}, {75, "D3DFMT_D24S8"}, {77, "D3DFMT_D24X8"}, {79, "D3DFMT_D24X4S4"}, {80, "D3DFMT_D16"},
    {81, "D3DFMT_L16"}, {82, "D3DFMT_D32F_LOCKABLE"}, {83, "D3DFMT_D24FS8"}, {111, "D3DFMT_R16F"}, {112, "D3DFMT_G16R16F"}, {113, "D3DFMT_A16B16G16R16F"},
    {114, "D3DFMT_R32F"}, {115, "D3DFMT_G32R32F"}, {116, "D3DFMT_A32B32G32R32F"},
    {FOURCC('A', 'T', 'I', '1'), "ATI1"}, {FOURCC('A', 'T', 'I', '2'), "ATI2"}, {FOURCC('B', 'C', '4', 'S'), "BC4S"}, {FOURCC('B', 'C', '4', 'U'), "BC4U"},
    {FOURCC('B', 'C', '5', 'S'), "BC5S"}, {FOURCC('B', 'C', '5', 'U'), "BC5U"}, {FOURCC('D', 'X', 'T', '1'), "DXT1"}, {FOURCC('D', 'X', 'T', '2'), "DXT2"},
    {FOURCC('D', 'X', 'T', '3'), "DXT3"}, {FOURCC('D', 'X', 'T', '4'), "DXT4"}, {FOURCC('D', 'X', 'T', '5'), "DXT5"},
};

bool d3d_exact(uint32_t id) { return !(id == 71 || id == 73 || id == 79 || id == 83); }

std::vector<nue_tex_d3d9_format_entry> build_d3d9_table() {
    std::vector<nue_tex_d3d9_format_entry> t;
    for (const auto& n : kD3dNames) {
        bool exact = true;
        const Fmt f = nue_tex_format_from_d3d9(n.id, nullptr);
        if (f == NUE_TEX_FORMAT_UNKNOWN) continue;
        exact = d3d_exact(n.id) && nue_tex_format_to_d3d9(f) == n.id;
        if (n.id == FOURCC('B', 'C', '4', 'U') || n.id == FOURCC('B', 'C', '5', 'U')) exact = true;
        nue_tex_d3d9_format_entry e{};
        e.d3d = n.id; e.format = f; e.is_fourcc = n.id > 0xFFFF; e.exact = exact;
        e.premultiplied_alpha = f == NUE_TEX_BC2_PMA_UNORM || f == NUE_TEX_BC3_PMA_UNORM;
        e.d3d_name = n.name;
        t.push_back(e);
    }
    std::sort(t.begin(), t.end(), [](const auto& a, const auto& b) { return a.d3d < b.d3d; });
    return t;
}
const std::vector<nue_tex_d3d9_format_entry>& d3d9_table() { static const auto t = build_d3d9_table(); return t; }

template <class Get>
Fmt find_primary(uint32_t v, Get get) {
    if (!v) return NUE_TEX_FORMAT_UNKNOWN;
    for (const Row& r : rows()) if (get(r) == v) return r.fmt;
    return NUE_TEX_FORMAT_UNKNOWN;
}
Fmt find_alias(int col, uint32_t v) {
    for (const Alias& a : kAliases) if (a.column == col && a.value == v) return a.fmt;
    return NUE_TEX_FORMAT_UNKNOWN;
}
const Row* row_of(Fmt f) {
    for (const Row& r : rows()) if (r.fmt == f) return &r;
    return nullptr;
}
}  // namespace

extern "C" {

nue_tex_format NUE_CALL nue_tex_format_from_d3d9(uint32_t d3dfmt, nue_bool* exact) {
    Fmt f = find_primary(d3dfmt, [](const Row& r) { return r.d3d9; });
    bool ex = f != NUE_TEX_FORMAT_UNKNOWN;
    if (f == NUE_TEX_FORMAT_UNKNOWN) { f = find_alias(0, d3dfmt); ex = f != NUE_TEX_FORMAT_UNKNOWN && d3dfmt != 71 && d3dfmt != 73 && d3dfmt != 79 && d3dfmt != 83; }
    if (f != NUE_TEX_FORMAT_UNKNOWN && d3dfmt <= 0xFFFF && !d3d_exact(d3dfmt)) ex = false;
    if (exact) *exact = ex ? NUE_TRUE : NUE_FALSE;
    return f;
}
uint32_t NUE_CALL nue_tex_format_to_d3d9(nue_tex_format format) { const Row* r = row_of(format); return r ? r->d3d9 : 0; }
nue_tex_format NUE_CALL nue_tex_format_from_dxgi(uint32_t v) { Fmt f = find_primary(v, [](const Row& r) { return r.dxgi; }); return f != NUE_TEX_FORMAT_UNKNOWN ? f : find_alias(1, v); }
uint32_t NUE_CALL nue_tex_format_to_dxgi(nue_tex_format format) { const Row* r = row_of(format); return r ? r->dxgi : 0; }
nue_tex_format NUE_CALL nue_tex_format_from_vk(uint32_t v) { Fmt f = find_primary(v, [](const Row& r) { return r.vk; }); return f != NUE_TEX_FORMAT_UNKNOWN ? f : find_alias(2, v); }
uint32_t NUE_CALL nue_tex_format_to_vk(nue_tex_format format) { const Row* r = row_of(format); return r ? r->vk : 0; }
nue_tex_format NUE_CALL nue_tex_format_from_metal(uint32_t v) { Fmt f = find_primary(v, [](const Row& r) { return r.mtl; }); return f != NUE_TEX_FORMAT_UNKNOWN ? f : find_alias(3, v); }
uint32_t NUE_CALL nue_tex_format_to_metal(nue_tex_format format) { const Row* r = row_of(format); return r ? r->mtl : 0; }

nue_tex_format NUE_CALL nue_tex_format_from_gl(uint32_t internal_format) {
    // Several of our formats share a GL internal format (BGRA8 / RGBA8): the first row (RGBA order) wins; callers that know the upload format use nue_tex_format_to_gl.
    Fmt f = find_primary(internal_format, [](const Row& r) { return r.gl_internal; });
    return f != NUE_TEX_FORMAT_UNKNOWN ? f : find_alias(4, internal_format);
}
nue_result NUE_CALL nue_tex_format_to_gl(nue_tex_format format, nue_tex_gl_format* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    const Row* r = row_of(format);
    if (!r || !r->gl_internal) return NUE_ERR_UNSUPPORTED;
    out->internal_format = r->gl_internal; out->format = r->gl_format; out->type = r->gl_type;
    return NUE_OK;
}

nue_tex_format NUE_CALL nue_tex_format_from_bgfx(uint32_t id, nue_bool srgb) {
    if (id >= B_Count || id == B_Unknown || id == B_UnknownDepth) return NUE_TEX_FORMAT_UNKNOWN;
    Fmt linear = NUE_TEX_FORMAT_UNKNOWN;
    for (const Row& r : rows()) {
        if (r.bgfx != id) continue;
        if (r.bgfx_srgb) { if (srgb) return r.fmt; } else linear = r.fmt;
    }
    return linear;
}
uint32_t NUE_CALL nue_tex_format_to_bgfx(nue_tex_format format, nue_bool* out_srgb) {
    const Row* r = row_of(format);
    if (out_srgb) *out_srgb = r && r->bgfx_srgb ? NUE_TRUE : NUE_FALSE;
    return r && r->bgfx != BGFX_NONE ? r->bgfx : B_Count;
}

const nue_tex_d3d9_format_entry* NUE_CALL nue_tex_d3d9_format_table(uint32_t* out_count) {
    const auto& t = d3d9_table();
    if (out_count) *out_count = (uint32_t)t.size();
    return t.data();
}

}  // extern "C"
