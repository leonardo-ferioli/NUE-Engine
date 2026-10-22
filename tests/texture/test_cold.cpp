// Cold conversion: every format cooked ahead of time into the cache, run time = lookup by content, no encoding while "playing".
#include "tex_common.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>

using namespace texttest;
namespace fs = std::filesystem;

static nue_tex_image encode_image(nue_tex_format f, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed, bool alpha) {
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d); d.format = NUE_TEX_RGBA8_UNORM; d.width = w; d.height = h; d.mip_levels = 1;
    nue_tex_image src = nullptr, out = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &src), NUE_OK);
    const auto px = synth_rgba(w, h, seed, alpha);
    nue_tex_level_view v; NUE_STRUCT_INIT(&v); nue_tex_image_get_level(src, 0, 0, 0, &v);
    std::memcpy(v.data, px.data(), px.size());
    nue_tex_encode_params ep; nue_tex_encode_params_default(&ep); ep.quality = NUE_TEX_QUALITY_FAST;
    CHECK_EQ(nue_tex_image_convert(src, f, mips, &ep, nullptr, &out), NUE_OK);
    nue_tex_image_destroy(src);
    return out;
}

static std::string temp_dir(const char* name) {
    fs::path p = fs::temp_directory_path() / (std::string("nue_tex_") + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.string();
}

static std::map<nue_tex_format, int> cache_formats(const std::string& dir) {
    std::map<nue_tex_format, int> m;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".ktx2") continue;
        nue_tex_image im = nullptr;
        if (nue_tex_image_load_file(e.path().string().c_str(), NUE_TEX_FILE_AUTO, 0, &im) < 0) continue;
        nue_tex_image_desc d; NUE_STRUCT_INIT(&d); nue_tex_image_get_desc(im, &d);
        ++m[d.format];
        nue_tex_image_destroy(im);
    }
    return m;
}

TEST_CASE(cook_all_formats_then_serve_without_encoding) {
    const std::string src = temp_dir("cold_src"), cache = temp_dir("cold_cache");
    // three "game textures": DXT5 with smooth alpha, opaque DXT1, a DXT5/BC5 normal map
    nue_tex_image tex[3];
    tex[0] = encode_image(NUE_TEX_BC3_UNORM, 64, 64, 4, 21, true);
    tex[1] = encode_image(NUE_TEX_BC1_UNORM, 64, 64, 3, 22, false);
    tex[2] = encode_image(NUE_TEX_BC5_UNORM, 32, 32, 2, 23, false);
    const char* names[3] = {"t0_dxt5.dds", "t1_dxt1.dds", "t2_normals.dds"};
    for (int i = 0; i < 3; ++i) CHECK_EQ(nue_tex_image_save_file(tex[i], (src + "/" + names[i]).c_str(), NUE_TEX_FILE_AUTO, 0), NUE_OK);

    // ---- cold phase: ONE import converts every texture for EVERY device class
    nue_tex_caps caps; nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.cache_dir = cache.c_str(); cd.worker_threads = 2;      // no ALLOW_RUNTIME_ENCODE
    nue_tex_context cook = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &cook), NUE_OK);
    nue_tex_importer imp = nullptr;
    CHECK_EQ(nue_tex_importer_create(cook, nullptr, nullptr, &imp), NUE_OK);
    CHECK_EQ(nue_tex_importer_set_classes(imp, NUE_TEX_CLASS_MASK_ALL), NUE_OK);
    for (int i = 0; i < 3; ++i) {
        nue_tex_import_item it; NUE_STRUCT_INIT(&it);
        const std::string p = src + "/" + names[i];
        it.name = names[i]; it.path = p.c_str(); it.usage = i == 2 ? NUE_TEX_USAGE_NORMAL_MAP : NUE_TEX_USAGE_COLOR;
        CHECK_EQ(nue_tex_importer_add(imp, &it), NUE_OK);
    }
    CHECK_EQ(nue_tex_importer_start(imp), NUE_OK);
    CHECK_EQ(nue_tex_importer_wait(imp, 120000), NUE_OK);
    nue_tex_import_progress pr; NUE_STRUCT_INIT(&pr); nue_tex_importer_get_progress(imp, &pr);
    CHECK_EQ(pr.done, 3u); CHECK_EQ(pr.failed, 0u); CHECK_EQ(pr.skipped_cached, 0u); CHECK(pr.produced_bytes > 0);
    nue_tex_importer_destroy(imp);

    const auto formats = cache_formats(cache);
    std::printf("  cold cache holds:");
    int total = 0;
    for (auto& e : formats) { std::printf(" %s x%d", nue_tex_format_name(e.first), e.second); total += e.second; }
    std::printf("\n");
    CHECK(formats.count(NUE_TEX_ASTC_4x4_UNORM));            // Android/Apple ASTC devices
    CHECK(formats.count(NUE_TEX_ETC2_RGBA8));                // Android ETC2-only devices
    CHECK(formats.count(NUE_TEX_ETC2_RGB8));                 // the opaque DXT1 on ETC2 devices
    CHECK(formats.count(NUE_TEX_EAC_RG11_UNORM));            // the normal map
    CHECK(formats.count(NUE_TEX_PVRTC1_4BPP));               // old Apple devices (64x64 power-of-two square)
    CHECK(formats.count(NUE_TEX_ASTC_UNORM(3)));             // DXT1 whose alpha the layer does not know: an alpha-capable target (ASTC 6x5) is cooked too
    CHECK(total >= 8);

    // resumable and idempotent: a second cook finds everything it needs already there
    nue_tex_cook_result cr; NUE_STRUCT_INIT(&cr);
    nue_tex_convert_request rq; NUE_STRUCT_INIT(&rq); rq.source = tex[0];
    CHECK_EQ(nue_tex_cook_image(cook, &rq, NUE_TEX_CLASS_MASK_ALL, &cr), NUE_OK);
    CHECK(cr.variants >= 3); CHECK_EQ(cr.stored, 0u); CHECK_EQ(cr.already_cached, cr.variants); CHECK_EQ(cr.failed, 0u);
    // the contents did not change but the cook also works for a subset of devices
    CHECK_EQ(nue_tex_cook_image(cook, &rq, NUE_TEX_CLASS_BIT(NUE_TEX_DEVICE_ANDROID_ETC2), &cr), NUE_OK);
    CHECK_EQ(cr.variants, 1u);
    nue_tex_context_destroy(cook);

    // ---- run time: a NEW context on the same folder (a later start of the game), encoding disabled
    nue_tex_context play = nullptr;
    cd.worker_threads = 1;
    CHECK_EQ(nue_tex_context_create(&cd, &play), NUE_OK);
    nue_tex_runtime_stats rs; NUE_STRUCT_INIT(&rs); nue_tex_context_get_runtime_stats(play, &rs);
    CHECK(rs.indexed_levels >= 10);                              // the index was rebuilt from the cache folder on creation
    const uint32_t DXT5 = 'D' | ('X' << 8) | ('T' << 16) | ('5' << 24), DXT1 = 'D' | ('X' << 8) | ('T' << 16) | ('1' << 24);
    nue_tex_d3d9_texture_desc td; NUE_STRUCT_INIT(&td);
    td.d3d_format = DXT5; td.width = 64; td.height = 64; td.levels = 4; td.pool = 1; td.alpha_hint = NUE_TEX_ALPHA_INFO_SMOOTH;
    nue_tex_d3d9_texture t = nullptr; nue_tex_d3d9_texture_info info; NUE_STRUCT_INIT(&info); nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
    CHECK_EQ(nue_tex_d3d9_texture_create(play, &td, &t, &info, &plan), NUE_OK);
    CHECK_EQ(info.gpu_format, NUE_TEX_ASTC_4x4_UNORM); CHECK_EQ(info.gpu_levels, 4u);
    nue_tex_image gpu_ref = nullptr;                              // the cooked file, loaded directly, to compare bytes
    {
        nue_tex_convert_request r2; NUE_STRUCT_INIT(&r2); r2.source = tex[0];
        // a development context produces the same bytes as the cooked file would
        nue_tex_context_desc dd = cd; dd.flags = NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE;
        nue_tex_context dev = nullptr;
        CHECK_EQ(nue_tex_context_create(&dd, &dev), NUE_OK);
        nue_bool hit = 0;
        CHECK_EQ(nue_tex_context_convert(dev, &r2, &gpu_ref, nullptr, &hit), NUE_OK);
        CHECK(hit);                                               // served from the cooked cache, not re-encoded
        nue_tex_context_destroy(dev);
    }
    for (uint32_t l = 0; l < 4; ++l) {
        nue_tex_level_view sv; NUE_STRUCT_INIT(&sv); nue_tex_image_get_level(tex[0], l, 0, 0, &sv);
        nue_tex_level_view gv; NUE_STRUCT_INIT(&gv); nue_tex_image_get_level(gpu_ref, l, 0, 0, &gv);
        std::vector<uint8_t> out(nue_tex_d3d9_level_gpu_size(t, l, nullptr));
        uint32_t pitch = 0; size_t written = 0;
        CHECK_EQ(nue_tex_d3d9_convert_level(t, l, 0, nullptr, sv.data, sv.row_pitch, out.data(), out.size(), &pitch, &written), NUE_OK);
        CHECK(written == gv.size && std::memcmp(out.data(), gv.data, size_t(gv.size)) == 0);
    }
    nue_tex_d3d9_texture_destroy(t);
    // opaque DXT1 whose alpha the layer knows -> ETC2 RGB; the same texture with unknown alpha -> ASTC 6x5; both cooked
    td.d3d_format = DXT1; td.levels = 3; td.alpha_hint = NUE_TEX_ALPHA_INFO_OPAQUE;
    for (int pass = 0; pass < 2; ++pass) {
        td.alpha_hint = pass ? NUE_TEX_ALPHA_INFO_UNKNOWN : NUE_TEX_ALPHA_INFO_OPAQUE;
        CHECK_EQ(nue_tex_d3d9_texture_create(play, &td, &t, &info, &plan), NUE_OK);
        CHECK_EQ(info.gpu_format, pass ? NUE_TEX_ASTC_UNORM(3) : NUE_TEX_ETC2_RGB8);
        for (uint32_t l = 0; l < 3; ++l) {
            nue_tex_level_view sv; NUE_STRUCT_INIT(&sv); nue_tex_image_get_level(tex[1], l, 0, 0, &sv);
            std::vector<uint8_t> out(nue_tex_d3d9_level_gpu_size(t, l, nullptr));
            uint32_t pitch; size_t written;
            CHECK_EQ(nue_tex_d3d9_convert_level(t, l, 0, nullptr, sv.data, sv.row_pitch, out.data(), out.size(), &pitch, &written), NUE_OK);
        }
        nue_tex_d3d9_texture_destroy(t);
    }
    // a texture the game builds by itself (not in the archives that were cooked): placeholder, never an encode
    td.alpha_hint = NUE_TEX_ALPHA_INFO_OPAQUE;
    CHECK_EQ(nue_tex_d3d9_texture_create(play, &td, &t, &info, &plan), NUE_OK);
    nue_tex_image stranger = encode_image(NUE_TEX_BC1_UNORM, 64, 64, 1, 99, false);
    nue_tex_level_view sv; NUE_STRUCT_INIT(&sv); nue_tex_image_get_level(stranger, 0, 0, 0, &sv);
    std::vector<uint8_t> out(nue_tex_d3d9_level_gpu_size(t, 0, nullptr), 0xAB);
    uint32_t pitch; size_t written;
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 0, 0, nullptr, sv.data, sv.row_pitch, out.data(), out.size(), &pitch, &written), NUE_OK_PARTIAL);
    CHECK_EQ(written, out.size());
    bool zero = true; for (uint8_t b : out) zero &= b == 0;
    CHECK(zero);
    // a partial rectangle of a level is not cached content either
    nue_rect_i r{0, 0, 32, 32};
    std::vector<uint8_t> part(nue_tex_d3d9_level_gpu_size(t, 0, &r));
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 0, 0, &r, sv.data, sv.row_pitch, part.data(), part.size(), &pitch, &written), NUE_OK_PARTIAL);
    nue_tex_d3d9_texture_destroy(t);
    nue_tex_context_get_runtime_stats(play, &rs);
    CHECK_EQ(rs.runtime_encodes, 0u);                             // nothing was encoded while "playing"
    CHECK_EQ(rs.level_hits, 4u + 3u + 3u); CHECK_EQ(rs.level_misses, 2u);
    // the generic conversion entry also refuses to encode (and says why)
    nue_tex_convert_request r3; NUE_STRUCT_INIT(&r3); r3.source = stranger;
    nue_tex_image none = nullptr;
    CHECK_EQ(nue_tex_context_convert(play, &r3, &none, nullptr, nullptr), NUE_ERR_NOT_FOUND);
    nue_tex_image_destroy(stranger); nue_tex_image_destroy(gpu_ref);
    nue_tex_context_destroy(play);
    for (auto i : tex) nue_tex_image_destroy(i);
}

TEST_CASE(a_cache_can_be_copied_to_another_machine) {
    // keys are content hashes, not device ids and not paths: moving the folder is enough
    const std::string a = temp_dir("cold_a"), b = temp_dir("cold_b");
    nue_tex_image tex = encode_image(NUE_TEX_BC3_UNORM, 32, 32, 3, 31, true);
    nue_tex_caps caps; nue_tex_caps_for_class(NUE_TEX_DEVICE_DESKTOP_BC, &caps);       // cooked on a PC...
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.cache_dir = a.c_str(); cd.worker_threads = 2;
    nue_tex_context c1 = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &c1), NUE_OK);
    nue_tex_convert_request rq; NUE_STRUCT_INIT(&rq); rq.source = tex;
    nue_tex_cook_result cr; NUE_STRUCT_INIT(&cr);
    CHECK_EQ(nue_tex_cook_image(c1, &rq, NUE_TEX_CLASS_MASK_ALL, &cr), NUE_OK);
    CHECK(cr.stored >= 2);
    nue_tex_context_destroy(c1);
    fs::copy(a, b, fs::copy_options::recursive);
    nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);                          // ...used on a phone
    cd.cache_dir = b.c_str();
    nue_tex_context c2 = nullptr;
    CHECK_EQ(nue_tex_context_create(&cd, &c2), NUE_OK);
    nue_tex_d3d9_texture_desc td; NUE_STRUCT_INIT(&td);
    td.d3d_format = 'D' | ('X' << 8) | ('T' << 16) | ('5' << 24); td.width = 32; td.height = 32; td.levels = 3; td.pool = 1; td.alpha_hint = NUE_TEX_ALPHA_INFO_SMOOTH;
    nue_tex_d3d9_texture t = nullptr; nue_tex_d3d9_texture_info info; NUE_STRUCT_INIT(&info);
    CHECK_EQ(nue_tex_d3d9_texture_create(c2, &td, &t, &info, nullptr), NUE_OK);
    nue_tex_level_view sv; NUE_STRUCT_INIT(&sv); nue_tex_image_get_level(tex, 0, 0, 0, &sv);
    std::vector<uint8_t> out(nue_tex_d3d9_level_gpu_size(t, 0, nullptr));
    uint32_t pitch; size_t written;
    CHECK_EQ(nue_tex_d3d9_convert_level(t, 0, 0, nullptr, sv.data, sv.row_pitch, out.data(), out.size(), &pitch, &written), NUE_OK);
    nue_tex_d3d9_texture_destroy(t);
    nue_tex_context_destroy(c2);
    nue_tex_image_destroy(tex);
}

TEST_MAIN_BEGIN
    RUN(cook_all_formats_then_serve_without_encoding);
    RUN(a_cache_can_be_copied_to_another_machine);
TEST_MAIN_END
