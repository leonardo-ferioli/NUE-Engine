// A .nuepack reached through the engine VFS ("user:" root of the null-backend engine): asynchronous reads, and the texture module consuming the variant a
// chosen GPU family can sample.
#include "asset_common.hpp"
#include "test_env.h"

using namespace asstest;

static nue_test::Engine& engine() { static nue_test::Engine e; return e; }

static nue_tex_image gradient(uint32_t w, uint32_t h, uint32_t mips) {
    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    d.format = NUE_TEX_RGBA8_UNORM; d.width = w; d.height = h; d.mip_levels = mips;
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &im), NUE_OK);
    nue_tex_image_desc g;
    NUE_STRUCT_INIT(&g);
    nue_tex_image_get_desc(im, &g);
    for (uint32_t l = 0; l < g.mip_levels; ++l) {
        nue_tex_level_view v;
        NUE_STRUCT_INIT(&v);
        nue_tex_image_get_level(im, l, 0, 0, &v);
        uint8_t* p = static_cast<uint8_t*>(v.data);
        for (uint32_t y = 0; y < v.height; ++y)
            for (uint32_t x = 0; x < v.width; ++x) {
                uint8_t* px = p + (size_t(y) * v.width + x) * 4;
                px[0] = uint8_t(x * 255 / std::max(1u, v.width - 1));
                px[1] = uint8_t(y * 255 / std::max(1u, v.height - 1));
                px[2] = uint8_t(128 + 100 * ((x / 8 + y / 8) & 1) / 2);
                px[3] = 255;
            }
    }
    return im;
}

static void NUE_CALL on_done(void* user, nue_io_request, nue_result r, size_t bytes) { auto* v = static_cast<std::pair<int, size_t>*>(user); v->first = r >= 0 ? 1 : -1; v->second = bytes; }

TEST_CASE(pack_through_the_vfs_for_each_gpu_family) {
    nue_test::Engine& eng = engine();
    nue_assets_context ctx = nullptr;
    CHECK_EQ(nue_assets_context_create(nullptr, &ctx), NUE_OK);

    // build the package and put it in the engine's writable user root
    nue_pack_builder b = nullptr;
    CHECK_EQ(nue_pack_builder_create(ctx, nullptr, &b), NUE_OK);
    nue_tex_image src = gradient(128, 128, 8);
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    o.variants = (1u << NUE_PACK_VARIANT_BC) | (1u << NUE_PACK_VARIANT_ASTC) | (1u << NUE_PACK_VARIANT_ETC2) | (1u << NUE_PACK_VARIANT_PVRTC) | (1u << NUE_PACK_VARIANT_RGBA8);
    o.quality = NUE_TEX_QUALITY_NORMAL;
    CHECK_EQ(nue_pack_builder_add_image(b, "ui/gradient", src, &o), NUE_OK);
    CHECK_EQ(nue_pack_builder_write(b, "user:packs/test.nuepack", nullptr, nullptr), NUE_OK);      // written through the VFS
    nue_pack_builder_destroy(b);
    CHECK(nue_vfs_exists("user:packs/test.nuepack"));
    CHECK(nue_vfs_exists("USER:PACKS\\TEST.NUEPACK"));                                               // the VFS path rules apply (case, separators)

    struct Case { nue_tex_device_class dc; nue_pack_variant expect; double min_psnr; };
    const Case cases[] = {
        {NUE_TEX_DEVICE_DESKTOP_BC, NUE_PACK_VARIANT_BC, 30},
        {NUE_TEX_DEVICE_ANDROID_ASTC, NUE_PACK_VARIANT_ASTC, 35},
        {NUE_TEX_DEVICE_ANDROID_ETC2, NUE_PACK_VARIANT_ETC2, 28},
        {NUE_TEX_DEVICE_APPLE_PVRTC, NUE_PACK_VARIANT_PVRTC, 24},
        {NUE_TEX_DEVICE_SOFTWARE, NUE_PACK_VARIANT_RGBA8, 90},
    };
    for (const Case& c : cases) {
        nue_tex_caps caps;
        NUE_STRUCT_INIT(&caps);
        CHECK_EQ(nue_tex_caps_for_class(c.dc, &caps), NUE_OK);
        CHECK_EQ(nue_pack_variant_for_caps(&caps), c.expect);
        nue_pack_mount m = nullptr;
        CHECK_EQ(nue_pack_mount_create_for_caps(ctx, "user:packs/test.nuepack", &caps, &m), NUE_OK);
        CHECK(nue_pack_mount_exists(m, "ui/gradient"));
        uint64_t size = 0;
        CHECK_EQ(nue_pack_mount_size(m, "ui/gradient", &size), NUE_OK);
        nue_pack_file f = nullptr;
        CHECK_EQ(nue_pack_file_open(m, "ui/gradient", &f), NUE_OK);
        CHECK_EQ(nue_pack_file_size(f), size);

        // asynchronous read of the whole texture, in two halves, straight from the file
        std::vector<uint8_t> bytes(size_t(size), 0);
        const size_t half = bytes.size() / 2;
        std::pair<int, size_t> d1{0, 0}, d2{0, 0};
        nue_io_request r1 = 0, r2 = 0;
        CHECK(nue_pack_file_read_async(f, 0, bytes.data(), half, on_done, &d1, &r1) >= 0);
        CHECK(nue_pack_file_read_async(f, half, bytes.data() + half, bytes.size() - half, on_done, &d2, &r2) >= 0);
        size_t n1 = 0, n2 = 0;
        CHECK(r1 != 0 && r2 != 0);                                      // real requests: the pack lives in a file
        CHECK(nue_io_wait(r1, 5000000000ll, &n1) >= 0);
        CHECK(nue_io_wait(r2, 5000000000ll, &n2) >= 0);
        nue_io_release(r1); nue_io_release(r2);
        CHECK_EQ(n1 + n2, bytes.size());
        nue_pack_file_close(f);
        nue_pack_mount_destroy(m);

        // the bytes are the entry the pack holds for this variant, and the texture module reads them as that variant's format
        nue_pack p = nullptr;
        CHECK_EQ(nue_pack_open_file(ctx, "user:packs/test.nuepack", 0, &p), NUE_OK);
        uint32_t e = nue_pack_entry_find(p, "ui/gradient", c.expect);
        nue_pack_entry_info ei;
        NUE_STRUCT_INIT(&ei);
        nue_pack_entry_get(p, e, &ei);
        CHECK_EQ(ei.variant, uint32_t(c.expect));
        const void* ref = nullptr;
        size_t rn = 0;
        nue_bool owned = NUE_FALSE;
        CHECK_EQ(nue_pack_entry_data(p, e, &ref, &rn, &owned), NUE_OK);
        CHECK(rn == bytes.size() && std::memcmp(ref, bytes.data(), rn) == 0);
        if (owned) nue_free(const_cast<void*>(ref));
        nue_tex_image im = nullptr;
        CHECK_EQ(nue_pack_entry_image(p, e, &im), NUE_OK);
        nue_tex_image_desc id;
        NUE_STRUCT_INIT(&id);
        nue_tex_image_get_desc(im, &id);
        CHECK(nue_tex_caps_can_use(&caps, id.format));                  // the device can sample what the pack gave it
        CHECK_EQ(id.mip_levels, 8u);
        // decode what the GPU would see and compare with the original pixels
        nue_tex_image back = nullptr;
        nue_tex_encode_params ep;
        nue_tex_encode_params_default(&ep);
        CHECK_EQ(nue_tex_image_convert(im, NUE_TEX_RGBA8_UNORM, 0, &ep, nullptr, &back), NUE_OK);
        nue_tex_metrics mt;
        NUE_STRUCT_INIT(&mt);
        CHECK_EQ(nue_tex_compare_images(src, back, &mt), NUE_OK);
        std::printf("  %-26s -> %-6s %-14s %6.1f dB  (%llu bytes for %llu of RGBA8)\n", nue_tex_device_class_name(c.dc), nue_pack_variant_name(c.expect), nue_tex_format_name(id.format), mt.psnr_db,
                    (unsigned long long)ei.raw_size, (unsigned long long)nue_tex_image_total_size(src));
        CHECK(mt.psnr_db >= c.min_psnr);
        nue_tex_image_destroy(back);
        nue_tex_image_destroy(im);
        nue_pack_close(p);
    }
    nue_tex_image_destroy(src);
    (void)eng;
    nue_assets_context_destroy(ctx);
}

TEST_MAIN_BEGIN
    RUN(pack_through_the_vfs_for_each_gpu_family);
TEST_MAIN_END
