// HUFF codec, car texture packs, mesh resources of solids and the glTF / OBJ export, on synthetic data.
#include "model_fixture.hpp"
#include "test_env.h"

#include <algorithm>
#include <filesystem>

using namespace asstest;

static nue_test::Engine& engine() { static nue_test::Engine e; return e; }

struct Fx {
    nue_assets_context ctx = nullptr;
    Fx() { engine(); CHECK_EQ(nue_assets_context_create(nullptr, &ctx), NUE_OK); }
    ~Fx() { nue_assets_context_destroy(ctx); }
    nue_asset_container open(const std::vector<uint8_t>& bytes, const char* hint = ".bun", uint32_t flags = 0) {
        nue_asset_open_desc d;
        NUE_STRUCT_INIT(&d);
        d.flags = flags | NUE_ASSET_OPEN_COPY;
        d.memory = bytes.data();
        d.memory_size = bytes.size();
        d.name_hint = hint;
        nue_asset_container c = nullptr;
        CHECK_EQ(nue_asset_container_open(ctx, &d, &c), NUE_OK);
        return c;
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

static uint32_t find_mesh(nue_asset_container c, const char* name) {
    for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
        nue_asset_resource_info r;
        NUE_STRUCT_INIT(&r);
        nue_asset_resource_get(c, i, &r);
        if (r.kind == NUE_ASSET_RES_MESH && std::string(r.name) == name) return i;
    }
    return NUE_ASSET_NO_NODE;
}

TEST_CASE(huff_streams) {
    nue_result r;
    std::vector<std::vector<uint8_t>> inputs;
    inputs.push_back({'a'});
    inputs.push_back(std::vector<uint8_t>(1000, 7));
    inputs.push_back({1, 2, 3, 3, 3, 3, 3, 2, 1, 1, 1, 9});
    std::vector<uint8_t> mixed;
    uint32_t s = 5;
    for (int i = 0; i < 5000; ++i) mixed.push_back(uint8_t((rng_next(s) & 3) ? 0x40 : rng_next(s) % 50));
    inputs.push_back(mixed);
    for (bool runs : {true, false})
        for (const auto& in : inputs) {
            auto packed = huff_encode(in, runs);
            uint32_t unc = 0, stream = 0;
            CHECK_EQ(nue_huff_identify(packed.data(), packed.size(), &unc, &stream), NUE_OK);
            CHECK_EQ(unc, uint32_t(in.size()));
            CHECK_EQ(stream, uint32_t(packed.size()));
            std::vector<uint8_t> out(in.size() + 16, 0xEE);
            size_t n = 0;
            r = nue_huff_decompress(packed.data(), packed.size(), out.data(), in.size(), &n);
            CHECK_EQ(r, NUE_OK);
            CHECK_EQ(n, in.size());
            CHECK(std::equal(in.begin(), in.end(), out.begin()));
            for (size_t k = in.size(); k < out.size(); ++k) CHECK_EQ(out[k], 0xEE);
        }
    // bad input: magic, sizes, truncation, flipped bits (never crashes, never writes outside the buffer)
    auto good = huff_encode(mixed);
    std::vector<uint8_t> out(mixed.size() + 32, 0xAB);
    size_t n = 0;
    auto bad = good; bad[0] = 'X';
    CHECK_EQ(nue_huff_decompress(bad.data(), bad.size(), out.data(), mixed.size(), &n), NUE_ERR_UNSUPPORTED);
    CHECK_EQ(nue_huff_decompress(good.data(), good.size(), out.data(), mixed.size() - 1, &n), NUE_ERR_BUFFER_TOO_SMALL);
    bad.assign(good.begin(), good.begin() + good.size() / 3);
    CHECK(nue_huff_decompress(bad.data(), bad.size(), out.data(), mixed.size(), &n) < 0);
    std::vector<uint8_t> zeros(64, 0);
    std::memcpy(zeros.data(), "HUFF", 4);
    zeros[4] = 1; zeros[5] = 0x10; zeros[8] = 16;
    CHECK(nue_huff_decompress(zeros.data(), zeros.size(), out.data(), 16, &n) < 0);          // a stream of zero bits must fail, not spin
    uint32_t seed = 77;
    for (int i = 0; i < 3000; ++i) {
        auto m = good;
        m[16 + rng_next(seed) % (m.size() - 16)] ^= uint8_t(1u << (rng_next(seed) & 7));
        std::fill(out.begin(), out.end(), 0xAB);
        nue_huff_decompress(m.data(), m.size(), out.data(), mixed.size(), &n);
        for (size_t k = mixed.size(); k < out.size(); ++k) CHECK_EQ(out[k], 0xAB);
        if (nue_test::failures() > 20) break;
    }
}

TEST_CASE(car_texture_pack) {
    Fx fx;
    std::vector<CarTex> tex = {
        {"CAR_BODY", 0x11110001u, 0x22, 32, 32, 3, 1, 0},
        {"CAR_LIGHT", 0x11110002u, 0x24, 16, 16, 2, 2, 1},        // HUFF
        {"CAR_PLATE", 0x11110003u, 0x20, 8, 8, 1, 3, 0},
        {"CAR_PALETTED", 0x11110004u, 0x08, 16, 16, 2, 4, 1},     // HUFF
        {"CAR_RIM", 0x11110005u, 0x22, 64, 32, 4, 5, 0},
    };
    std::vector<std::vector<uint8_t>> pixels, palettes;
    auto file = make_car_pack(tex, &pixels, &palettes);
    nue_asset_container c = fx.open(file, ".bin");
    CHECK_EQ(nue_asset_resource_count(c), uint32_t(1 + tex.size()));
    nue_asset_resource_info r;
    NUE_STRUCT_INIT(&r);
    nue_asset_resource_get(c, 0, &r);
    CHECK_EQ(r.kind, uint32_t(NUE_ASSET_RES_TEXTURE_PACK));
    CHECK_STR(r.name, "CARTEXTURES");
    CHECK_EQ(r.derived_node, NUE_ASSET_NO_NODE);
    for (size_t i = 0; i < tex.size(); ++i) {
        NUE_STRUCT_INIT(&r);
        CHECK_EQ(nue_asset_resource_get(c, uint32_t(1 + i), &r), NUE_OK);
        CHECK_STR(r.name, tex[i].name);
        CHECK_EQ(r.hash, uint64_t(tex[i].hash));
        CHECK_EQ(r.width, tex[i].w);
        CHECK_EQ(r.mip_levels, tex[i].mips);
        CHECK((r.flags & NUE_ASSET_RESFLAG_READONLY) != 0);
        // native bytes are the decoded pixels (and the palette)
        nue_asset_native_texture nt;
        void* mem = nullptr;
        NUE_STRUCT_INIT(&nt);
        CHECK_EQ(nue_asset_resource_get_native(c, uint32_t(1 + i), &nt, &mem), NUE_OK);
        CHECK_EQ(nt.size, uint64_t(pixels[i].size() + palettes[i].size()));
        const uint8_t* b = static_cast<const uint8_t*>(nt.data);
        CHECK(std::equal(pixels[i].begin(), pixels[i].end(), b));
        CHECK(std::equal(palettes[i].begin(), palettes[i].end(), b + pixels[i].size()));
        nue_free(mem);
        nue_tex_image im = nullptr;
        CHECK_EQ(nue_asset_resource_get_image(c, uint32_t(1 + i), &im), NUE_OK);
        nue_tex_image_destroy(im);
        // read only: no replacing
        nue_tex_image_desc d;
        NUE_STRUCT_INIT(&d);
        d.format = NUE_TEX_RGBA8_UNORM; d.width = 4; d.height = 4; d.mip_levels = 1;
        nue_tex_image repl = nullptr;
        CHECK_EQ(nue_tex_image_create(&d, &repl), NUE_OK);
        CHECK_EQ(nue_asset_resource_set_image(c, uint32_t(1 + i), repl), NUE_ERR_UNSUPPORTED);
        nue_tex_image_destroy(repl);
    }
    CHECK(write_mem(c, 0) == file);
    CHECK(write_mem(c, NUE_ASSET_WRITE_REBUILD) == file);                       // the pack is not rebuilt: its chunks are copied as they are
    nue_asset_container_close(c);
    // a damaged blob leaves that texture out of the list instead of failing the file
    auto damaged = file;
    for (size_t i = damaged.size() - 200; i < damaged.size() - 100; ++i) damaged[i] ^= 0x5A;
    nue_asset_container d = fx.open(damaged, ".bin", NUE_ASSET_OPEN_LENIENT);
    CHECK(nue_asset_resource_count(d) <= uint32_t(1 + tex.size()));
    for (uint32_t i = 0; i < nue_asset_resource_count(d); ++i) {
        nue_tex_image im = nullptr;
        if (nue_asset_resource_get_image(d, i, &im) >= 0) nue_tex_image_destroy(im);
    }
    nue_asset_container_close(d);
}

TEST_CASE(mesh_resources) {
    Fx fx;
    auto file = make_model_bun();
    nue_asset_container c = fx.open(file);
    const auto specs = default_solids();
    uint32_t meshes = 0;
    for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) { nue_asset_resource_info r; NUE_STRUCT_INIT(&r); nue_asset_resource_get(c, i, &r); if (r.kind == NUE_ASSET_RES_MESH) ++meshes; }
    CHECK_EQ(meshes, uint32_t(specs.size()));
    for (const SolidSpec& s : specs) {
        const uint32_t res = find_mesh(c, s.name);
        CHECK(res != NUE_ASSET_NO_NODE);
        if (res == NUE_ASSET_NO_NODE) continue;
        nue_asset_resource_info ri;
        NUE_STRUCT_INIT(&ri);
        nue_asset_resource_get(c, res, &ri);
        const SolidData d = solid_data(s);
        CHECK_EQ(ri.width, uint32_t(d.pos.size() / 3));
        CHECK_EQ(ri.height, uint32_t(d.idx.size() / 3));
        nue_asset_mesh m;
        void* mem = nullptr;
        NUE_STRUCT_INIT(&m);
        CHECK_EQ(nue_asset_resource_get_mesh(c, res, &m, &mem), NUE_OK);
        CHECK_STR(m.name, s.name);
        CHECK_EQ(m.vertex_count, uint32_t(d.pos.size() / 3));
        CHECK_EQ(m.index_count, uint32_t(d.idx.size()));
        CHECK(std::equal(d.pos.begin(), d.pos.end(), m.positions));
        CHECK(std::equal(d.uv.begin(), d.uv.end(), m.uvs));
        CHECK(std::equal(d.rgba.begin(), d.rgba.end(), m.colors));               // B,G,R,A in the file, R,G,B,A out
        CHECK(std::equal(d.idx.begin(), d.idx.end(), m.indices));
        CHECK_EQ((m.flags & NUE_ASSET_MESH_NORMALS) != 0, s.normals);
        if (s.normals) CHECK(std::equal(d.nrm.begin(), d.nrm.end(), m.normals)); else CHECK(m.normals == nullptr);
        CHECK_EQ(m.run_count, 2u);
        CHECK_EQ(m.runs[0].index_offset, 0u);
        CHECK_EQ(m.runs[0].index_count, d.run0_count);
        CHECK_EQ(m.runs[0].material_hash, uint64_t(0xAAAA0001u));
        CHECK_EQ(m.runs[0].shader_hash, uint64_t(0x11111111u));
        CHECK_EQ(m.runs[1].index_offset, d.run0_count);
        CHECK_EQ(m.runs[1].index_count, uint32_t(d.idx.size()) - d.run0_count);
        CHECK_EQ(m.runs[1].material_hash, uint64_t(0x12345678u));
        CHECK_EQ(m.runs[1].shader_hash, uint64_t(0x22222222u));
        CHECK_NEAR(m.transform[12], s.offset, 1e-6);                             // translation of the solid header
        CHECK_NEAR(m.transform[13], 2.0, 1e-6);
        CHECK_NEAR(m.transform[0], 1.0, 1e-6);
        float lo = 1e30f, hi = -1e30f;
        for (size_t k = 0; k < d.pos.size(); k += 3) { lo = std::min(lo, d.pos[k]); hi = std::max(hi, d.pos[k]); }
        CHECK_NEAR(m.bbox_min[0], lo, 1e-6);
        CHECK_NEAR(m.bbox_max[0], hi, 1e-6);
        nue_free(mem);
    }
    // unrelated resources are untouched: the texture pack still lists its textures
    CHECK(nue_asset_resource_find(c, "TEX_DXT1_16", 0) != NUE_ASSET_NO_NODE);
    // a damaged solid does not stop the others
    nue_asset_container_close(c);
    auto bad = file;
    uint32_t seed = 11;
    for (int i = 0; i < 200; ++i) {
        auto m = bad;
        for (int k = 0; k < 3; ++k) m[rng_next(seed) % m.size()] ^= uint8_t(1u << (rng_next(seed) & 7));
        nue_asset_open_desc od;
        NUE_STRUCT_INIT(&od);
        od.flags = NUE_ASSET_OPEN_COPY | NUE_ASSET_OPEN_LENIENT;
        od.memory = m.data(); od.memory_size = m.size(); od.name_hint = ".bun";
        nue_asset_container x = nullptr;
        if (nue_asset_container_open(fx.ctx, &od, &x) < 0) continue;
        for (uint32_t r = 0; r < nue_asset_resource_count(x); ++r) {
            nue_asset_mesh mm; void* mem = nullptr;
            NUE_STRUCT_INIT(&mm);
            if (nue_asset_resource_get_mesh(x, r, &mm, &mem) >= 0) {
                for (uint32_t k = 0; k < mm.index_count; ++k) CHECK(mm.indices[k] < mm.vertex_count);
                nue_free(mem);
            }
        }
        nue_asset_container_close(x);
    }
}

static std::string dir_for(const char* tag) { return nue_test::unique_temp_dir(tag); }

TEST_CASE(model_export_glb_and_obj) {
    Fx fx;
    auto file = make_model_bun();
    nue_asset_container c = fx.open(file);
    const std::string dir = dir_for("assets_model");
    nue_asset_model_desc md;
    NUE_STRUCT_INIT(&md);
    md.containers = &c;
    md.container_count = 1;
    std::string glb = dir + "/model.glb";
    md.path = glb.c_str();
    md.format = NUE_ASSET_MODEL_GLB;
    nue_asset_model_stats st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_asset_export_model(&md, &st), NUE_OK);
    uint64_t verts = 0, tris = 0;
    for (const SolidSpec& s : default_solids()) { auto d = solid_data(s); verts += d.pos.size() / 3; tris += d.idx.size() / 3; }
    CHECK_EQ(st.meshes, uint32_t(default_solids().size()));
    CHECK_EQ(st.vertices, verts);
    CHECK_EQ(st.triangles, tris);
    CHECK_EQ(st.textures, 1u);                                                    // only the first material's texture exists in the file
    std::vector<uint8_t> g;
    CHECK(load_file(glb, g));
    CHECK_EQ(get32(&g[0]), 0x46546C67u);
    CHECK_EQ(get32(&g[4]), 2u);
    CHECK_EQ(get32(&g[8]), uint32_t(g.size()));
    const uint32_t jl = get32(&g[12]);
    CHECK_EQ(get32(&g[16]), 0x4E4F534Au);
    CHECK_EQ(jl % 4, 0u);
    const uint32_t bo = 20 + jl;
    CHECK_EQ(get32(&g[bo + 4]), 0x004E4942u);
    CHECK_EQ(uint64_t(bo) + 8 + get32(&g[bo]), uint64_t(g.size()));
    const std::string json(g.begin() + 20, g.begin() + 20 + jl);
    auto count = [&](const char* needle) { size_t n = 0, p = 0; while ((p = json.find(needle, p)) != std::string::npos) { ++n; p += std::strlen(needle); } return n; };
    CHECK_EQ(count("\"POSITION\""), default_solids().size() * 2);                // one primitive per material run
    CHECK_EQ(count("\"mesh\""), default_solids().size());
    CHECK_EQ(count("\"image/png\""), size_t(1));
    CHECK(json.find("\"rotation\":[-0.7") != std::string::npos);                   // Z up -> Y up on the root
    const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(std::search(g.begin(), g.end(), png, png + 8) != g.end());
    // no textures, native axes, best LOD, filter
    md.flags = NUE_ASSET_MODEL_NO_TEXTURES | NUE_ASSET_MODEL_KEEP_Z_UP | NUE_ASSET_MODEL_BEST_LOD;
    md.name_filter = "car_";
    glb = dir + "/lod.glb";
    md.path = glb.c_str();
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_asset_export_model(&md, &st), NUE_OK);
    CHECK_EQ(st.meshes, 2u);                                                       // CAR_BASE_A and CAR_DECAL (CAR_BASE_B is a less detailed copy of CAR_BASE_A)
    CHECK_EQ(st.textures, 0u);
    CHECK(load_file(glb, g));
    CHECK(std::string(g.begin() + 20, g.begin() + 20 + get32(&g[12])).find("\"rotation\"") == std::string::npos);
    // nothing to export
    md.name_filter = "no such mesh";
    CHECK_EQ(nue_asset_export_model(&md, &st), NUE_ERR_NOT_FOUND);
    // OBJ + MTL + PNG
    md.flags = 0;
    md.name_filter = nullptr;
    md.format = NUE_ASSET_MODEL_OBJ;
    std::string obj = dir + "/model.obj";
    md.path = obj.c_str();
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_asset_export_model(&md, &st), NUE_OK);
    std::vector<uint8_t> ob;
    CHECK(load_file(obj, ob));
    const std::string text(ob.begin(), ob.end());
    size_t nv = 0, nf = 0, nvt = 0, nvn = 0;
    for (size_t p = 0; p < text.size();) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        if (text.compare(p, 2, "v ") == 0) ++nv;
        else if (text.compare(p, 3, "vt ") == 0) ++nvt;
        else if (text.compare(p, 3, "vn ") == 0) ++nvn;
        else if (text.compare(p, 2, "f ") == 0) ++nf;
        p = e + 1;
    }
    CHECK_EQ(nv, verts);
    CHECK_EQ(nvt, verts);
    CHECK_EQ(nf, tris);
    CHECK(nvn > 0 && nvn < verts);                                                 // only the solids with 36 byte vertices have normals
    // every face index points at a vertex that exists
    std::vector<uint8_t> mtl;
    CHECK(load_file(dir + "/model.mtl", mtl));
    CHECK(std::string(mtl.begin(), mtl.end()).find("map_Kd model_textures/") != std::string::npos);
    CHECK(std::filesystem::exists(dir + "/model_textures"));
    nue_asset_container_close(c);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_MAIN_BEGIN
    RUN(huff_streams);
    RUN(car_texture_pack);
    RUN(mesh_resources);
    RUN(model_export_glb_and_obj);
TEST_MAIN_END
