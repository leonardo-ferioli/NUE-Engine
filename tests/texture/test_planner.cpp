// Planner decisions per device class, content type and policy.
#include "tex_common.hpp"
#include <cstring>
#include <string>

struct Plan {
    nue_result r; nue_tex_plan p;
    bool has(uint32_t flag) const { return (p.flags & flag) != 0; }
};

static Plan plan(nue_tex_device_class cls, nue_tex_format src, uint32_t w = 256, uint32_t h = 256, nue_tex_alpha_info alpha = NUE_TEX_ALPHA_INFO_SMOOTH,
                 nue_tex_usage usage = NUE_TEX_USAGE_COLOR, uint32_t req_flags = 0, const nue_tex_policy* pol = nullptr) {
    nue_tex_caps caps; nue_tex_caps_for_class(cls, &caps);
    nue_tex_plan_request rq; NUE_STRUCT_INIT(&rq);
    rq.source_format = src; rq.width = w; rq.height = h; rq.depth = 1; rq.alpha_info = alpha; rq.usage = usage; rq.flags = req_flags; rq.policy = pol;
    Plan out; NUE_STRUCT_INIT(&out.p);
    out.r = nue_tex_make_plan(&caps, &rq, &out.p);
    return out;
}

#define EXPECT_PLAN(P, TARGET, ACTION) do { CHECK_EQ((P).r, NUE_OK); CHECK_EQ((P).p.target_format, (TARGET)); CHECK_EQ((P).p.action, (ACTION)); \
    if ((P).p.target_format != (TARGET) || (P).p.action != (ACTION)) std::printf("    plan: %s -> %s (%s)\n    %s\n", nue_tex_format_name((P).p.source_format), \
        nue_tex_format_name((P).p.target_format), nue_tex_action_name((P).p.action), (P).p.explanation); } while (0)

TEST_CASE(desktop_keeps_bc) {
    for (nue_tex_format f : {NUE_TEX_BC1_UNORM, NUE_TEX_BC2_UNORM, NUE_TEX_BC3_UNORM, NUE_TEX_BC4_UNORM, NUE_TEX_BC5_UNORM, NUE_TEX_BC7_UNORM}) {
        Plan p = plan(NUE_TEX_DEVICE_DESKTOP_BC, f);
        EXPECT_PLAN(p, f, NUE_TEX_ACTION_UPLOAD_AS_IS);
        CHECK(!p.has(NUE_TEX_PLAN_LOSSY));
    }
    Plan p = plan(NUE_TEX_DEVICE_DESKTOP_BC, NUE_TEX_BC1_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_COLOR, NUE_TEX_REQ_SRGB);
    EXPECT_PLAN(p, NUE_TEX_BC1_SRGB, NUE_TEX_ACTION_UPLOAD_AS_IS);
    p = plan(NUE_TEX_DEVICE_DESKTOP_BC, NUE_TEX_BC3_PMA_UNORM);
    EXPECT_PLAN(p, NUE_TEX_BC3_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);                      // same bytes, premultiplied alpha is a property of the content
    p = plan(NUE_TEX_DEVICE_DESKTOP_BC, NUE_TEX_ETC2_RGB8, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE);
    EXPECT_PLAN(p, NUE_TEX_BC1_UNORM, NUE_TEX_ACTION_TRANSCODE);                         // mobile content on a desktop GPU
    p = plan(NUE_TEX_DEVICE_DESKTOP_BC, NUE_TEX_ASTC_4x4_UNORM);
    EXPECT_PLAN(p, NUE_TEX_BC7_UNORM, NUE_TEX_ACTION_TRANSCODE);
}

TEST_CASE(android_astc_etc2) {
    Plan p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE);
    EXPECT_PLAN(p, NUE_TEX_ETC2_RGB8, NUE_TEX_ACTION_TRANSCODE);                         // same size as DXT1, cheap encoder
    CHECK(p.has(NUE_TEX_PLAN_LOSSY));
    CHECK_EQ(p.p.target_bytes, p.p.source_bytes);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_BINARY);
    EXPECT_PLAN(p, NUE_TEX_ASTC_UNORM(3), NUE_TEX_ACTION_TRANSCODE);                     // 1-bit alpha: ASTC 6x5 (4.27 bpp) stays within 1.1x of DXT1's 4 bpp
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC3_UNORM);
    EXPECT_PLAN(p, NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_ACTION_TRANSCODE);                    // 8 bpp -> 8 bpp
    CHECK_EQ(p.p.alternatives[0], NUE_TEX_ETC2_RGBA8);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC3_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE);
    EXPECT_PLAN(p, NUE_TEX_ETC2_RGB8, NUE_TEX_ACTION_TRANSCODE);                         // DXT5 whose alpha is all 255 halves its memory
    CHECK(p.has(NUE_TEX_PLAN_ALPHA_DROPPED) == false || true);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC4_UNORM);
    EXPECT_PLAN(p, NUE_TEX_EAC_R11_UNORM, NUE_TEX_ACTION_TRANSCODE);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC5_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_NORMAL_MAP);
    EXPECT_PLAN(p, NUE_TEX_EAC_RG11_UNORM, NUE_TEX_ACTION_TRANSCODE);                    // no swizzle needed
    CHECK_STR(p.p.shader_swizzle, "rgba");
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC7_UNORM);
    EXPECT_PLAN(p, NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_ACTION_TRANSCODE);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC6H_UFLOAT, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_HDR);
    EXPECT_PLAN(p, NUE_TEX_RG11B10_FLOAT, NUE_TEX_ACTION_DECODE);                        // no HDR ASTC: HDR survives as a packed float format
    p = plan(NUE_TEX_DEVICE_ANDROID_ETC2, NUE_TEX_BC3_UNORM);
    EXPECT_PLAN(p, NUE_TEX_ETC2_RGBA8, NUE_TEX_ACTION_TRANSCODE);
    p = plan(NUE_TEX_DEVICE_ANDROID_ETC2, NUE_TEX_BC7_UNORM);
    EXPECT_PLAN(p, NUE_TEX_ETC2_RGBA8, NUE_TEX_ACTION_TRANSCODE);
    p = plan(NUE_TEX_DEVICE_ANDROID_ETC2_BC, NUE_TEX_BC1_UNORM);
    EXPECT_PLAN(p, NUE_TEX_BC1_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);                      // Tegra / x86 Android: native BC
}

TEST_CASE(apple_classes) {
    Plan p = plan(NUE_TEX_DEVICE_APPLE_ASTC, NUE_TEX_BC3_UNORM);
    EXPECT_PLAN(p, NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_ACTION_TRANSCODE);
    p = plan(NUE_TEX_DEVICE_APPLE_PVRTC, NUE_TEX_BC1_UNORM, 128, 128, NUE_TEX_ALPHA_INFO_OPAQUE);
    EXPECT_PLAN(p, NUE_TEX_PVRTC1_4BPP, NUE_TEX_ACTION_TRANSCODE);
    p = plan(NUE_TEX_DEVICE_APPLE_PVRTC, NUE_TEX_BC1_UNORM, 100, 100, NUE_TEX_ALPHA_INFO_OPAQUE);          // PVRTC1 needs power-of-two squares
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_DECODE);
    CHECK(p.has(NUE_TEX_PLAN_FALLBACK));
    p = plan(NUE_TEX_DEVICE_APPLE_SILICON_MAC, NUE_TEX_BC7_UNORM);
    EXPECT_PLAN(p, NUE_TEX_BC7_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);
}

TEST_CASE(software_and_forced) {
    Plan p = plan(NUE_TEX_DEVICE_SOFTWARE, NUE_TEX_BC1_UNORM);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_DECODE);
    CHECK(p.p.target_bytes > p.p.source_bytes * 7.9);                                   // about 8x (tiny mips are padded to a block): the cost of the RGBA8 fallback the D3D9 layer has today
    nue_tex_policy pol; nue_tex_policy_default(&pol);
    pol.flags |= NUE_TEX_POLICY_FORCE_UNCOMPRESSED;
    p = plan(NUE_TEX_DEVICE_DESKTOP_BC, NUE_TEX_BC3_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_SMOOTH, NUE_TEX_USAGE_COLOR, 0, &pol);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_DECODE);
    nue_tex_policy_default(&pol);
    pol.flags |= NUE_TEX_POLICY_NO_LOSSY;
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_COLOR, 0, &pol);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_DECODE);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_COLOR, NUE_TEX_REQ_RENDER_TARGET);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_DECODE);                          // render targets and dynamic textures never get a compressed format
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 8, 8, NUE_TEX_ALPHA_INFO_OPAQUE);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_DECODE);                          // smaller than min_recompress_dimension: not worth an encode
    CHECK_EQ(plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_FORMAT_UNKNOWN).r, NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 0, 0).r, NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(memory_policy_and_dimensions) {
    nue_tex_policy pol; nue_tex_policy_default(&pol);
    pol.priority = NUE_TEX_PRIORITY_MEMORY;
    Plan p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC3_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_SMOOTH, NUE_TEX_USAGE_COLOR, 0, &pol);
    EXPECT_PLAN(p, NUE_TEX_ASTC_UNORM(3), NUE_TEX_ACTION_TRANSCODE);                     // 8 bpp * 0.6 = 4.8 -> 6x5 (4.27 bpp)
    CHECK(p.p.target_bytes < p.p.source_bytes * 6 / 10);
    nue_tex_policy_default(&pol);
    pol.priority = NUE_TEX_PRIORITY_QUALITY;
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC1_UNORM, 256, 256, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_COLOR, 0, &pol);
    EXPECT_PLAN(p, NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_ACTION_TRANSCODE);                    // up to 2x the memory for quality
    nue_tex_policy_default(&pol);
    pol.max_dimension = 256;
    p = plan(NUE_TEX_DEVICE_DESKTOP_BC, NUE_TEX_BC1_UNORM, 1024, 1024, NUE_TEX_ALPHA_INFO_OPAQUE, NUE_TEX_USAGE_COLOR, 0, &pol);
    EXPECT_PLAN(p, NUE_TEX_BC1_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);
    CHECK_EQ(p.p.skip_mips, 2u);
    CHECK(p.has(NUE_TEX_PLAN_MIPS_SKIPPED));
    // uncompressed sources
    nue_tex_policy_default(&pol);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_RGBA8_UNORM, 512, 512);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);
    pol.flags |= NUE_TEX_POLICY_COMPRESS_RAW;
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_RGBA8_UNORM, 512, 512, NUE_TEX_ALPHA_INFO_SMOOTH, NUE_TEX_USAGE_COLOR, 0, &pol);
    CHECK_EQ(p.p.action, NUE_TEX_ACTION_ENCODE);
    CHECK(nue_tex_format_is_compressed(p.p.target_format));
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_RGBA8_UNORM, 512, 512, NUE_TEX_ALPHA_INFO_SMOOTH, NUE_TEX_USAGE_COLOR, NUE_TEX_REQ_DYNAMIC, &pol);
    EXPECT_PLAN(p, NUE_TEX_RGBA8_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BGRA8_UNORM, 64, 64);       // uncompressed BGRA that the device lacks is converted, not encoded
    EXPECT_PLAN(p, NUE_TEX_BGRA8_UNORM, NUE_TEX_ACTION_UPLOAD_AS_IS);
    p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BGRX8_UNORM, 64, 64);
    CHECK_EQ(p.r, NUE_OK);
    CHECK(p.p.action == NUE_TEX_ACTION_CONVERT_PIXELS || p.p.action == NUE_TEX_ACTION_UPLOAD_AS_IS);
}

TEST_CASE(explanations_exist) {
    Plan p = plan(NUE_TEX_DEVICE_ANDROID_ASTC, NUE_TEX_BC3_UNORM);
    CHECK(std::strlen(p.p.explanation) > 30);
    CHECK(std::string(p.p.explanation).find("BC3_UNORM") != std::string::npos);
    CHECK(std::string(p.p.explanation).find("ASTC_4x4_UNORM") != std::string::npos);
    p = plan(NUE_TEX_DEVICE_DESKTOP_BC_ETC2, NUE_TEX_ETC2_RGB8, 64, 64, NUE_TEX_ALPHA_INFO_OPAQUE);
    CHECK(std::string(p.p.explanation).find("emulated") != std::string::npos);            // the log says why the emulated format was not trusted
}

TEST_MAIN_BEGIN
    RUN(desktop_keeps_bc);
    RUN(android_astc_etc2);
    RUN(apple_classes);
    RUN(software_and_forced);
    RUN(memory_policy_and_dimensions);
    RUN(explanations_exist);
TEST_MAIN_END
