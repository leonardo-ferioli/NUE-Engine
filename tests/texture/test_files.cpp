// DDS / KTX2 / PNG containers: round trips with mips, cube maps, arrays, volumes; legacy D3D9 DDS headers; corrupt input.
#include "tex_common.hpp"
#include <cstring>
#include <string>

using namespace texttest;

static nue_tex_image make_image(nue_tex_format f, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers = 1, uint32_t faces = 1, uint32_t depth = 1) {
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d);
    d.format = f; d.width = w; d.height = h; d.depth = depth; d.mip_levels = mips; d.layers = layers; d.faces = faces;
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &im), NUE_OK);
    if (!im) return nullptr;
    // deterministic content
    nue_tex_image_desc g; NUE_STRUCT_INIT(&g); nue_tex_image_get_desc(im, &g);
    uint32_t s = 12345;
    for (uint32_t layer = 0; layer < g.layers; ++layer)
        for (uint32_t face = 0; face < g.faces; ++face)
            for (uint32_t l = 0; l < g.mip_levels; ++l) {
                nue_tex_level_view v; NUE_STRUCT_INIT(&v);
                nue_tex_image_get_level(im, l, layer, face, &v);
                for (uint64_t i = 0; i < v.size; ++i) static_cast<uint8_t*>(v.data)[i] = uint8_t(rng_next(s));
            }
    return im;
}

static bool same_content(nue_tex_image a, nue_tex_image b) {
    nue_tex_image_desc da, db; NUE_STRUCT_INIT(&da); NUE_STRUCT_INIT(&db);
    nue_tex_image_get_desc(a, &da); nue_tex_image_get_desc(b, &db);
    if (da.format != db.format || da.width != db.width || da.height != db.height || da.depth != db.depth || da.mip_levels != db.mip_levels || da.layers != db.layers || da.faces != db.faces) return false;
    for (uint32_t layer = 0; layer < da.layers; ++layer)
        for (uint32_t face = 0; face < da.faces; ++face)
            for (uint32_t l = 0; l < da.mip_levels; ++l) {
                nue_tex_level_view va, vb; NUE_STRUCT_INIT(&va); NUE_STRUCT_INIT(&vb);
                nue_tex_image_get_level(a, l, layer, face, &va); nue_tex_image_get_level(b, l, layer, face, &vb);
                if (va.size != vb.size || std::memcmp(va.data, vb.data, size_t(va.size)) != 0) return false;
            }
    return true;
}

static void round_trip(nue_tex_file_kind kind, nue_tex_format f, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers = 1, uint32_t faces = 1, uint32_t depth = 1, uint32_t flags = 0) {
    nue_tex_image a = make_image(f, w, h, mips, layers, faces, depth);
    void* data = nullptr; size_t size = 0;
    nue_result r = nue_tex_image_save_memory(a, kind, flags, &data, &size);
    if (r < 0) { std::printf("  save %s kind %d failed: %d\n", nue_tex_format_name(f), (int)kind, (int)r); CHECK_EQ(r, NUE_OK); nue_tex_image_destroy(a); return; }
    CHECK_EQ(nue_tex_detect_file_kind(data, size), kind);
    nue_tex_image b = nullptr;
    r = nue_tex_image_load_memory(data, size, NUE_TEX_FILE_AUTO, 0, &b);
    CHECK_EQ(r, NUE_OK);
    if (b) { const bool same = same_content(a, b); CHECK(same); if (!same) std::printf("  mismatch: %s %ux%u mips %u layers %u faces %u depth %u kind %d\n", nue_tex_format_name(f), w, h, mips, layers, faces, depth, (int)kind); }
    // every truncation of a valid file must fail cleanly (no crash, no success)
    for (size_t cut : {size_t(0), size_t(3), size_t(40), size / 2, size - 1}) {
        nue_tex_image c = nullptr;
        if (cut < size && nue_tex_image_load_memory(data, cut, NUE_TEX_FILE_AUTO, 0, &c) >= 0) { /* a prefix may be a valid shorter file only if nothing is missing */ CHECK(false); nue_tex_image_destroy(c); }
    }
    nue_tex_free(data);
    nue_tex_image_destroy(a); nue_tex_image_destroy(b);
}

TEST_CASE(dds_round_trips) {
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC1_UNORM, 64, 64, 7);
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC3_UNORM, 37, 21, 0);                              // NPOT, full chain
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC5_UNORM, 32, 32, 3);
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BGRA8_UNORM, 16, 8, 4);
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_RGBA16_FLOAT, 8, 8, 1);
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC1_UNORM, 16, 16, 5, 1, 6);                        // cube map
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC7_UNORM, 16, 16, 3, 3, 1);                        // array (DX10 header)
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_RGBA8_UNORM, 8, 8, 4, 1, 1, 8);                     // volume
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC7_SRGB, 32, 32, 2);
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_RGB565, 8, 8, 1);
    round_trip(NUE_TEX_FILE_DDS, NUE_TEX_BC1_UNORM, 8, 8, 2, 1, 1, 1, NUE_TEX_FILE_DDS_FORCE_DX10);
}

TEST_CASE(dds_legacy_header_layout) {
    // A DXT1 file as the D3D9 era wrote it: the FOURCC header, no DX10 extension
    nue_tex_image a = make_image(NUE_TEX_BC1_UNORM, 8, 8, 1);
    void* data = nullptr; size_t size = 0;
    CHECK_EQ(nue_tex_image_save_memory(a, NUE_TEX_FILE_DDS, NUE_TEX_FILE_DDS_LEGACY_ONLY, &data, &size), NUE_OK);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    CHECK_EQ(size, 4u + 124u + 32u);                                                         // magic + header + 4 blocks of 8 bytes
    CHECK(std::memcmp(p, "DDS ", 4) == 0);
    CHECK(std::memcmp(p + 4 + 72 + 8, "DXT1", 4) == 0);                                      // ddspf.fourCC
    nue_tex_free(data);
    nue_tex_image_destroy(a);
    nue_tex_image b = make_image(NUE_TEX_BC7_UNORM, 8, 8, 1);
    CHECK_EQ(nue_tex_image_save_memory(b, NUE_TEX_FILE_DDS, NUE_TEX_FILE_DDS_LEGACY_ONLY, &data, &size), NUE_ERR_UNSUPPORTED);   // BC7 only exists in DX10 files
    nue_tex_image_destroy(b);
    // hand-built legacy files: A8R8G8B8 bit masks, and a numeric D3DFMT in the FOURCC field (A16B16G16R16F = 113)
    uint8_t file[4 + 124 + 32];
    std::memset(file, 0, sizeof file);
    std::memcpy(file, "DDS ", 4);
    auto put = [&](size_t off, uint32_t v) { std::memcpy(file + 4 + off, &v, 4); };
    put(0, 124); put(4, 0x1007); put(8, 2); put(12, 2); put(72, 32);           // size, flags, height 2, width 2, pf size
    put(72 + 4, 0x41); put(72 + 12, 32);                                         // pf.flags RGB|ALPHAPIXELS, bits
    put(72 + 16, 0x00FF0000); put(72 + 20, 0x0000FF00); put(72 + 24, 0x000000FF); put(72 + 28, 0xFF000000); put(104 + 0, 0x1000);
    nue_tex_image c = nullptr;
    CHECK_EQ(nue_tex_image_load_memory(file, 4 + 124 + 32, NUE_TEX_FILE_AUTO, 0, &c), NUE_OK);
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d);
    if (c) { nue_tex_image_get_desc(c, &d); CHECK_EQ(d.format, NUE_TEX_BGRA8_UNORM); CHECK_EQ(d.width, 2u); nue_tex_image_destroy(c); }
    put(72 + 4, 0x4); put(72 + 8, 113);
    CHECK_EQ(nue_tex_image_load_memory(file, 4 + 124 + 32, NUE_TEX_FILE_AUTO, 0, &c), NUE_OK);
    if (c) { nue_tex_image_get_desc(c, &d); CHECK_EQ(d.format, NUE_TEX_RGBA16_FLOAT); nue_tex_image_destroy(c); }
}

TEST_CASE(ktx2_round_trips) {
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_BC1_UNORM, 64, 64, 7);
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_ETC2_RGBA8, 37, 21, 0);
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_ASTC_UNORM(4), 50, 30, 0);
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_ASTC_4x4_HDR, 16, 16, 2);
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_PVRTC1_4BPP, 16, 16, 3);
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_RGBA8_SRGB, 16, 8, 4);
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_RGB8_UNORM, 7, 5, 3);                              // 3-byte texels: level alignment lcm(3,4)
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_BC7_UNORM, 16, 16, 3, 1, 6);                       // cube
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_BC3_UNORM, 16, 16, 3, 4, 1);                       // array
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_RGBA16_FLOAT, 8, 8, 4, 1, 1, 4);                   // volume
    round_trip(NUE_TEX_FILE_KTX2, NUE_TEX_RG11B10_FLOAT, 8, 8, 1);
    // an ASTC image keeps the sRGB flag through KTX2
    nue_tex_image a = make_image(NUE_TEX_ASTC_SRGB(2), 20, 20, 1), b = nullptr;
    void* data; size_t size;
    CHECK_EQ(nue_tex_image_save_memory(a, NUE_TEX_FILE_KTX2, 0, &data, &size), NUE_OK);
    CHECK_EQ(nue_tex_image_load_memory(data, size, NUE_TEX_FILE_KTX2, 0, &b), NUE_OK);
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d);
    if (b) { nue_tex_image_get_desc(b, &d); CHECK_EQ(d.format, NUE_TEX_ASTC_SRGB(2)); }
    // header says supercompression: refused, not misread
    std::vector<uint8_t> copy(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    copy[12 + 4 * 8] = 2;           // supercompressionScheme = zstd
    nue_tex_image c = nullptr;
    CHECK_EQ(nue_tex_image_load_memory(copy.data(), copy.size(), NUE_TEX_FILE_KTX2, 0, &c), NUE_ERR_UNSUPPORTED);
    nue_tex_free(data); nue_tex_image_destroy(a); nue_tex_image_destroy(b);
}

TEST_CASE(png_round_trip_and_reader) {
    const uint32_t w = 21, h = 13;
    const auto px = synth_rgba(w, h, 9);
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d); d.format = NUE_TEX_RGBA8_UNORM; d.width = w; d.height = h; d.mip_levels = 1;
    nue_tex_image a = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &a), NUE_OK);
    nue_tex_level_view v; NUE_STRUCT_INIT(&v); nue_tex_image_get_level(a, 0, 0, 0, &v);
    std::memcpy(v.data, px.data(), px.size());
    void* data; size_t size;
    CHECK_EQ(nue_tex_image_save_memory(a, NUE_TEX_FILE_PNG, 0, &data, &size), NUE_OK);
    CHECK_EQ(nue_tex_detect_file_kind(data, size), NUE_TEX_FILE_PNG);
    nue_tex_image b = nullptr;
    CHECK_EQ(nue_tex_image_load_memory(data, size, NUE_TEX_FILE_AUTO, 0, &b), NUE_OK);
    CHECK(b && same_content(a, b));
    nue_tex_image s = nullptr;
    CHECK_EQ(nue_tex_image_load_memory(data, size, NUE_TEX_FILE_PNG, NUE_TEX_FILE_ASSUME_SRGB, &s), NUE_OK);
    nue_tex_image_desc g; NUE_STRUCT_INIT(&g);
    if (s) { nue_tex_image_get_desc(s, &g); CHECK_EQ(g.format, NUE_TEX_RGBA8_SRGB); }
    nue_tex_free(data); nue_tex_image_destroy(a); nue_tex_image_destroy(b); nue_tex_image_destroy(s);
}

TEST_CASE(file_io_and_errors) {
    nue_tex_image a = make_image(NUE_TEX_BC3_UNORM, 32, 32, 6);
    const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") + "/nue_tex_test_file.dds";
    CHECK_EQ(nue_tex_image_save_file(a, path.c_str(), NUE_TEX_FILE_AUTO, 0), NUE_OK);
    nue_tex_image b = nullptr;
    CHECK_EQ(nue_tex_image_load_file(path.c_str(), NUE_TEX_FILE_AUTO, 0, &b), NUE_OK);
    CHECK(b && same_content(a, b));
    CHECK_EQ(nue_tex_image_load_file("/definitely/not/here.dds", NUE_TEX_FILE_AUTO, 0, &b), NUE_ERR_NOT_FOUND);
    uint8_t junk[64] = {1, 2, 3};
    nue_tex_image c = nullptr;
    CHECK(nue_tex_image_load_memory(junk, sizeof junk, NUE_TEX_FILE_AUTO, 0, &c) < 0);
    CHECK(nue_tex_image_load_memory(nullptr, 0, NUE_TEX_FILE_AUTO, 0, &c) < 0);
    nue_tex_image_destroy(a); nue_tex_image_destroy(b);
    std::remove(path.c_str());
}

TEST_MAIN_BEGIN
    RUN(dds_round_trips);
    RUN(dds_legacy_header_layout);
    RUN(ktx2_round_trips);
    RUN(png_round_trip_and_reader);
    RUN(file_io_and_errors);
TEST_MAIN_END
