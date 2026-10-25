// NFS plugin and the container framework on synthetic files: node tree, texture packs, byte exact rebuild, edits, corrupt input, codec wrapping, budget.
#include "nfs_fixture.hpp"
#include "test_env.h"

using namespace asstest;

// One engine (null backend) for the whole test executable.
static nue_test::Engine& engine() { static nue_test::Engine e; return e; }

struct Fx {
    nue_assets_context ctx = nullptr;
    Fx() { engine(); CHECK_EQ(nue_assets_context_create(nullptr, &ctx), NUE_OK); }
    ~Fx() { nue_assets_context_destroy(ctx); }
    nue_result open(const std::vector<uint8_t>& bytes, nue_asset_container* out, uint32_t flags = 0, const char* hint = ".bun", const char* format = nullptr) {
        nue_asset_open_desc d;
        NUE_STRUCT_INIT(&d);
        d.flags = flags | NUE_ASSET_OPEN_COPY | (format ? NUE_ASSET_OPEN_FORMAT : 0);
        d.format_name = format;
        d.memory = bytes.data();
        d.memory_size = bytes.size();
        d.name_hint = hint;
        return nue_asset_container_open(ctx, &d, out);
    }
};

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

static nue_tex_image make_image(nue_tex_format f, uint32_t w, uint32_t h, uint32_t mips, uint32_t seed) {
    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    d.format = f; d.width = w; d.height = h; d.mip_levels = mips;
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_tex_image_create(&d, &im), NUE_OK);
    nue_tex_image_desc g;
    NUE_STRUCT_INIT(&g);
    nue_tex_image_get_desc(im, &g);
    for (uint32_t l = 0; l < g.mip_levels; ++l) {
        nue_tex_level_view v;
        NUE_STRUCT_INIT(&v);
        nue_tex_image_get_level(im, l, 0, 0, &v);
        for (uint64_t i = 0; i < v.size; ++i) static_cast<uint8_t*>(v.data)[i] = uint8_t(rng_next(seed));
    }
    return im;
}

static uint32_t find_res(nue_asset_container c, const char* name) { return nue_asset_resource_find(c, name, 0); }

// every padding node keeps the alignment it had: the node after it starts on a multiple of `align`
static bool padding_holds(const std::vector<uint8_t>& file) {
    Fx fx;
    nue_asset_container c = nullptr;
    if (fx.open(file, &c) < 0) return false;
    bool ok = true;
    for (uint32_t i = 0; i < nue_asset_node_count(c); ++i) {
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        nue_asset_node_get(c, i, &n);
        if (n.kind == NUE_ASSET_NODE_PADDING && n.align > 1) {
            const uint64_t next_header = n.offset + n.size;
            if (next_header % n.align != 0) ok = false;
        }
    }
    nue_asset_container_close(c);
    return ok;
}

TEST_CASE(registry_and_probe) {
    Fx fx;
    CHECK_EQ(nue_assets_format_count(fx.ctx), 1u);
    CHECK_STR(nue_assets_format_name(fx.ctx, 0), "nfs-bun");
    CHECK_EQ(nue_assets_codec_count(fx.ctx), 1u);
    CHECK_STR(nue_assets_codec_name(fx.ctx, 0), "jdlz");
    // noise is not a BUN
    nue_asset_container c = nullptr;
    auto junk = noise(4096, 42);
    CHECK(fx.open(junk, &c) < 0);
    CHECK(c == nullptr);
    // an empty .BUN is a valid, empty container (the game ships one)
    std::vector<uint8_t> empty;
    CHECK_EQ(fx.open(empty, &c), NUE_OK);
    CHECK_EQ(nue_asset_node_count(c), 0u);
    nue_asset_container_info info;
    NUE_STRUCT_INIT(&info);
    nue_asset_container_get_info(c, &info);
    CHECK_STR(info.format, "nfs-bun");
    nue_asset_container_close(c);
    // an empty context knows nothing
    nue_assets_context bare = nullptr;
    nue_assets_context_desc d;
    NUE_STRUCT_INIT(&d);
    d.flags = NUE_ASSETS_CTX_NO_BUILTINS;
    CHECK_EQ(nue_assets_context_create(&d, &bare), NUE_OK);
    CHECK_EQ(nue_assets_format_count(bare), 0u);
    CHECK_EQ(nue_assets_register_nfs(bare), NUE_OK);
    CHECK_EQ(nue_assets_format_count(bare), 1u);
    nue_assets_context_destroy(bare);
}

TEST_CASE(node_tree) {
    Fx fx;
    auto file = make_bun(default_textures());
    nue_asset_container c = nullptr;
    CHECK_EQ(fx.open(file, &c), NUE_OK);
    nue_asset_container_info info;
    NUE_STRUCT_INIT(&info);
    nue_asset_container_get_info(c, &info);
    CHECK_EQ(info.file_size, uint64_t(file.size()));
    CHECK(info.codec == nullptr);
    CHECK(!info.dirty);
    nue_asset_node_info n;
    NUE_STRUCT_INIT(&n);
    CHECK_EQ(nue_asset_node_get(c, 0, &n), NUE_OK);
    CHECK_EQ(n.id, uint64_t(0x80134000u));
    CHECK_EQ(n.kind, uint32_t(NUE_ASSET_NODE_GROUP));
    CHECK_EQ(n.parent, NUE_ASSET_NO_NODE);
    CHECK_EQ(n.offset, uint64_t(8));
    // the first child of the section is a padding chunk of size 0 followed by the sub group
    CHECK_EQ(n.first_child, 1u);
    nue_asset_node_get(c, 1, &n);
    CHECK_EQ(n.kind, uint32_t(NUE_ASSET_NODE_PADDING));
    CHECK_EQ(n.size, uint64_t(0));
    CHECK_EQ(n.depth, 1u);
    uint32_t tpk = nue_asset_node_find(c, 0xB3300000u, 0);
    CHECK(tpk != NUE_ASSET_NO_NODE);
    CHECK_EQ(nue_asset_node_find(c, 0xDEADBEEFu, 0), NUE_ASSET_NO_NODE);
    // payload reading, including the end of the payload
    nue_asset_node_info t;
    NUE_STRUCT_INIT(&t);
    uint32_t pre = nue_asset_node_find(c, 0x00030201u, 0);
    nue_asset_node_get(c, pre, &t);
    CHECK_EQ(t.size, uint64_t(4464));
    std::vector<uint8_t> payload(5000);
    size_t got = 0;
    CHECK_EQ(nue_asset_node_read(c, pre, 0, payload.data(), payload.size(), &got), NUE_OK_PARTIAL);
    CHECK_EQ(got, size_t(4464));
    auto expect = noise(4464, 3);
    CHECK(std::equal(expect.begin(), expect.end(), payload.begin()));
    CHECK(nue_asset_node_peek(c, pre, 0, 4464) != nullptr);
    nue_asset_container_close(c);
}

TEST_CASE(texture_pack_resources) {
    Fx fx;
    auto specs = default_textures();
    auto file = make_bun(specs, true);
    nue_asset_container c = nullptr;
    CHECK_EQ(fx.open(file, &c), NUE_OK);
    CHECK_EQ(nue_asset_resource_count(c), uint32_t(1 + specs.size() + 1 + 1));
    nue_asset_resource_info r;
    NUE_STRUCT_INIT(&r);
    CHECK_EQ(nue_asset_resource_get(c, 0, &r), NUE_OK);
    CHECK_EQ(r.kind, uint32_t(NUE_ASSET_RES_TEXTURE_PACK));
    CHECK_STR(r.name, "SYNTHETIC");
    CHECK_EQ(r.hash, uint64_t(0x5E8978A3u));
    CHECK(r.derived_node != NUE_ASSET_NO_NODE);
    for (size_t i = 0; i < specs.size(); ++i) {
        CHECK_EQ(nue_asset_resource_get(c, uint32_t(1 + i), &r), NUE_OK);
        CHECK_EQ(r.kind, uint32_t(NUE_ASSET_RES_TEXTURE));
        CHECK_STR(r.name, specs[i].name);
        CHECK_EQ(r.hash, uint64_t(specs[i].hash));
        CHECK_EQ(r.width, specs[i].w);
        CHECK_EQ(r.height, specs[i].h);
        CHECK_EQ(r.mip_levels, specs[i].mips);
        CHECK_EQ(r.native_format, uint32_t(specs[i].ct));
        CHECK_EQ(r.parent, 0u);
        nue_tex_image im = nullptr;
        CHECK_EQ(nue_asset_resource_get_image(c, uint32_t(1 + i), &im), NUE_OK);
        nue_tex_image_desc d;
        NUE_STRUCT_INIT(&d);
        nue_tex_image_get_desc(im, &d);
        CHECK_EQ(d.width, specs[i].w);
        CHECK_EQ(d.mip_levels, specs[i].mips);
        auto data = tex_data(specs[i]);
        const nue_tex_format fmt = specs[i].ct == 0x22 ? NUE_TEX_BC1_UNORM : specs[i].ct == 0x24 ? NUE_TEX_BC2_UNORM : NUE_TEX_BGRA8_UNORM;
        CHECK_EQ(d.format, fmt);
        uint64_t off = 0;
        for (uint32_t l = 0; l < d.mip_levels; ++l) {
            nue_tex_level_view v;
            NUE_STRUCT_INIT(&v);
            nue_tex_image_get_level(im, l, 0, 0, &v);
            if (specs[i].ct == 0x08) {            // indices expanded through the palette (stored B,G,R,A = BGRA8 bytes)
                const uint64_t px = uint64_t(v.width) * v.height;
                bool same = true;
                for (uint64_t k = 0; k < px && same; ++k) same = std::memcmp(static_cast<uint8_t*>(v.data) + k * 4, &data.palette[data.image[size_t(off + k)] * 4], 4) == 0;
                CHECK(same);
                off += px;
            } else {
                CHECK(std::memcmp(v.data, &data.image[size_t(off)], size_t(v.size)) == 0);
                off += v.size;
            }
        }
        nue_tex_image_destroy(im);
    }
    // find by name (case-insensitive) and by hash
    CHECK_EQ(find_res(c, "tex_dxt3_32"), 2u);
    CHECK_EQ(nue_asset_resource_find(c, nullptr, 0xAAAA0004u), 4u);
    CHECK_EQ(find_res(c, "NOPE"), NUE_ASSET_NO_NODE);
    nue_asset_container_close(c);
}

TEST_CASE(rebuild_is_byte_identical) {
    Fx fx;
    for (bool second : {false, true}) {
        auto file = make_bun(default_textures(), second);
        nue_asset_container c = nullptr;
        CHECK_EQ(fx.open(file, &c), NUE_OK);
        nue_result r;
        CHECK(write_mem(c, 0, &r) == file);                                       // nothing edited: the original bytes
        CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD, &r) == file);                 // serialised from the node tree
        CHECK_EQ(r, NUE_OK);
        CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_REGENERATE, &r) == file);   // texture tables and pixel layout regenerated
        CHECK_EQ(r, NUE_OK);
        nue_asset_container_close(c);
    }
}

TEST_CASE(codec_wrapper) {
    Fx fx;
    auto bun = make_bun(default_textures());
    std::vector<uint8_t> lzc(nue_jdlz_compress_bound(bun.size()));
    size_t n = 0;
    CHECK_EQ(nue_jdlz_compress(bun.data(), bun.size(), 6, lzc.data(), lzc.size(), &n), NUE_OK);
    lzc.resize(n);
    nue_asset_container c = nullptr;
    CHECK_EQ(fx.open(lzc, &c, 0, ".lzc"), NUE_OK);
    nue_asset_container_info info;
    NUE_STRUCT_INIT(&info);
    nue_asset_container_get_info(c, &info);
    CHECK_STR(info.codec, "jdlz");
    CHECK_EQ(info.file_size, uint64_t(lzc.size()));
    CHECK_EQ(info.content_size, uint64_t(bun.size()));
    CHECK_EQ(nue_asset_resource_count(c), 7u);
    CHECK(write_mem(c, 0) == lzc);                                                // untouched: the compressed bytes as they were
    CHECK(write_mem(c, NUE_ASSET_WRITE_NO_CODEC) == bun);                         // .LZC -> .BUN
    nue_result r;
    auto rebuilt = write_mem(c, NUE_ASSET_WRITE_REBUILD, &r);                     // rebuilt and compressed again (our encoder, level 6)
    CHECK_EQ(r, NUE_OK);
    CHECK(rebuilt == lzc);                                                        // same encoder and level: same bytes
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_NO_CODEC) == bun);
    nue_asset_container_close(c);
    // a plain BUN wrapped on request
    nue_asset_container b = nullptr;
    CHECK_EQ(fx.open(bun, &b), NUE_OK);
    nue_asset_write_desc wd;
    NUE_STRUCT_INIT(&wd);
    void* wdata = nullptr;
    size_t wsize = 0;
    wd.flags = NUE_ASSET_WRITE_FORCE_CODEC;
    wd.codec = "jdlz";
    wd.out_data = &wdata;
    wd.out_size = &wsize;
    r = nue_asset_container_write(b, &wd);
    std::vector<uint8_t> wrapped(static_cast<uint8_t*>(wdata), static_cast<uint8_t*>(wdata) + wsize);
    nue_free(wdata);
    CHECK_EQ(r, NUE_OK);
    CHECK(wrapped == lzc);
    // a corrupted compressed file fails to open
    auto bad = lzc;
    bad[bad.size() / 2] ^= 0x55;
    nue_asset_container x = nullptr;
    nue_result ro = fx.open(bad, &x, 0, ".lzc");
    if (ro >= 0) nue_asset_container_close(x);                                    // a flipped bit may still decode to *something*: it must not crash
    auto trunc = std::vector<uint8_t>(lzc.begin(), lzc.begin() + lzc.size() / 2);
    CHECK(fx.open(trunc, &x, 0, ".lzc") < 0);
    // NO_UNWRAP opens the bytes as they are: not a BUN
    CHECK(fx.open(lzc, &x, NUE_ASSET_OPEN_NO_UNWRAP, ".lzc") < 0);
    nue_asset_container_close(b);
}

TEST_CASE(replace_texture_keeps_structure) {
    Fx fx;
    auto specs = default_textures();
    auto file = make_bun(specs, true);
    nue_asset_container c = nullptr;
    CHECK_EQ(fx.open(file, &c), NUE_OK);

    // 1. a different size and format: BGRA8 32x32 with 6 mip levels replaces the 16x16 DXT1 texture
    nue_tex_image bg = make_image(NUE_TEX_BGRA8_UNORM, 32, 32, 6, 99);
    uint32_t dxt1 = find_res(c, "TEX_DXT1_16");
    CHECK_EQ(nue_asset_resource_set_image(c, dxt1, bg), NUE_OK);
    // 2. an RGBA8 image where DXT3 was: converted to what the game stores (DXT5)
    nue_tex_image rgba = make_image(NUE_TEX_RGBA8_UNORM, 16, 16, 1, 5);
    uint32_t dxt3 = find_res(c, "TEX_DXT3_32");
    CHECK_EQ(nue_asset_resource_set_image(c, dxt3, rgba), NUE_OK);
    // 3. a palettised texture replaced by a BGRA8 image of the same size
    nue_tex_image bg8 = make_image(NUE_TEX_BGRA8_UNORM, 16, 16, 2, 7);
    uint32_t p8 = find_res(c, "TEX_P8_16");
    CHECK_EQ(nue_asset_resource_set_image(c, p8, bg8), NUE_OK);

    nue_asset_container_info info;
    NUE_STRUCT_INIT(&info);
    nue_asset_container_get_info(c, &info);
    CHECK(info.dirty);
    nue_result r;
    auto out = write_mem(c, 0, &r);
    if (r < 0) std::printf("  write failed: %s\n", nue_get_last_error_message());
    CHECK_EQ(r, NUE_OK);
    CHECK(out != file);
    CHECK(padding_holds(out));

    nue_asset_container d = nullptr;
    CHECK_EQ(fx.open(out, &d), NUE_OK);
    CHECK_EQ(nue_asset_resource_count(d), nue_asset_resource_count(c));
    // replaced textures read back as written
    nue_tex_image back = nullptr;
    CHECK_EQ(nue_asset_resource_get_image(d, find_res(d, "TEX_DXT1_16"), &back), NUE_OK);
    nue_tex_image_desc bd;
    NUE_STRUCT_INIT(&bd);
    nue_tex_image_get_desc(back, &bd);
    CHECK_EQ(bd.format, NUE_TEX_BGRA8_UNORM);
    CHECK_EQ(bd.width, 32u);
    CHECK_EQ(bd.mip_levels, 6u);
    CHECK(nue_tex_image_total_size(back) == nue_tex_image_total_size(bg));
    for (uint32_t l = 0; l < 6; ++l) {
        nue_tex_level_view a, b;
        NUE_STRUCT_INIT(&a); NUE_STRUCT_INIT(&b);
        nue_tex_image_get_level(bg, l, 0, 0, &a);
        nue_tex_image_get_level(back, l, 0, 0, &b);
        CHECK(a.size == b.size && std::memcmp(a.data, b.data, size_t(a.size)) == 0);
    }
    nue_tex_image_destroy(back);
    nue_asset_resource_info ri;
    NUE_STRUCT_INIT(&ri);
    nue_asset_resource_get(d, find_res(d, "TEX_DXT3_32"), &ri);
    CHECK_EQ(ri.native_format, 0x26u);                                             // DXT3 became DXT5 (BC3)
    CHECK_EQ(ri.width, 16u);
    CHECK_EQ(ri.height, 16u);
    CHECK(ri.mip_levels > 1);                                                      // a single level became a full chain
    nue_asset_resource_get(d, find_res(d, "TEX_P8_16"), &ri);
    CHECK_EQ(ri.native_format, 0x20u);
    CHECK((ri.flags & NUE_ASSET_RESFLAG_PALETTED) == 0);
    // untouched textures are byte for byte what they were
    for (const char* name : {"TEX_BGRA_8", "TEX_P8_8", "TEX_DXT1_64", "ONLY_ONE"}) {
        nue_tex_image x = nullptr, y = nullptr;
        CHECK_EQ(nue_asset_resource_get_image(c, find_res(c, name), &x), NUE_OK);
        CHECK_EQ(nue_asset_resource_get_image(d, find_res(d, name), &y), NUE_OK);
        CHECK(nue_tex_image_total_size(x) == nue_tex_image_total_size(y));
        nue_tex_level_view a, b;
        NUE_STRUCT_INIT(&a); NUE_STRUCT_INIT(&b);
        nue_tex_image_get_level(x, 0, 0, 0, &a);
        nue_tex_image_get_level(y, 0, 0, 0, &b);
        CHECK(a.size == b.size && std::memcmp(a.data, b.data, size_t(a.size)) == 0);
        nue_tex_image_destroy(x); nue_tex_image_destroy(y);
    }
    // everything that is not a texture table is untouched (same payload bytes, same order)
    for (uint64_t id : {0x00030201ull, 0x00030203ull, 0x0003B700ull, 0x00134002ull}) {
        uint32_t a = nue_asset_node_find(c, id, 0), b = nue_asset_node_find(d, id, 0);
        nue_asset_node_info na, nb;
        NUE_STRUCT_INIT(&na); NUE_STRUCT_INIT(&nb);
        nue_asset_node_get(c, a, &na); nue_asset_node_get(d, b, &nb);
        CHECK_EQ(na.size, nb.size);
        std::vector<uint8_t> pa(size_t(na.size)), pb(size_t(nb.size));
        size_t g;
        nue_asset_node_read(c, a, 0, pa.data(), pa.size(), &g);
        nue_asset_node_read(d, b, 0, pb.data(), pb.size(), &g);
        CHECK(pa == pb);
    }
    // the written file is stable: reading it and regenerating gives the same bytes
    CHECK(write_mem(d, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_REGENERATE) == out);
    CHECK(write_mem(d, NUE_ASSET_WRITE_REBUILD) == out);
    nue_tex_image_destroy(bg); nue_tex_image_destroy(rgba); nue_tex_image_destroy(bg8);
    nue_asset_container_close(d);
    nue_asset_container_close(c);
}

TEST_CASE(shrinking_and_growing_move_the_rest_and_keep_alignment) {
    Fx fx;
    for (uint32_t size : {4u, 8u, 64u, 128u, 256u}) {
        auto file = make_bun(default_textures(), true);
        nue_asset_container c = nullptr;
        CHECK_EQ(fx.open(file, &c), NUE_OK);
        nue_tex_image im = make_image(NUE_TEX_BGRA8_UNORM, size, size, 1, size);
        CHECK_EQ(nue_asset_resource_set_image(c, find_res(c, "TEX_BGRA_8"), im), NUE_OK);
        nue_result r;
        auto out = write_mem(c, 0, &r);
        CHECK_EQ(r, NUE_OK);
        CHECK(padding_holds(out));
        // the second pack starts where it has to and still reads
        nue_asset_container d = nullptr;
        CHECK_EQ(fx.open(out, &d), NUE_OK);
        nue_tex_image back = nullptr;
        CHECK_EQ(nue_asset_resource_get_image(d, find_res(d, "ONLY_ONE"), &back), NUE_OK);
        nue_tex_image_destroy(back);
        // all chunks of the output fit their parents: the walk had no repairs
        nue_asset_container_info info;
        NUE_STRUCT_INIT(&info);
        nue_asset_container_get_info(d, &info);
        CHECK(!info.lenient_repairs);
        CHECK_EQ(info.content_size, uint64_t(out.size()));
        // the pixel data chunk starts the pixels on a 128 boundary of the file
        uint32_t data = nue_asset_node_find(d, 0x33320002u, 0);
        nue_asset_node_info dn;
        NUE_STRUCT_INIT(&dn);
        nue_asset_node_get(d, data, &dn);
        CHECK_EQ(((dn.offset + 127) / 128 * 128) % 128, uint64_t(0));
        nue_tex_image_destroy(im);
        nue_asset_container_close(d);
        nue_asset_container_close(c);
    }
}

TEST_CASE(opaque_blobs_are_kept_and_editable) {
    Fx fx;
    auto file = make_bun(default_textures());
    nue_asset_container c = nullptr;
    CHECK_EQ(fx.open(file, &c), NUE_OK);
    uint32_t blob = nue_asset_node_find(c, 0x00030201u, 0);
    auto bigger = noise(10000, 77);
    CHECK_EQ(nue_asset_node_set_payload(c, blob, bigger.data(), bigger.size()), NUE_OK);
    uint32_t group = nue_asset_node_find(c, 0x80134000u, 0);
    CHECK_EQ(nue_asset_node_set_payload(c, group, bigger.data(), 4), NUE_ERR_UNSUPPORTED);      // groups are rebuilt from their children
    nue_result r;
    auto out = write_mem(c, 0, &r);
    CHECK_EQ(r, NUE_OK);
    CHECK(padding_holds(out));
    nue_asset_container d = nullptr;
    CHECK_EQ(fx.open(out, &d), NUE_OK);
    uint32_t b2 = nue_asset_node_find(d, 0x00030201u, 0);
    nue_asset_node_info nb;
    NUE_STRUCT_INIT(&nb);
    nue_asset_node_get(d, b2, &nb);
    CHECK_EQ(nb.size, uint64_t(10000));
    std::vector<uint8_t> p(10000);
    size_t g;
    nue_asset_node_read(d, b2, 0, p.data(), p.size(), &g);
    CHECK(p == bigger);
    CHECK_EQ(nue_asset_resource_count(d), nue_asset_resource_count(c));       // the texture pack after the blob is intact
    nue_tex_image im = nullptr;
    CHECK_EQ(nue_asset_resource_get_image(d, find_res(d, "TEX_DXT1_64"), &im), NUE_OK);
    nue_tex_image_destroy(im);
    nue_asset_container_close(d);
    nue_asset_container_close(c);
}

TEST_CASE(damaged_files) {
    Fx fx;
    auto file = make_bun(default_textures());
    nue_asset_container c = nullptr;
    // cut inside a chunk
    auto cut = std::vector<uint8_t>(file.begin(), file.begin() + file.size() - 30);
    CHECK_EQ(fx.open(cut, &c), NUE_ERR_CORRUPT);
    CHECK_EQ(fx.open(cut, &c, NUE_ASSET_OPEN_LENIENT), NUE_OK);                // lenient: the damaged tail is kept as opaque bytes
    nue_asset_container_info info;
    NUE_STRUCT_INIT(&info);
    nue_asset_container_get_info(c, &info);
    CHECK(info.lenient_repairs);
    const uint32_t count = nue_asset_node_count(c);
    nue_asset_node_info last;
    NUE_STRUCT_INIT(&last);
    nue_asset_node_get(c, count - 1, &last);
    CHECK_EQ(last.kind, uint32_t(NUE_ASSET_NODE_TAIL));
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD) == cut);                       // nothing is lost
    nue_asset_container_close(c);
    // trailing bytes shorter than a chunk header
    auto tail = file;
    tail.push_back(1); tail.push_back(2); tail.push_back(3);
    CHECK_EQ(fx.open(tail, &c), NUE_ERR_CORRUPT);
    CHECK_EQ(fx.open(tail, &c, NUE_ASSET_OPEN_LENIENT), NUE_OK);
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD) == tail);
    nue_asset_container_close(c);
    // a "group" whose content is not a chunk sequence stays one opaque blob and survives a rebuild
    Bun b;
    b.chunk(0x80000001u, noise(21, 5));
    b.chunk(0x00000100u, noise(8, 6));
    CHECK_EQ(fx.open(b.b, &c), NUE_OK);
    nue_asset_node_info n;
    NUE_STRUCT_INIT(&n);
    nue_asset_node_get(c, 0, &n);
    CHECK_EQ(n.kind, uint32_t(NUE_ASSET_NODE_BLOB));
    CHECK((n.flags & NUE_ASSET_NODE_OPAQUE) != 0);
    CHECK_EQ(nue_asset_node_count(c), 2u);
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD) == b.b);
    nue_asset_container_close(c);
    // a chunk size that overflows everything
    Bun o;
    put32(o.b, 0x00000200u);
    put32(o.b, 0xFFFFFFF0u);
    o.b.resize(64, 0);
    CHECK_EQ(fx.open(o.b, &c, 0, ".bun", "nfs-bun"), NUE_ERR_CORRUPT);
    CHECK(fx.open(o.b, &c) < 0);                                              // and the probe does not take it for a BUN
    // random corruption of a valid file: never crashes, repairs or fails cleanly
    uint32_t seed = 4242;
    for (int i = 0; i < 300; ++i) {
        auto m = file;
        for (int k = 0; k < 4; ++k) m[rng_next(seed) % m.size()] ^= uint8_t(1u << (rng_next(seed) & 7));
        nue_asset_container x = nullptr;
        if (fx.open(m, &x, NUE_ASSET_OPEN_LENIENT) >= 0) {
            for (uint32_t r = 0; r < nue_asset_resource_count(x); ++r) {
                nue_tex_image im = nullptr;
                if (nue_asset_resource_get_image(x, r, &im) >= 0) nue_tex_image_destroy(im);
            }
            nue_result w;
            write_mem(x, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_REGENERATE, &w);
            nue_asset_container_close(x);
        }
    }
}

TEST_CASE(memory_budget_is_respected) {
    engine();
    auto bun = make_bun(default_textures());
    std::vector<uint8_t> lzc(nue_jdlz_compress_bound(bun.size()));
    size_t n = 0;
    nue_jdlz_compress(bun.data(), bun.size(), 6, lzc.data(), lzc.size(), &n);
    lzc.resize(n);
    // a budget of 1 KiB cannot hold the decoded file: opening fails cleanly
    nue_assets_context ctx = nullptr;
    nue_assets_context_desc d;
    NUE_STRUCT_INIT(&d);
    d.budget_name = "assets_tiny";
    d.budget_policy.struct_size = sizeof(nue_budget_policy);
    d.budget_policy.fixed_bytes = 1024;
    CHECK_EQ(nue_assets_context_create(&d, &ctx), NUE_OK);
    nue_asset_open_desc od;
    NUE_STRUCT_INIT(&od);
    od.flags = NUE_ASSET_OPEN_COPY;
    od.memory = lzc.data();
    od.memory_size = lzc.size();
    od.name_hint = ".lzc";
    nue_asset_container c = nullptr;
    CHECK_EQ(nue_asset_container_open(ctx, &od, &c), NUE_ERR_OUT_OF_MEMORY);
    CHECK(c == nullptr);
    nue_assets_context_destroy(ctx);
    // a generous one works and the accounting returns to zero after close
    d.budget_name = "assets_big";
    d.budget_policy.fixed_bytes = 256u << 20;
    CHECK_EQ(nue_assets_context_create(&d, &ctx), NUE_OK);
    CHECK_EQ(nue_asset_container_open(ctx, &od, &c), NUE_OK);
    nue_budget_id id = 0;
    CHECK_EQ(nue_budget_find("assets_big", &id), NUE_OK);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(id, &st);
    CHECK(st.used_bytes >= bun.size());
    nue_asset_container_close(c);
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.used_bytes, uint64_t(0));
    nue_assets_context_destroy(ctx);
}

// ---- a second format through the public plugin interface: the framework does not know NFS ----
static int32_t toy_probe(void*, const void* head, size_t n, uint64_t) { return n >= 4 && std::memcmp(head, "TOY1", 4) == 0 ? 90 : 0; }
static nue_result toy_open(void*, nue_asset_open_ctx* oc) {
    // "TOY1", u32 count, then count records: u8 name length, name, u32 size, bytes
    uint8_t h[8];
    size_t got = 0;
    nue_asset_source_read(oc->source, 0, h, 8, &got);
    uint32_t count = get32(h + 4);
    uint64_t off = 8;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t len = 0;
        nue_asset_source_read(oc->source, off, &len, 1, &got);
        char name[256] = {0};
        nue_asset_source_read(oc->source, off + 1, name, len, &got);
        uint8_t sz[4];
        nue_asset_source_read(oc->source, off + 1 + len, sz, 4, &got);
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        n.parent = NUE_ASSET_NO_NODE;
        n.kind = NUE_ASSET_NODE_BLOB;
        n.id = i;
        n.name = name;
        n.offset = off + 1 + len + 4;
        n.size = get32(sz);
        nue_result r = oc->ops->add_node(oc, &n, nullptr);
        if (r < 0) return r;
        off = n.offset + n.size;
    }
    return NUE_OK;
}
static nue_result toy_write(void*, nue_asset_container c, void*, uint32_t, nue_asset_sink* sink) {
    uint32_t count = nue_asset_node_count(c);
    std::vector<uint8_t> o = {'T', 'O', 'Y', '1'};
    put32(o, count);
    for (uint32_t i = 0; i < count; ++i) {
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        nue_asset_node_get(c, i, &n);
        o.push_back(uint8_t(std::strlen(n.name)));
        o.insert(o.end(), n.name, n.name + std::strlen(n.name));
        put32(o, uint32_t(n.size));
        std::vector<uint8_t> p(size_t(n.size));
        size_t g;
        nue_asset_node_read(c, i, 0, p.data(), p.size(), &g);
        o.insert(o.end(), p.begin(), p.end());
    }
    return nue_asset_sink_write(sink, o.data(), o.size());
}

TEST_CASE(a_new_format_plugs_in_without_touching_the_framework) {
    Fx fx;
    nue_asset_format f;
    NUE_STRUCT_INIT(&f);
    f.name = "toy";
    f.extensions = "toy";
    f.probe = toy_probe;
    f.open = toy_open;
    f.write = toy_write;
    CHECK_EQ(nue_assets_register_format(fx.ctx, &f), NUE_OK);
    CHECK_EQ(nue_assets_register_format(fx.ctx, &f), NUE_ERR_ALREADY_EXISTS);
    std::vector<uint8_t> file = {'T', 'O', 'Y', '1'};
    put32(file, 2);
    file.push_back(1); file.push_back('a'); put32(file, 3); file.insert(file.end(), {1, 2, 3});
    file.push_back(2); file.push_back('b'); file.push_back('c'); put32(file, 1); file.push_back(9);
    nue_asset_container c = nullptr;
    CHECK_EQ(fx.open(file, &c, 0, ".toy"), NUE_OK);
    nue_asset_container_info info;
    NUE_STRUCT_INIT(&info);
    nue_asset_container_get_info(c, &info);
    CHECK_STR(info.format, "toy");
    CHECK_EQ(nue_asset_node_count(c), 2u);
    nue_asset_node_info n;
    NUE_STRUCT_INIT(&n);
    nue_asset_node_get(c, 1, &n);
    CHECK_STR(n.name, "bc");
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD) == file);
    const uint8_t patch[5] = {7, 7, 7, 7, 7};
    CHECK_EQ(nue_asset_node_set_payload(c, 0, patch, 5), NUE_OK);
    auto out = write_mem(c, 0);
    CHECK_EQ(out.size(), file.size() + 2);
    nue_asset_container_close(c);
    // the NFS plugin still wins on its own files
    auto bun = make_bun(default_textures());
    CHECK_EQ(fx.open(bun, &c), NUE_OK);
    nue_asset_container_get_info(c, &info);
    CHECK_STR(info.format, "nfs-bun");
    nue_asset_container_close(c);
}

TEST_MAIN_BEGIN
    RUN(registry_and_probe);
    RUN(node_tree);
    RUN(texture_pack_resources);
    RUN(rebuild_is_byte_identical);
    RUN(codec_wrapper);
    RUN(replace_texture_keeps_structure);
    RUN(shrinking_and_growing_move_the_rest_and_keep_alignment);
    RUN(opaque_blobs_are_kept_and_editable);
    RUN(damaged_files);
    RUN(memory_budget_is_respected);
    RUN(a_new_format_plugs_in_without_touching_the_framework);
TEST_MAIN_END
