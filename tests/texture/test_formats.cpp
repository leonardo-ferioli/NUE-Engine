// Format registry, geometry, API mappings, capability tables.
#include "tex_common.hpp"
#include "nue/gfx.h"
#ifdef NUE_TEXTURE_TEST_INTERNALS
#  include "internal.hpp"
#endif
#include <set>
#include <string>

static std::vector<nue_tex_format> all_formats() {
    nue_tex_format list[NUE_TEX_FORMAT_COUNT];
    const uint32_t n = nue_tex_format_list(list, NUE_TEX_FORMAT_COUNT);
    return std::vector<nue_tex_format>(list, list + n);
}

TEST_CASE(registry_is_consistent) {
    const auto fmts = all_formats();
    CHECK(fmts.size() > 120);
    std::set<std::string> names;
    for (nue_tex_format f : fmts) {
        const nue_tex_format_info* i = nue_tex_get_format_info(f);
        CHECK(i != nullptr);
        if (!i) continue;
        CHECK_EQ(i->format, f);
        CHECK(names.insert(i->name).second);                                     // unique names
        CHECK_EQ(nue_tex_format_from_name(i->name), f);
        CHECK_STR(nue_tex_format_name(f), i->name);
        CHECK(i->block_bytes > 0 && i->block_width > 0 && i->block_height > 0);
        CHECK_NEAR(i->bits_per_pixel, 8.0 * i->block_bytes / (i->block_width * i->block_height), 1e-4);
        CHECK((i->kind == NUE_TEX_KIND_BLOCK) == (nue_tex_format_is_compressed(f) != 0));
        // twins point back at each other
        if (i->srgb_twin != f) CHECK_EQ(nue_tex_get_format_info(i->srgb_twin)->linear_twin, f);
        if (i->kind == NUE_TEX_KIND_DEPTH) continue;
        // decoder / encoder flags match what the codecs really do
#ifdef NUE_TEXTURE_TEST_INTERNALS
        CHECK_EQ((i->flags & NUE_TEX_FINFO_HAS_DECODER) != 0, nuetex::can_decode(f) || i->kind == NUE_TEX_KIND_UNCOMPRESSED);
        CHECK_EQ((i->flags & NUE_TEX_FINFO_HAS_ENCODER) != 0, nuetex::can_encode(f) || i->kind == NUE_TEX_KIND_UNCOMPRESSED);
#endif
    }
    CHECK_EQ(nue_tex_get_format_info(NUE_TEX_FORMAT_UNKNOWN) == nullptr, true);
    CHECK_EQ(nue_tex_get_format_info(nue_tex_format(250)) == nullptr, true);
}

TEST_CASE(geometry) {
    CHECK_EQ(nue_tex_level_size(NUE_TEX_BC1_UNORM, 256, 256, 1), 256 * 256 / 2);
    CHECK_EQ(nue_tex_level_size(NUE_TEX_BC3_UNORM, 256, 256, 1), 256 * 256);
    CHECK_EQ(nue_tex_level_size(NUE_TEX_BC1_UNORM, 1, 1, 1), 8);                 // one block
    CHECK_EQ(nue_tex_level_size(NUE_TEX_BC7_UNORM, 5, 3, 1), 2 * 16);
    CHECK_EQ(nue_tex_level_size(NUE_TEX_RGBA8_UNORM, 7, 3, 1), 7 * 3 * 4);
    CHECK_EQ(nue_tex_level_size(NUE_TEX_RGB8_UNORM, 5, 5, 1), 75);
    CHECK_EQ(nue_tex_level_size(NUE_TEX_ASTC_UNORM(4), 64, 64, 1), 11 * 11 * 16);   // 6x6 blocks, 64 -> 11 blocks
    CHECK_EQ(nue_tex_level_size(NUE_TEX_ASTC_12x12_UNORM, 1, 1, 1), 16);
    CHECK_EQ(nue_tex_level_size(NUE_TEX_PVRTC1_4BPP, 1, 1, 1), 2 * 2 * 8);       // minimum 8x8 texels = 2x2 blocks
    CHECK_EQ(nue_tex_level_size(NUE_TEX_PVRTC1_2BPP, 8, 8, 1), 1 * 2 * 8 * 2);   // minimum 16x8: 2 x 2 blocks of 8x4
    CHECK_EQ(nue_tex_row_pitch(NUE_TEX_BC1_UNORM, 17), 5 * 8);
    CHECK_EQ(nue_tex_rows(NUE_TEX_BC1_UNORM, 17), 5);
    CHECK_EQ(nue_tex_mip_count(256, 64, 1), 9);
    CHECK_EQ(nue_tex_mip_count(1, 1, 1), 1);
    CHECK_EQ(nue_tex_mip_extent(100, 3), 12);
    CHECK_EQ(nue_tex_mip_extent(1, 5), 1);
    // chain sizes: 256x256 DXT1 with all mips
    uint64_t sum = 0;
    for (uint32_t l = 0; l < 9; ++l) sum += nue_tex_level_size(NUE_TEX_BC1_UNORM, nue_tex_mip_extent(256, l), nue_tex_mip_extent(256, l), 1);
    CHECK_EQ(nue_tex_chain_size(NUE_TEX_BC1_UNORM, 256, 256, 1, 0, 1, 1), sum);
    CHECK_EQ(nue_tex_chain_size(NUE_TEX_BC1_UNORM, 256, 256, 1, 0, 2, 6), sum * 12);
}

TEST_CASE(mappings_round_trip) {
    nue_tex_format list[NUE_TEX_FORMAT_COUNT];
    const uint32_t n = nue_tex_format_list(list, NUE_TEX_FORMAT_COUNT);
    for (uint32_t k = 0; k < n; ++k) {
        const nue_tex_format f = list[k];
        if (const uint32_t v = nue_tex_format_to_vk(f)) CHECK_EQ(nue_tex_format_from_vk(v), f);
        if (const uint32_t v = nue_tex_format_to_dxgi(f)) CHECK_EQ(nue_tex_format_from_dxgi(v), f);
        if (const uint32_t v = nue_tex_format_to_metal(f)) CHECK_EQ(nue_tex_format_from_metal(v), f);
        if (const uint32_t v = nue_tex_format_to_d3d9(f)) CHECK_EQ(nue_tex_format_from_d3d9(v, nullptr), f);
        nue_bool srgb = 0;
        const uint32_t b = nue_tex_format_to_bgfx(f, &srgb);
        if (b < NUE_TEX_BGFX_FORMAT_COUNT) CHECK_EQ(nue_tex_format_from_bgfx(b, srgb), f);
        nue_tex_gl_format gl;
        if (nue_tex_format_to_gl(f, &gl) >= 0 && nue_tex_get_format_info(f)->kind == NUE_TEX_KIND_BLOCK) CHECK_EQ(nue_tex_format_from_gl(gl.internal_format), f);
    }
}

TEST_CASE(d3d9_known_values) {
    nue_bool exact = 0;
    const uint32_t DXT1 = 'D' | ('X' << 8) | ('T' << 16) | ('1' << 24), DXT5 = 'D' | ('X' << 8) | ('T' << 16) | ('5' << 24);
    CHECK_EQ(nue_tex_format_from_d3d9(DXT1, &exact), NUE_TEX_BC1_UNORM); CHECK(exact);
    CHECK_EQ(nue_tex_format_from_d3d9(DXT5, nullptr), NUE_TEX_BC3_UNORM);
    CHECK_EQ(nue_tex_format_from_d3d9('A' | ('T' << 8) | ('I' << 16) | ('2' << 24), nullptr), NUE_TEX_BC5_UNORM);
    CHECK_EQ(nue_tex_format_from_d3d9(21, nullptr), NUE_TEX_BGRA8_UNORM);           // D3DFMT_A8R8G8B8 = BGRA bytes
    CHECK_EQ(nue_tex_format_from_d3d9(22, nullptr), NUE_TEX_BGRX8_UNORM);
    CHECK_EQ(nue_tex_format_from_d3d9(23, nullptr), NUE_TEX_RGB565);
    CHECK_EQ(nue_tex_format_from_d3d9(26, nullptr), NUE_TEX_ARGB4);
    CHECK_EQ(nue_tex_format_from_d3d9(60, nullptr), NUE_TEX_RG8_SNORM);             // V8U8
    CHECK_EQ(nue_tex_format_from_d3d9(116, nullptr), NUE_TEX_RGBA32_FLOAT);
    CHECK_EQ(nue_tex_format_from_d3d9(83, &exact), NUE_TEX_D24_UNORM_S8_UINT); CHECK(!exact);   // D24FS8 is only approximated
    CHECK_EQ(nue_tex_format_from_d3d9(9999, nullptr), NUE_TEX_FORMAT_UNKNOWN);
    CHECK_EQ(nue_tex_format_to_d3d9(NUE_TEX_BC1_UNORM), DXT1);
    // data table: sorted, every entry resolves to its format
    uint32_t count = 0;
    const nue_tex_d3d9_format_entry* t = nue_tex_d3d9_format_table(&count);
    CHECK(count > 30);
    for (uint32_t i = 0; i < count; ++i) {
        if (i) CHECK(t[i - 1].d3d < t[i].d3d);
        CHECK_EQ(nue_tex_format_from_d3d9(t[i].d3d, nullptr), t[i].format);
        CHECK(t[i].d3d_name != nullptr);
    }
}

TEST_CASE(vulkan_dxgi_known_values) {
    CHECK_EQ(nue_tex_format_to_vk(NUE_TEX_BC7_SRGB), 146u);
    CHECK_EQ(nue_tex_format_to_vk(NUE_TEX_ETC2_RGBA8), 151u);
    CHECK_EQ(nue_tex_format_to_vk(NUE_TEX_ASTC_4x4_UNORM), 157u);
    CHECK_EQ(nue_tex_format_to_vk(NUE_TEX_ASTC_12x12_SRGB), 184u);
    CHECK_EQ(nue_tex_format_to_vk(NUE_TEX_ASTC_4x4_HDR), 1000066000u);
    CHECK_EQ(nue_tex_format_to_vk(NUE_TEX_RGBA8_UNORM), 37u);
    CHECK_EQ(nue_tex_format_from_vk(131), NUE_TEX_BC1_UNORM);                       // BC1 RGB alias
    CHECK_EQ(nue_tex_format_to_dxgi(NUE_TEX_BC1_UNORM), 71u);
    CHECK_EQ(nue_tex_format_to_dxgi(NUE_TEX_BC7_UNORM), 98u);
    CHECK_EQ(nue_tex_format_to_dxgi(NUE_TEX_RGBA8_UNORM), 28u);
    CHECK_EQ(nue_tex_format_to_bgfx(NUE_TEX_BC1_UNORM, nullptr), 0u);
    nue_bool s = 0;
    CHECK_EQ(nue_tex_format_to_bgfx(NUE_TEX_ASTC_UNORM(4), &s), 27u + 4u); CHECK(!s);
    CHECK_EQ(nue_tex_format_to_bgfx(NUE_TEX_ASTC_SRGB(4), &s), 27u + 4u); CHECK(s);
    CHECK_EQ(nue_tex_format_to_bgfx(NUE_TEX_RGBA8_UNORM, nullptr), 74u);
    CHECK_EQ(nue_tex_format_to_bgfx(NUE_TEX_ASTC_4x4_HDR, nullptr), NUE_TEX_BGFX_FORMAT_COUNT);
}

TEST_CASE(caps_presets) {
    nue_tex_caps c;
    for (int cls = 0; cls < NUE_TEX_DEVICE_COUNT; ++cls) {
        CHECK(nue_tex_caps_for_class(nue_tex_device_class(cls), &c) >= 0);
        CHECK(nue_tex_caps_can_use(&c, NUE_TEX_RGBA8_UNORM) || cls == NUE_TEX_DEVICE_APPLE_PVRTC);
        CHECK_EQ(nue_tex_device_class_from_name(nue_tex_device_class_name(nue_tex_device_class(cls))), cls);
    }
    nue_tex_caps_for_class(NUE_TEX_DEVICE_DESKTOP_BC, &c);
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_BC1_UNORM)); CHECK(nue_tex_caps_can_use(&c, NUE_TEX_BC7_SRGB));
    CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_ETC2_RGB8)); CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_ASTC_4x4_UNORM));
    nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &c);
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_ETC2_RGBA8)); CHECK(nue_tex_caps_can_use(&c, NUE_TEX_ASTC_UNORM(7))); CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_BC1_UNORM));
    nue_tex_caps_for_class(NUE_TEX_DEVICE_APPLE_ASTC, &c);
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_PVRTC1_4BPP)); CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_BC7_UNORM)); CHECK(!c.astc_hdr);
    nue_tex_caps_for_class(NUE_TEX_DEVICE_DESKTOP_BC_ETC2, &c);
    CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_ETC2_RGB8));                                   // emulated: no memory saved
    CHECK(nue_tex_caps_get_format(&c, NUE_TEX_ETC2_RGB8) & NUE_TEX_FCAP_EMULATED);
}

TEST_CASE(caps_from_bgfx) {
    uint16_t bg[NUE_TEX_BGFX_FORMAT_COUNT] = {};
    bg[0] = 0x1 | 0x2;            // BC1: 2D + sRGB
    bg[27] = 0x1;                 // ASTC 4x4 2D, no sRGB
    bg[11] = 0x1 | 0x4;           // ETC2 emulated
    bg[74] = 0x1 | 0x2 | 0x1000;  // RGBA8
    nue_tex_caps c; NUE_STRUCT_INIT(&c);
    CHECK_EQ(nue_tex_caps_from_bgfx(&c, bg, NUE_TEX_BGFX_FORMAT_COUNT, 8192), NUE_OK);
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_BC1_UNORM));
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_BC1_SRGB));                                    // sRGB twin derived
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_ASTC_4x4_UNORM));
    CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_ASTC_4x4_SRGB));                              // bgfx said no sRGB
    CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_ETC2_RGB8));
    CHECK(nue_tex_caps_get_format(&c, NUE_TEX_RGBA8_UNORM) & NUE_TEX_FCAP_RENDER);
    CHECK_EQ(c.max_size_2d, 8192u);
    char text[4096]; size_t need = 0;
    CHECK_EQ(nue_tex_caps_to_string(&c, text, sizeof text, &need), NUE_OK);
    CHECK(need > 20 && std::string(text).find("BC1_UNORM") != std::string::npos);
    CHECK_EQ(nue_tex_caps_from_bgfx(&c, bg, 5, 4096), NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(caps_from_engine_gpu_probe) {
    nue_gpu_caps g; NUE_STRUCT_INIT(&g);
    g.probed = NUE_TRUE;
    const nue_format_support basic = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_FILTER | NUE_FMTSUP_MIPMAPS;
    g.format[NUE_FMT_RGBA8] = basic | NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_SRGB;
    g.format[NUE_FMT_ETC2_RGB] = basic | NUE_FMTSUP_SRGB;
    g.format[NUE_FMT_ASTC_LDR] = basic | NUE_FMTSUP_EMULATED;                       // the driver decompresses ASTC at upload
    g.format[NUE_FMT_BC1] = basic;                                                  // no sRGB variant: BC1_SRGB must not be offered
    g.max_texture_2d = 4096; g.npot_mipmaps = NUE_TRUE; g.dedicated_memory_bytes = 123456;
    nue_tex_caps c; NUE_STRUCT_INIT(&c);
    CHECK_EQ(nue_tex_caps_from_gpu_caps(&c, &g), NUE_OK);
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_ETC2_RGB8)); CHECK(nue_tex_caps_can_use(&c, NUE_TEX_ETC2_RGB8_SRGB));
    CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_ASTC_4x4_UNORM));                       // emulated => not usable for saving memory
    CHECK(nue_tex_caps_get_format(&c, NUE_TEX_ASTC_4x4_UNORM) & NUE_TEX_FCAP_EMULATED);
    CHECK(nue_tex_caps_can_use(&c, NUE_TEX_BC1_UNORM)); CHECK(!nue_tex_caps_can_use(&c, NUE_TEX_BC1_SRGB));
    CHECK(nue_tex_caps_get_format(&c, NUE_TEX_RGBA8_UNORM) & NUE_TEX_FCAP_RENDER);
    CHECK_EQ(c.max_size_2d, 4096u); CHECK_EQ(c.available_memory_bytes, 123456u);
    CHECK_EQ(nue_tex_caps_from_gpu_caps(&c, nullptr), NUE_ERR_INVALID_ARGUMENT);
}

TEST_MAIN_BEGIN
    RUN(registry_is_consistent);
    RUN(geometry);
    RUN(mappings_round_trip);
    RUN(d3d9_known_values);
    RUN(vulkan_dxgi_known_values);
    RUN(caps_presets);
    RUN(caps_from_bgfx);
    RUN(caps_from_engine_gpu_probe);
TEST_MAIN_END
