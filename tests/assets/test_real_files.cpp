// Opt-in golden tests on the user's installed game files: set NUE_TEST_GAME_DIR to the game's data folder (the one holding GLOBAL, FRONTEND, ...).
// Without it the test prints "skipped" and passes. Files above 64 MiB (the STREAM*.BUN world files) are only used with NUE_TEST_GAME_BIG=1.
// Nothing read here is ever written into the repository.
#include "asset_common.hpp"
#include "test_env.h"

#include <algorithm>
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
    size_t total_files = 0, total_tex = 0, total_packs = 0;
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
    std::printf("  %zu files, %zu texture packs, %zu textures: unchanged write, rebuild and regeneration byte-identical\n", total_files, total_packs, total_tex);
    nue_assets_context_destroy(ctx);
}

TEST_MAIN_BEGIN
    RUN(round_trip_every_game_file);
TEST_MAIN_END
