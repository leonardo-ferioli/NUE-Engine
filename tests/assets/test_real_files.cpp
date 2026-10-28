// Opt-in golden tests on the user's installed game files: set NUE_TEST_GAME_DIR to the game's data folder (the one holding GLOBAL, FRONTEND, ...).
// Without it the test prints "skipped" and passes. Files above 64 MiB (the STREAM*.BUN world files) are only used with NUE_TEST_GAME_BIG=1.
// Nothing read here is ever written into the repository.
#include "asset_common.hpp"
#include "test_env.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

using namespace asstest;
namespace fs = std::filesystem;

static std::vector<uint8_t> write_mem(nue_asset_container c, uint32_t flags, nue_result* res) {
    void* data = nullptr;
    size_t size = 0;
    nue_asset_write_desc w;
    NUE_STRUCT_INIT(&w);
    w.flags = flags;
    w.out_data = &data;
    w.out_size = &size;
    nue_result r = nue_asset_container_write(c, &w);
    if (res) *res = r;
    std::vector<uint8_t> out;
    if (r >= 0 && data) out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    if (data) nue_free(data);
    return out;
}

TEST_CASE(round_trip_every_game_file) {
    if (!game_dir()) { std::printf("  (skipped: set NUE_TEST_GAME_DIR to run on the game files)\n"); return; }
    nue_test::Engine eng;
    nue_assets_context ctx = nullptr;
    CHECK_EQ(nue_assets_context_create(nullptr, &ctx), NUE_OK);
    const bool big = std::getenv("NUE_TEST_GAME_BIG") != nullptr;
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(game_dir(), ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".bun" && ext != ".lzc") continue;
        if (it->file_size() > (64u << 20) && !big) continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    size_t total_files = 0, total_tex = 0, total_packs = 0, total_pack_bytes = 0, total_content_bytes = 0;
    for (const auto& p : files) {
        const std::string path = p.generic_string();
        std::vector<uint8_t> file;
        CHECK(load_file(path, file));
        nue_asset_open_desc d;
        NUE_STRUCT_INIT(&d);
        d.path = path.c_str();
        d.name_hint = path.c_str();
        nue_asset_container c = nullptr;
        nue_result r = nue_asset_container_open(ctx, &d, &c);
        if (r < 0) { std::printf("  FAIL open %s: %s\n", p.filename().string().c_str(), nue_get_last_error_message()); CHECK(r >= 0); continue; }
        nue_asset_container_info info;
        NUE_STRUCT_INIT(&info);
        nue_asset_container_get_info(c, &info);
        CHECK_EQ(info.file_size, uint64_t(file.size()));
        CHECK(!info.lenient_repairs);
        nue_result w = NUE_OK;
        // 1. untouched: the very bytes of the file
        auto out = write_mem(c, 0, &w);
        CHECK_EQ(w, NUE_OK);
        CHECK(out == file);
        // 2. rebuilt from the node tree: the plain content is identical (for .LZC the decoded .BUN)
        std::vector<uint8_t> content = file;
        if (info.codec) {
            content = write_mem(c, NUE_ASSET_WRITE_NO_CODEC, &w);
            CHECK_EQ(w, NUE_OK);
            CHECK_EQ(content.size(), size_t(info.content_size));
        }
        auto rebuilt = write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_NO_CODEC, &w);
        CHECK_EQ(w, NUE_OK);
        const bool same = rebuilt == content;
        CHECK(same);
        // 3. texture tables and pixel layout regenerated from the resources: identical again
        auto regen = write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_REGENERATE | NUE_ASSET_WRITE_NO_CODEC, &w);
        CHECK_EQ(w, NUE_OK);
        const bool same2 = regen == content;
        CHECK(same2);
        // 4. every texture reads
        size_t tex = 0, packs = 0, bad = 0;
        for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
            nue_asset_resource_info ri;
            NUE_STRUCT_INIT(&ri);
            nue_asset_resource_get(c, i, &ri);
            if (ri.kind == NUE_ASSET_RES_TEXTURE_PACK) ++packs;
            if (ri.kind != NUE_ASSET_RES_TEXTURE) continue;
            nue_tex_image im = nullptr;
            if (nue_asset_resource_get_image(c, i, &im) < 0) ++bad; else { ++tex; nue_tex_image_destroy(im); }
        }
        CHECK_EQ(bad, size_t(0));
        // 4b. into a .nuepack (source variant) and back: the same content again
        {
            nue_pack_builder b = nullptr;
            CHECK_EQ(nue_pack_builder_create(ctx, nullptr, &b), NUE_OK);
            nue_pack_convert_options po;
            NUE_STRUCT_INIT(&po);
            CHECK(nue_pack_builder_add_container(b, c, &po) >= 0);
            void* pdata = nullptr;
            size_t psize = 0;
            CHECK_EQ(nue_pack_builder_write(b, nullptr, &pdata, &psize), NUE_OK);
            nue_pack_builder_destroy(b);
            nue_pack pk = nullptr;
            CHECK_EQ(nue_pack_open_memory(ctx, pdata, psize, NUE_PACK_OPEN_VERIFY, &pk), NUE_OK);
            nue_asset_container x = nullptr;
            CHECK_EQ(nue_pack_export_container(pk, &x), NUE_OK);
            nue_result xw;
            auto exported = write_mem(x, NUE_ASSET_WRITE_NO_CODEC, &xw);
            CHECK_EQ(xw, NUE_OK);
            const bool pack_same = exported == content;
            CHECK(pack_same);
            if (!pack_same) std::printf("  PACK ROUND TRIP DIFFERS for %s\n", p.filename().string().c_str());
            total_pack_bytes += psize;
            total_content_bytes += content.size();
            nue_asset_container_close(x);
            nue_pack_close(pk);
            nue_free(pdata);
        }
        // 5. compressed files: our encoder's output decodes to the same content
        if (info.codec) {
            auto again = write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_RECOMPRESS, &w);
            CHECK_EQ(w, NUE_OK);
            std::vector<uint8_t> back(content.size());
            size_t n = 0;
            CHECK_EQ(nue_jdlz_decompress(again.data(), again.size(), back.data(), back.size(), &n), NUE_OK);
            CHECK(back == content);
            std::printf("  %-34s %10zu B  lzc %zu -> recompressed %zu (%.1f%% vs %.1f%%)  packs %zu textures %zu\n", p.filename().string().c_str(), content.size(), file.size(), again.size(),
                        100.0 * again.size() / content.size(), 100.0 * file.size() / content.size(), packs, tex);
        } else {
            std::printf("  %-34s %10zu B  nodes %u  packs %zu textures %zu%s\n", p.filename().string().c_str(), file.size(), info.node_count, packs, tex, (same && same2) ? "" : "  MISMATCH");
        }
        ++total_files; total_tex += tex; total_packs += packs;
        nue_asset_container_close(c);
    }
    std::printf("  %zu files, %zu texture packs, %zu textures: unchanged write, rebuild and regeneration and .nuepack round trip byte-identical\n", total_files, total_packs, total_tex);
    std::printf("  .nuepack (source variant only, blobs JDLZ compressed): %zu bytes for %zu bytes of content (%.1f%%)\n", total_pack_bytes, total_content_bytes,
                100.0 * double(total_pack_bytes) / double(total_content_bytes ? total_content_bytes : 1));
    nue_assets_context_destroy(ctx);
}

// Cars (GEOMETRY.BIN, TEXTURES.BIN, VINYLS.BIN with NUE_TEST_GAME_BIG) and the city (STREAM*.BUN): every mesh and texture reads, files round trip.
static void check_models_and_textures(nue_assets_context ctx, const std::string& path, size_t& meshes, size_t& tex, size_t& verts, size_t& tris) {
    std::vector<uint8_t> file;
    CHECK(load_file(path, file));
    nue_asset_open_desc d;
    NUE_STRUCT_INIT(&d);
    d.path = path.c_str();
    d.name_hint = path.c_str();
    nue_asset_container c = nullptr;
    nue_result r = nue_asset_container_open(ctx, &d, &c);
    if (r < 0) { std::printf("  FAIL open %s: %s\n", path.c_str(), nue_get_last_error_message()); CHECK(r >= 0); return; }
    nue_result w;
    CHECK(write_mem(c, 0, &w) == file);
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD, &w) == file);
    for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
        nue_asset_resource_info ri;
        NUE_STRUCT_INIT(&ri);
        nue_asset_resource_get(c, i, &ri);
        if (ri.kind == NUE_ASSET_RES_MESH) {
            nue_asset_mesh m;
            void* mem = nullptr;
            NUE_STRUCT_INIT(&m);
            r = nue_asset_resource_get_mesh(c, i, &m, &mem);
            if (r < 0) { std::printf("  FAIL mesh %s in %s: %s\n", ri.name, path.c_str(), nue_get_last_error_message()); CHECK(r >= 0); continue; }
            CHECK_EQ(m.vertex_count, ri.width);
            CHECK_EQ(m.index_count, ri.height * 3);
            for (uint32_t k = 0; k < m.index_count; ++k) if (m.indices[k] >= m.vertex_count) { CHECK(false); break; }
            for (uint32_t k = 0; k < m.vertex_count * 3; ++k) if (!(std::fabs(m.positions[k]) < 1e6f)) { std::printf("  FAIL wild vertex in %s of %s\n", ri.name, path.c_str()); CHECK(false); break; }
            ++meshes; verts += m.vertex_count; tris += m.index_count / 3;
            nue_free(mem);
        } else if (ri.kind == NUE_ASSET_RES_TEXTURE) {
            nue_tex_image im = nullptr;
            r = nue_asset_resource_get_image(c, i, &im);
            CHECK(r >= 0);
            if (r >= 0) { ++tex; nue_tex_image_destroy(im); }
        }
    }
    nue_asset_container_close(c);
}

TEST_CASE(cars_and_city_models) {
    if (!game_dir()) { std::printf("  (skipped: set NUE_TEST_GAME_DIR to run on the game files)\n"); return; }
    nue_test::Engine eng;
    nue_assets_context ctx = nullptr;
    CHECK_EQ(nue_assets_context_create(nullptr, &ctx), NUE_OK);
    const bool big = std::getenv("NUE_TEST_GAME_BIG") != nullptr;
    size_t cars = 0, meshes = 0, tex = 0, verts = 0, tris = 0;
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (auto& e : fs::directory_iterator(fs::path(game_dir()) / "CARS", ec)) if (e.is_directory()) dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end());
    for (const auto& dir : dirs) {
        bool any = false;
        for (const char* name : {"GEOMETRY.BIN", "TEXTURES.BIN", "VINYLS.BIN"}) {
            if (std::string(name) == "VINYLS.BIN" && !big) continue;
            const fs::path f = dir / name;
            if (!fs::exists(f, ec) || fs::file_size(f, ec) == 0) continue;
            check_models_and_textures(ctx, f.generic_string(), meshes, tex, verts, tris);
            any = true;
        }
        if (any) ++cars;
    }
    std::printf("  %zu car folders: %zu meshes (%zu vertices, %zu triangles), %zu textures decoded\n", cars, meshes, verts, tris, tex);
    CHECK(cars > 30);
    CHECK(meshes > 15000);
    size_t cm = 0, ct = 0, cv = 0, cr = 0;
    for (const char* n : {"STREAML4RB.BUN", "STREAML4RC.BUN", "STREAML4RF.BUN", "STREAML4RG.BUN", "STREAML4RH.BUN", "STREAML4RR.BUN", "L4RA.BUN"})
        check_models_and_textures(ctx, game_path((std::string("TRACKS/") + n).c_str()), cm, ct, cv, cr);
    if (big) for (const char* n : {"STREAML4RA.BUN", "STREAML4RD.BUN"}) check_models_and_textures(ctx, game_path((std::string("TRACKS/") + n).c_str()), cm, ct, cv, cr);
    std::printf("  city files: %zu meshes (%zu vertices, %zu triangles), %zu textures\n", cm, cv, cr, ct);
    CHECK(cm > 5000);
    // a car with its textures and the global ones as one glTF
    const std::string car = game_path("CARS/240SX/GEOMETRY.BIN"), tx = game_path("CARS/240SX/TEXTURES.BIN"), gl = game_path("GLOBAL/GlobalB.lzc");
    nue_asset_container cs[3] = {nullptr, nullptr, nullptr};
    const std::string paths[3] = {car, tx, gl};
    for (int i = 0; i < 3; ++i) {
        nue_asset_open_desc od;
        NUE_STRUCT_INIT(&od);
        od.path = paths[i].c_str();
        CHECK_EQ(nue_asset_container_open(ctx, &od, &cs[i]), NUE_OK);
    }
    const std::string out = nue_test::unique_temp_dir("assets_car") + "/car.glb";
    nue_asset_model_desc md;
    NUE_STRUCT_INIT(&md);
    md.containers = cs;
    md.container_count = 3;
    md.format = NUE_ASSET_MODEL_GLB;
    md.flags = NUE_ASSET_MODEL_BEST_LOD;
    md.name_filter = "240SX";
    md.path = out.c_str();
    nue_asset_model_stats st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_asset_export_model(&md, &st), NUE_OK);
    std::printf("  240SX.glb: %u meshes, %llu triangles, %u textures, %llu bytes\n", st.meshes, (unsigned long long)st.triangles, st.textures, (unsigned long long)st.bytes_written);
    CHECK(st.meshes > 250);
    CHECK_EQ(st.meshes_skipped, 0u);
    CHECK(st.textures >= 10);
    for (auto c : cs) nue_asset_container_close(c);
    std::filesystem::remove_all(fs::path(out).parent_path(), ec);
    nue_assets_context_destroy(ctx);
}

TEST_MAIN_BEGIN
    RUN(round_trip_every_game_file);
    RUN(cars_and_city_models);
TEST_MAIN_END
