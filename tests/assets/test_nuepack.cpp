// .nuepack: building from containers, variants, deduplication, reading, verification, damaged files, export back to the game format, mounting.
#include "nfs_fixture.hpp"
#include "test_env.h"

#include <filesystem>

using namespace asstest;

static nue_test::Engine& engine() { static nue_test::Engine e; return e; }

struct Fx {
    nue_assets_context ctx = nullptr;
    Fx() { engine(); CHECK_EQ(nue_assets_context_create(nullptr, &ctx), NUE_OK); }
    ~Fx() { nue_assets_context_destroy(ctx); }
    nue_asset_container open(const std::vector<uint8_t>& bytes, const char* hint = ".bun") {
        nue_asset_open_desc d;
        NUE_STRUCT_INIT(&d);
        d.flags = NUE_ASSET_OPEN_COPY;
        d.memory = bytes.data();
        d.memory_size = bytes.size();
        d.name_hint = hint;
        nue_asset_container c = nullptr;
        CHECK_EQ(nue_asset_container_open(ctx, &d, &c), NUE_OK);
        return c;
    }
};

static std::vector<uint8_t> build_pack(Fx& fx, nue_asset_container c, uint32_t variants, nue_result* res = nullptr, const char* source = "synthetic.bun") {
    nue_pack_builder b = nullptr;
    nue_pack_builder_desc bd;
    NUE_STRUCT_INIT(&bd);
    bd.source_name = source;
    CHECK_EQ(nue_pack_builder_create(fx.ctx, &bd, &b), NUE_OK);
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    o.variants = variants;
    o.quality = NUE_TEX_QUALITY_FAST;
    nue_result r = nue_pack_builder_add_container(b, c, &o);
    if (res) *res = r;
    CHECK(r >= 0);
    void* data = nullptr;
    size_t size = 0;
    CHECK_EQ(nue_pack_builder_write(b, nullptr, &data, &size), NUE_OK);
    std::vector<uint8_t> out(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    nue_free(data);
    nue_pack_builder_destroy(b);
    return out;
}

static std::vector<uint8_t> write_mem(nue_asset_container c, uint32_t flags, nue_result* res = nullptr) {
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

static uint32_t all_variants() { return (1u << NUE_PACK_VARIANT_BC) | (1u << NUE_PACK_VARIANT_ASTC) | (1u << NUE_PACK_VARIANT_ETC2) | (1u << NUE_PACK_VARIANT_PVRTC) | (1u << NUE_PACK_VARIANT_RGBA8); }

TEST_CASE(build_read_and_verify) {
    Fx fx;
    auto bun = make_bun(default_textures(), true);
    nue_asset_container c = fx.open(bun);
    nue_result r;
    auto pack = build_pack(fx, c, all_variants(), &r);
    CHECK_EQ(r, NUE_OK);
    nue_pack p = nullptr;
    CHECK_EQ(nue_pack_open_memory(fx.ctx, pack.data(), pack.size(), NUE_PACK_OPEN_VERIFY, &p), NUE_OK);
    nue_pack_info info;
    NUE_STRUCT_INIT(&info);
    CHECK_EQ(nue_pack_get_info(p, &info), NUE_OK);
    CHECK_EQ(info.version_major, 1u);
    CHECK_EQ(info.total_size, uint64_t(pack.size()));
    CHECK(info.has_tree);
    CHECK_STR(info.format_name, "nfs-bun");
    CHECK_STR(info.codec_name, "");
    CHECK_STR(info.source_name, "synthetic.bun");
    CHECK_EQ(info.tree_node_count, nue_asset_node_count(c));
    CHECK_EQ(info.variant_mask, (all_variants() | 1u));
    // 7 textures x (source + 5 variants) + tree blobs
    uint32_t tex_entries = 0, tree_entries = 0;
    for (uint32_t i = 0; i < nue_pack_entry_count(p); ++i) {
        nue_pack_entry_info e;
        NUE_STRUCT_INIT(&e);
        nue_pack_entry_get(p, i, &e);
        if (e.kind == NUE_ASSET_RES_TEXTURE) ++tex_entries;
        if (e.flags & NUE_PACK_ENTRY_TREE_BLOB) ++tree_entries;
    }
    CHECK_EQ(tex_entries, 7u * 6u);
    CHECK(tree_entries > 5);
    CHECK_EQ(nue_pack_verify(p, nullptr), NUE_OK);
    // variants of one texture: right family, same size, decodable back to images
    const nue_pack_variant order[] = {NUE_PACK_VARIANT_SOURCE, NUE_PACK_VARIANT_BC, NUE_PACK_VARIANT_ASTC, NUE_PACK_VARIANT_ETC2, NUE_PACK_VARIANT_PVRTC, NUE_PACK_VARIANT_RGBA8};
    for (nue_pack_variant v : order) {
        uint32_t e = nue_pack_entry_find(p, "SYNTHETIC/TEX_DXT1_64", v);
        CHECK(e != NUE_ASSET_NO_NODE);
        nue_pack_entry_info ei;
        NUE_STRUCT_INIT(&ei);
        nue_pack_entry_get(p, e, &ei);
        CHECK_EQ(ei.variant, uint32_t(v));
        CHECK_EQ(ei.width, 64u);
        CHECK_EQ(ei.height, 64u);
        CHECK(ei.mip_levels >= 1);
        nue_tex_image im = nullptr;
        CHECK_EQ(nue_pack_entry_image(p, e, &im), NUE_OK);
        nue_tex_image_desc d;
        NUE_STRUCT_INIT(&d);
        nue_tex_image_get_desc(im, &d);
        CHECK_EQ(d.format, nue_tex_format(ei.tex_format));
        CHECK(nue_tex_image_total_size(im) > 0);
        if (v == NUE_PACK_VARIANT_BC) CHECK_EQ(d.format, NUE_TEX_BC1_UNORM);             // desktop keeps the DXT it already had
        if (v == NUE_PACK_VARIANT_RGBA8) CHECK_EQ(d.format, NUE_TEX_RGBA8_UNORM);
        if (v == NUE_PACK_VARIANT_ASTC) CHECK(d.format >= NUE_TEX_ASTC_4x4_UNORM && d.format <= NUE_TEX_ASTC_12x12_HDR);
        nue_tex_image_destroy(im);
    }
    // the BC variant is the source data: stored once
    nue_pack_entry_info a, b;
    NUE_STRUCT_INIT(&a); NUE_STRUCT_INIT(&b);
    nue_pack_entry_get(p, nue_pack_entry_find(p, "SYNTHETIC/TEX_DXT1_64", NUE_PACK_VARIANT_SOURCE), &a);
    nue_pack_entry_get(p, nue_pack_entry_find(p, "SYNTHETIC/TEX_DXT1_64", NUE_PACK_VARIANT_BC), &b);
    CHECK_EQ(a.offset, b.offset);
    CHECK(std::memcmp(a.sha256, b.sha256, 32) == 0);
    // palettised source: the SOURCE variant keeps indices and palette, the variants hold expanded pixels
    nue_pack_entry_info pe;
    NUE_STRUCT_INIT(&pe);
    nue_pack_entry_get(p, nue_pack_entry_find(p, "SYNTHETIC/TEX_P8_16", NUE_PACK_VARIANT_SOURCE), &pe);
    CHECK((pe.flags & NUE_PACK_ENTRY_PALETTED) != 0);
    CHECK_EQ(pe.raw_size, uint64_t(16 * 16 + 8 * 8 + 1024));
    // fallback when the preferred variant is absent: a pack made with the source only returns it for every request
    uint32_t fb = nue_pack_entry_find(p, "ONLY_ONE", NUE_PACK_VARIANT_ETC2);
    CHECK(fb == NUE_ASSET_NO_NODE);                                                      // the name is "SECOND/ONLY_ONE"
    CHECK(nue_pack_entry_find(p, "SECOND/ONLY_ONE", NUE_PACK_VARIANT_ETC2) != NUE_ASSET_NO_NODE);
    CHECK(nue_pack_entry_find(p, "second/only_one", NUE_PACK_VARIANT_BC) != NUE_ASSET_NO_NODE);   // case-insensitive
    CHECK(nue_pack_entry_find(p, "nope", NUE_PACK_VARIANT_BC) == NUE_ASSET_NO_NODE);
    nue_pack_close(p);

    // only the source variant
    auto src_only = build_pack(fx, c, 0);
    CHECK(src_only.size() < pack.size());
    CHECK(nue_pack_open_memory(fx.ctx, src_only.data(), src_only.size(), 0, &p) == NUE_OK);
    uint32_t e = nue_pack_entry_find(p, "SYNTHETIC/TEX_BGRA_8", NUE_PACK_VARIANT_ASTC);
    nue_pack_entry_info ei;
    NUE_STRUCT_INIT(&ei);
    nue_pack_entry_get(p, e, &ei);
    CHECK_EQ(ei.variant, uint32_t(NUE_PACK_VARIANT_SOURCE));
    nue_pack_close(p);
    nue_asset_container_close(c);
}

TEST_CASE(output_is_deterministic) {
    Fx fx;
    auto bun = make_bun(default_textures(), true);
    nue_asset_container c = fx.open(bun);
    auto a = build_pack(fx, c, all_variants());
    auto b = build_pack(fx, c, all_variants());
    CHECK(a == b);
    nue_asset_container_close(c);
    nue_asset_container c2 = fx.open(bun);
    CHECK(build_pack(fx, c2, all_variants()) == a);
    nue_asset_container_close(c2);
}

TEST_CASE(export_gives_the_original_file_back) {
    Fx fx;
    for (bool second : {false, true}) {
        auto bun = make_bun(default_textures(), second);
        nue_asset_container c = fx.open(bun);
        auto pack = build_pack(fx, c, (1u << NUE_PACK_VARIANT_BC) | (1u << NUE_PACK_VARIANT_ASTC));
        nue_pack p = nullptr;
        CHECK_EQ(nue_pack_open_memory(fx.ctx, pack.data(), pack.size(), 0, &p), NUE_OK);
        nue_asset_container x = nullptr;
        CHECK_EQ(nue_pack_export_container(p, &x), NUE_OK);
        CHECK_EQ(nue_asset_resource_count(x), nue_asset_resource_count(c));
        nue_result r;
        auto out = write_mem(x, 0, &r);
        CHECK_EQ(r, NUE_OK);
        CHECK(out == bun);                                                               // palettes, padding, odd sizes: byte for byte
        nue_asset_container_close(x);
        nue_pack_close(p);
        nue_asset_container_close(c);
    }
}

TEST_CASE(export_keeps_the_codec_and_edited_textures) {
    Fx fx;
    auto bun = make_bun(default_textures());
    std::vector<uint8_t> lzc(nue_jdlz_compress_bound(bun.size()));
    size_t n = 0;
    nue_jdlz_compress(bun.data(), bun.size(), 6, lzc.data(), lzc.size(), &n);
    lzc.resize(n);
    nue_asset_container c = fx.open(lzc, ".lzc");
    // edit one texture before packing
    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    d.format = NUE_TEX_BGRA8_UNORM; d.width = 16; d.height = 16; d.mip_levels = 3;
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &im), NUE_OK);
    nue_tex_image_desc g;
    NUE_STRUCT_INIT(&g);
    nue_tex_image_get_desc(im, &g);
    uint32_t seed = 5;
    for (uint32_t l = 0; l < g.mip_levels; ++l) { nue_tex_level_view v; NUE_STRUCT_INIT(&v); nue_tex_image_get_level(im, l, 0, 0, &v); for (uint64_t i = 0; i < v.size; ++i) static_cast<uint8_t*>(v.data)[i] = uint8_t(rng_next(seed)); }
    CHECK_EQ(nue_asset_resource_set_image(c, nue_asset_resource_find(c, "TEX_DXT1_16", 0), im), NUE_OK);
    auto expect = write_mem(c, 0);                                                       // what the edited container writes (compressed)
    auto pack = build_pack(fx, c, 1u << NUE_PACK_VARIANT_BC);
    nue_pack p = nullptr;
    CHECK_EQ(nue_pack_open_memory(fx.ctx, pack.data(), pack.size(), 0, &p), NUE_OK);
    nue_pack_info info;
    NUE_STRUCT_INIT(&info);
    nue_pack_get_info(p, &info);
    CHECK_STR(info.codec_name, "jdlz");
    nue_asset_container x = nullptr;
    CHECK_EQ(nue_pack_export_container(p, &x), NUE_OK);
    nue_result r;
    auto out = write_mem(x, 0, &r);
    CHECK_EQ(r, NUE_OK);
    CHECK(out == expect);                                                                // same edited file, compressed again
    nue_tex_image_destroy(im);
    nue_asset_container_close(x);
    nue_pack_close(p);
    nue_asset_container_close(c);
}

TEST_CASE(damaged_packs) {
    Fx fx;
    auto bun = make_bun(default_textures());
    nue_asset_container c = fx.open(bun);
    auto pack = build_pack(fx, c, 1u << NUE_PACK_VARIANT_ASTC);
    nue_pack p = nullptr;
    // bad magic / version / truncation
    auto bad = pack; bad[0] = 'X';
    CHECK(nue_pack_open_memory(fx.ctx, bad.data(), bad.size(), 0, &p) < 0);
    bad = pack; bad[4] = 9;
    CHECK(nue_pack_open_memory(fx.ctx, bad.data(), bad.size(), 0, &p) < 0);
    bad.assign(pack.begin(), pack.begin() + pack.size() - 100);
    CHECK(nue_pack_open_memory(fx.ctx, bad.data(), bad.size(), 0, &p) < 0);
    CHECK(nue_pack_open_memory(fx.ctx, pack.data(), 50, 0, &p) < 0);
    CHECK(nue_pack_open_memory(fx.ctx, nullptr, 0, 0, &p) < 0);
    // a flipped bit anywhere in the header or tables is caught on open; in the data, by verify
    uint32_t seed = 31337;
    int caught_open = 0, caught_verify = 0, silent = 0;
    for (int i = 0; i < 400; ++i) {
        auto m = pack;
        const size_t at = rng_next(seed) % m.size();
        m[at] ^= uint8_t(1u << (rng_next(seed) & 7));
        nue_pack q = nullptr;
        nue_result r = nue_pack_open_memory(fx.ctx, m.data(), m.size(), 0, &q);
        if (r < 0) { ++caught_open; continue; }
        uint32_t badn = 0;
        if (nue_pack_verify(q, &badn) < 0) ++caught_verify; else ++silent;
        // reading never crashes whatever was damaged
        for (uint32_t e = 0; e < nue_pack_entry_count(q); ++e) {
            nue_tex_image im = nullptr;
            if (nue_pack_entry_image(q, e, &im) >= 0) nue_tex_image_destroy(im);
        }
        nue_asset_container x = nullptr;
        if (nue_pack_export_container(q, &x) >= 0) { write_mem(x, 0); nue_asset_container_close(x); }
        nue_pack_close(q);
    }
    CHECK(caught_open + caught_verify >= 380);                                           // padding bytes between blocks are the only free bits
    std::printf("  400 single bit flips: %d caught at open, %d by verify, %d in unused padding\n", caught_open, caught_verify, silent);
    nue_asset_container_close(c);
}

TEST_CASE(loose_files) {
    Fx fx;
    nue_pack_builder b = nullptr;
    CHECK_EQ(nue_pack_builder_create(fx.ctx, nullptr, &b), NUE_OK);
    auto text = std::vector<uint8_t>(5000, 'a');
    for (size_t i = 0; i < text.size(); ++i) text[i] = uint8_t('a' + (i * 7) % 11);
    CHECK_EQ(nue_pack_builder_add_blob(b, "data/readme.txt", text.data(), text.size(), 0), NUE_OK);
    CHECK_EQ(nue_pack_builder_add_blob(b, "data/empty.bin", nullptr, 0, 0), NUE_OK);
    auto noise_data = noise(3000, 9);
    CHECK_EQ(nue_pack_builder_add_blob(b, "data/noise.bin", noise_data.data(), noise_data.size(), 0), NUE_OK);
    CHECK_EQ(nue_pack_builder_add_blob(b, "data/same.bin", noise_data.data(), noise_data.size(), 0), NUE_OK);     // same bytes: stored once
    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    d.format = NUE_TEX_RGBA8_UNORM; d.width = 32; d.height = 32; d.mip_levels = 6;
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &im), NUE_OK);
    nue_tex_level_view v;
    NUE_STRUCT_INIT(&v);
    nue_tex_image_get_level(im, 0, 0, 0, &v);
    for (uint64_t i = 0; i < v.size; ++i) static_cast<uint8_t*>(v.data)[i] = uint8_t(i * 5);
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    o.variants = (1u << NUE_PACK_VARIANT_BC) | (1u << NUE_PACK_VARIANT_ASTC);
    o.quality = NUE_TEX_QUALITY_FAST;
    CHECK_EQ(nue_pack_builder_add_image(b, "ui/logo", im, &o), NUE_OK);
    void* data = nullptr;
    size_t size = 0;
    CHECK_EQ(nue_pack_builder_write(b, nullptr, &data, &size), NUE_OK);
    nue_pack_builder_destroy(b);
    nue_pack p = nullptr;
    CHECK_EQ(nue_pack_open_memory(fx.ctx, data, size, NUE_PACK_OPEN_VERIFY, &p), NUE_OK);
    nue_pack_info info;
    NUE_STRUCT_INIT(&info);
    nue_pack_get_info(p, &info);
    CHECK(!info.has_tree);
    uint32_t e = nue_pack_entry_find(p, "data/readme.txt", NUE_PACK_VARIANT_BC);
    CHECK(e != NUE_ASSET_NO_NODE);
    const void* bytes = nullptr;
    size_t n = 0;
    nue_bool owned = NUE_FALSE;
    CHECK_EQ(nue_pack_entry_data(p, e, &bytes, &n, &owned), NUE_OK);
    CHECK(owned);                                                                         // the text was compressed: decoded into a new buffer
    CHECK(n == text.size() && std::memcmp(bytes, text.data(), n) == 0);
    nue_free(const_cast<void*>(bytes));
    nue_pack_entry_info ei;
    NUE_STRUCT_INIT(&ei);
    nue_pack_entry_get(p, e, &ei);
    CHECK(ei.stored_size < ei.raw_size);
    e = nue_pack_entry_find(p, "data/noise.bin", NUE_PACK_VARIANT_BC);
    CHECK_EQ(nue_pack_entry_data(p, e, &bytes, &n, &owned), NUE_OK);
    CHECK(!owned);                                                                        // stored raw: a pointer into the pack, no copy
    CHECK(bytes >= data && bytes < static_cast<uint8_t*>(data) + size);
    nue_pack_entry_info a, b2;
    NUE_STRUCT_INIT(&a); NUE_STRUCT_INIT(&b2);
    nue_pack_entry_get(p, e, &a);
    nue_pack_entry_get(p, nue_pack_entry_find(p, "data/same.bin", NUE_PACK_VARIANT_BC), &b2);
    CHECK_EQ(a.offset, b2.offset);
    e = nue_pack_entry_find(p, "data/empty.bin", NUE_PACK_VARIANT_BC);
    CHECK_EQ(nue_pack_entry_data(p, e, &bytes, &n, &owned), NUE_OK);
    CHECK_EQ(n, size_t(0));
    if (owned) nue_free(const_cast<void*>(bytes));
    // texture variants of a loose image; mips keep their count
    e = nue_pack_entry_find(p, "ui/logo", NUE_PACK_VARIANT_ASTC);
    nue_tex_image back = nullptr;
    CHECK_EQ(nue_pack_entry_image(p, e, &back), NUE_OK);
    nue_tex_image_desc bd;
    NUE_STRUCT_INIT(&bd);
    nue_tex_image_get_desc(back, &bd);
    CHECK_EQ(bd.width, 32u);
    CHECK_EQ(bd.mip_levels, 6u);
    nue_tex_image_destroy(back);
    // a pack without a tree cannot be exported
    nue_asset_container x = nullptr;
    CHECK_EQ(nue_pack_export_container(p, &x), NUE_ERR_UNSUPPORTED);
    nue_pack_close(p);
    nue_free(data);
    nue_tex_image_destroy(im);
}

static void NUE_CALL on_read(void* user, nue_io_request, nue_result r, size_t bytes) { auto* v = static_cast<std::pair<int, size_t>*>(user); v->first = r >= 0 ? 1 : -1; v->second = bytes; }

TEST_CASE(mount_with_asynchronous_reads) {
    Fx fx;
    auto bun = make_bun(default_textures(), true);
    const std::string dir = nue_test::unique_temp_dir("assets_mount");
    nue_asset_container c = fx.open(bun);
    auto pack = build_pack(fx, c, 1u << NUE_PACK_VARIANT_ASTC);
    const std::string pack_path = dir + "/test.nuepack";
    CHECK_EQ(nue_file_save(pack_path.c_str(), pack.data(), pack.size()), NUE_OK);

    nue_pack_mount m = nullptr;
    CHECK_EQ(nue_pack_mount_create(fx.ctx, pack_path.c_str(), NUE_PACK_VARIANT_SOURCE, &m), NUE_OK);
    CHECK(nue_pack_mount_exists(m, "SYNTHETIC/TEX_BGRA_8"));
    CHECK(!nue_pack_mount_exists(m, "SYNTHETIC/NOPE"));
    uint64_t sz = 0;
    CHECK_EQ(nue_pack_mount_size(m, "SYNTHETIC/TEX_BGRA_8", &sz), NUE_OK);
    CHECK_EQ(sz, uint64_t(8 * 8 * 4));
    nue_pack_file f = nullptr;
    CHECK_EQ(nue_pack_file_open(m, "SYNTHETIC/TEX_BGRA_8", &f), NUE_OK);
    CHECK_EQ(nue_pack_file_size(f), uint64_t(256));
    auto expect = tex_data(default_textures()[2]).image;
    std::vector<uint8_t> a(256), b(100);
    size_t got = 0;
    CHECK_EQ(nue_pack_file_read_at(f, 0, a.data(), a.size(), &got), NUE_OK);
    CHECK(got == 256 && a == expect);
    CHECK_EQ(nue_pack_file_read_at(f, 200, b.data(), b.size(), &got), NUE_OK_PARTIAL);      // clipped at the end of the entry
    CHECK_EQ(got, size_t(56));
    CHECK(std::equal(b.begin(), b.begin() + 56, expect.begin() + 200));
    // asynchronous read straight from the pack file
    std::vector<uint8_t> c2(128, 0);
    std::pair<int, size_t> done{0, 0};
    nue_io_request req = 0;
    CHECK(nue_pack_file_read_async(f, 64, c2.data(), c2.size(), on_read, &done, &req) >= 0);
    CHECK(req != 0);                                                                         // a real request: the pack is on disk
    size_t bytes = 0;
    CHECK(nue_io_wait(req, 5000000000ll, &bytes) >= 0);
    CHECK_EQ(bytes, size_t(128));
    nue_io_release(req);
    CHECK(std::equal(c2.begin(), c2.end(), expect.begin() + 64));
    nue_pack_file_close(f);
    // a compressed entry (a tree blob) is decoded when opened; the async read then completes at once
    const std::string packed_blob = "node/" + std::to_string(nue_asset_node_find(c, 0x00030220u, 0));
    CHECK(nue_pack_mount_exists(m, packed_blob.c_str()));
    nue_pack_file nf = nullptr;
    CHECK_EQ(nue_pack_file_open(m, packed_blob.c_str(), &nf), NUE_OK);
    CHECK_EQ(nue_pack_file_size(nf), uint64_t(3000));
    std::vector<uint8_t> nb(size_t(nue_pack_file_size(nf)));
    done = {0, 0};
    CHECK(nue_pack_file_read_async(nf, 0, nb.data(), nb.size(), on_read, &done, &req) >= 0);
    CHECK_EQ(req, 0u);
    CHECK_EQ(done.first, 1);
    CHECK(nb[0] == 'c' && nb[10] == ' ' && nb[11] == 'c');
    nue_pack_file_close(nf);
    nue_pack_mount_destroy(m);

    // a plain BUN on disk mounted read-only: node payloads and textures as DDS
    const std::string bun_path = dir + "/test.bun";
    CHECK_EQ(nue_file_save(bun_path.c_str(), bun.data(), bun.size()), NUE_OK);
    nue_asset_open_desc od;
    NUE_STRUCT_INIT(&od);
    od.path = bun_path.c_str();
    nue_asset_container disk = nullptr;
    CHECK_EQ(nue_asset_container_open(fx.ctx, &od, &disk), NUE_OK);
    nue_pack_mount cm = nullptr;
    CHECK_EQ(nue_pack_mount_create_container(fx.ctx, disk, &cm), NUE_OK);
    uint32_t blob = nue_asset_node_find(disk, 0x00030201u, 0);
    std::string name = "node/" + std::to_string(blob);
    nue_pack_file bf = nullptr;
    CHECK_EQ(nue_pack_file_open(cm, name.c_str(), &bf), NUE_OK);
    CHECK_EQ(nue_pack_file_size(bf), uint64_t(4464));
    std::vector<uint8_t> payload(4464, 0);
    done = {0, 0};
    CHECK(nue_pack_file_read_async(bf, 0, payload.data(), payload.size(), on_read, &done, &req) >= 0);
    CHECK(req != 0);
    CHECK(nue_io_wait(req, 5000000000ll, &bytes) >= 0);
    nue_io_release(req);
    auto want = noise(4464, 3);
    CHECK(payload == want);
    nue_pack_file_close(bf);
    nue_pack_file df = nullptr;
    CHECK_EQ(nue_pack_file_open(cm, "tex_dxt1_16.dds", &df), NUE_OK);
    std::vector<uint8_t> dds(size_t(nue_pack_file_size(df)));
    CHECK_EQ(nue_pack_file_read_at(df, 0, dds.data(), dds.size(), &got), NUE_OK);
    CHECK_EQ(std::memcmp(dds.data(), "DDS ", 4), 0);
    nue_tex_image dim = nullptr;
    CHECK_EQ(nue_tex_image_load_memory(dds.data(), dds.size(), NUE_TEX_FILE_DDS, 0, &dim), NUE_OK);
    nue_tex_image_destroy(dim);
    nue_pack_file_close(df);
    CHECK(!nue_pack_mount_exists(cm, "node/999999"));
    nue_pack_mount_destroy(cm);
    nue_asset_container_close(disk);
    nue_asset_container_close(c);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_MAIN_BEGIN
    RUN(build_read_and_verify);
    RUN(output_is_deterministic);
    RUN(export_gives_the_original_file_back);
    RUN(export_keeps_the_codec_and_edited_textures);
    RUN(damaged_packs);
    RUN(loose_files);
    RUN(mount_with_asynchronous_reads);
TEST_MAIN_END
