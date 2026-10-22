// Round trip decode(encode(x)) per encodable format on synthetic images, with PSNR floors that catch channel swaps and broken blocks.
#include "tex_common.hpp"
#include <cstdio>

using namespace texttest;

struct Case { nue_tex_format fmt; double min_rgb; double min_alpha; bool has_alpha; bool square_pot; };

static void run_case(const Case& c, uint32_t w, uint32_t h, nue_tex_quality q) {
    const auto src = synth_rgba(w, h, 3, c.has_alpha);
    nue_tex_encode_params p; nue_tex_encode_params_default(&p); p.quality = q;
    const uint64_t sz = nue_tex_level_size(c.fmt, w, h, 1);
    std::vector<uint8_t> enc(size_t(sz) + 16, 0xCD);
    nue_result r = nue_tex_encode_level(c.fmt, w, h, NUE_TEX_RGBA8_UNORM, src.data(), size_t(w) * 4, enc.data(), size_t(sz), &p);
    CHECK_EQ(r, NUE_OK);
    if (r < 0) return;
    CHECK_EQ(enc[size_t(sz)], 0xCD);                       // no write past the end
    std::vector<uint8_t> dec(size_t(w) * h * 4);
    r = nue_tex_decode_level(c.fmt, w, h, enc.data(), size_t(sz), NUE_TEX_RGBA8_UNORM, dec.data(), dec.size(), nullptr);
    CHECK_EQ(r, NUE_OK);
    if (r < 0) return;
    const double prgb = psnr_u8(src.data(), dec.data(), size_t(w) * h, 3);
    std::printf("  %-18s %3ux%-3u q%d  rgb %.1f dB", nue_tex_format_name(c.fmt), w, h, (int)q, prgb);
    const bool big = w >= 64 && h >= 64;
    CHECK(prgb >= (big ? c.min_rgb : 10.0));
    if (c.has_alpha && c.min_alpha > 0) {
        double se = 0;
        for (size_t i = 0; i < size_t(w) * h; ++i) { const double d = double(src[i * 4 + 3]) - dec[i * 4 + 3]; se += d * d; }
        const double pa = se == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / (se / (double(w) * h)));
        std::printf("  alpha %.1f dB", pa);
        CHECK(pa >= (big ? c.min_alpha : 15.0));
    }
    std::printf("\n");
}

TEST_CASE(round_trips_opaque_and_alpha) {
    const Case cases[] = {
        // floors are for the 64x64 image (measured on this synthetic noisy image minus ~1.5 dB); odd sizes only need to stay sane (see run_case)
        {NUE_TEX_BC1_UNORM, 23, 0, false, false},      {NUE_TEX_BC3_UNORM, 23, 30, true, false},    {NUE_TEX_BC7_UNORM, 32, 30, true, false},
        {NUE_TEX_ETC1_RGB8, 23, 0, false, false},      {NUE_TEX_ETC2_RGB8, 22, 0, false, false},    {NUE_TEX_ETC2_RGBA8, 22, 30, true, false},
        {NUE_TEX_ASTC_4x4_UNORM, 30, 30, true, false}, {NUE_TEX_ASTC_UNORM(4), 24, 24, true, false}, {NUE_TEX_ASTC_UNORM(7), 20, 20, true, false},
        {NUE_TEX_ASTC_12x12_UNORM, 16, 16, true, false}, {NUE_TEX_PVRTC1_4BPP, 21, 0, false, true},
    };
    for (const Case& c : cases) {
        run_case(c, 64, 64, NUE_TEX_QUALITY_NORMAL);
        if (!c.square_pot) { run_case(c, 37, 29, NUE_TEX_QUALITY_FAST); run_case(c, 5, 3, NUE_TEX_QUALITY_FAST); }
    }
}

TEST_CASE(single_and_dual_channel) {
    const uint32_t w = 48, h = 40;
    auto src = synth_rgba(w, h, 5, false);
    for (nue_tex_format f : {NUE_TEX_BC4_UNORM, NUE_TEX_EAC_R11_UNORM, NUE_TEX_BC5_UNORM, NUE_TEX_EAC_RG11_UNORM}) {
        const bool two = f == NUE_TEX_BC5_UNORM || f == NUE_TEX_EAC_RG11_UNORM;
        const uint64_t sz = nue_tex_level_size(f, w, h, 1);
        std::vector<uint8_t> enc{std::vector<uint8_t>(size_t(sz))};
        std::vector<uint8_t> dec(size_t(w) * h * 4);
        nue_tex_encode_params p; nue_tex_encode_params_default(&p);
        CHECK_EQ(nue_tex_encode_level(f, w, h, NUE_TEX_RGBA8_UNORM, src.data(), w * 4, enc.data(), enc.size(), &p), NUE_OK);
        CHECK_EQ(nue_tex_decode_level(f, w, h, enc.data(), enc.size(), NUE_TEX_RGBA8_UNORM, dec.data(), dec.size(), nullptr), NUE_OK);
        double se = 0;
        for (size_t i = 0; i < size_t(w) * h; ++i)
            for (int k = 0; k < (two ? 2 : 1); ++k) { const double d = double(src[i * 4 + k]) - dec[i * 4 + k]; se += d * d; }
        const double psnr = 10.0 * std::log10(255.0 * 255.0 / (se / (double(w) * h * (two ? 2 : 1))));
        std::printf("  %-18s %ux%u  %.1f dB\n", nue_tex_format_name(f), w, h, psnr);
        CHECK(psnr >= 35.0);
    }
}

TEST_CASE(bc_decoders_know_answers) {
    // BC1: c0 = pure red (0xF800), c1 = pure blue (0x001F), every index 0 -> red; index 1 -> blue
    uint8_t bc1[8] = {0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t out[64];
    CHECK_EQ(nue_tex_decode_level(NUE_TEX_BC1_UNORM, 4, 4, bc1, 8, NUE_TEX_RGBA8_UNORM, out, 64, nullptr), NUE_OK);
    CHECK_EQ(out[0], 255); CHECK_EQ(out[1], 0); CHECK_EQ(out[2], 0); CHECK_EQ(out[3], 255);
    bc1[4] = 0x55;       // indices 01 01 01 01 for the first row -> c1
    CHECK_EQ(nue_tex_decode_level(NUE_TEX_BC1_UNORM, 4, 4, bc1, 8, NUE_TEX_RGBA8_UNORM, out, 64, nullptr), NUE_OK);
    CHECK_EQ(out[0], 0); CHECK_EQ(out[1], 0); CHECK_EQ(out[2], 255);
    CHECK_EQ(out[16 + 2], 0 + 0);        // second row still index 0 = red -> blue channel 0
    // BC1 3-colour mode: c0 <= c1, index 3 is transparent black
    uint8_t bc1t[8] = {0x1F, 0x00, 0x00, 0xF8, 0xFF, 0x00, 0x00, 0x00};
    CHECK_EQ(nue_tex_decode_level(NUE_TEX_BC1_UNORM, 4, 4, bc1t, 8, NUE_TEX_RGBA8_UNORM, out, 64, nullptr), NUE_OK);
    CHECK_EQ(out[3], 0);
    nue_tex_scan_result sr; NUE_STRUCT_INIT(&sr);
    CHECK_EQ(nue_tex_scan_level(NUE_TEX_BC1_UNORM, 4, 4, bc1t, 8, &sr), NUE_OK);
    CHECK(sr.uses_punchthrough);
    CHECK_EQ(sr.alpha_info, NUE_TEX_ALPHA_INFO_BINARY);
    CHECK_EQ(nue_tex_scan_level(NUE_TEX_BC1_UNORM, 4, 4, bc1, 8, &sr), NUE_OK);
    CHECK(!sr.uses_punchthrough);
    CHECK_EQ(sr.alpha_info, NUE_TEX_ALPHA_INFO_OPAQUE);
    // BC3 alpha: alpha0 = 255, alpha1 = 0, all indices 0 -> opaque
    uint8_t bc3[16] = {255, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
    CHECK_EQ(nue_tex_decode_level(NUE_TEX_BC3_UNORM, 4, 4, bc3, 16, NUE_TEX_RGBA8_UNORM, out, 64, nullptr), NUE_OK);
    CHECK_EQ(out[0], 255); CHECK_EQ(out[3], 255);
}

TEST_CASE(srgb_variants_decode_to_encoded_bytes_and_linear_floats) {
    uint8_t bc1[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};       // solid red
    float f[16 * 4];
    CHECK_EQ(nue_tex_decode_level(NUE_TEX_BC1_SRGB, 4, 4, bc1, 8, NUE_TEX_RGBA32_FLOAT, f, sizeof f, nullptr), NUE_OK);
    CHECK_NEAR(f[0], 1.0, 1e-4);
    uint8_t px[4] = {128, 128, 128, 255};       // mid grey, sRGB encoded
    float lin[4];
    CHECK_EQ(nue_tex_unpack_pixels(NUE_TEX_RGBA8_SRGB, px, 1, NUE_TEX_RGBA32_FLOAT, lin), NUE_OK);
    CHECK_NEAR(lin[0], 0.2158, 0.002);
    uint8_t back[4];
    CHECK_EQ(nue_tex_pack_pixels(NUE_TEX_RGBA8_SRGB, lin, 1, NUE_TEX_RGBA32_FLOAT, back), NUE_OK);
    CHECK_EQ(back[0], 128);
}

TEST_MAIN_BEGIN
    RUN(round_trips_opaque_and_alpha);
    RUN(single_and_dual_channel);
    RUN(bc_decoders_know_answers);
    RUN(srgb_variants_decode_to_encoded_bytes_and_linear_floats);
TEST_MAIN_END
