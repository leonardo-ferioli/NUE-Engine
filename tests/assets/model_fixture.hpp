// Synthetic solids (meshes), car texture packs and a tiny HUFF encoder for the tests (no game data).
#pragma once
#include "nfs_fixture.hpp"

#include <algorithm>
#include <cmath>

namespace asstest {

// ------------------------------------------------------------------------------------------------ HUFF (just enough of an encoder to make real streams)
struct BitWriter {
    std::vector<uint8_t> bytes;
    int used = 0;
    void bit(int b) { if (used == 0) bytes.push_back(0); if (b) bytes.back() |= uint8_t(0x80 >> used); used = (used + 1) & 7; }
    void bits(uint64_t v, int n) { for (int i = n - 1; i >= 0; --i) bit(int((v >> i) & 1)); }
    void num(uint32_t v) {                                  // the format's variable length number
        const uint64_t t = uint64_t(v) + 4;
        int n = 0;
        while ((t >> (n + 1)) != 0) ++n;                    // n = floor(log2(v + 4)) >= 2
        const uint64_t raw = t - (1ull << n);
        for (int i = 0; i < n - 2; ++i) bit(0);
        bit(1);
        bits(raw, n);
    }
};

// Fixed length codes: the alphabet is the distinct bytes of `data` plus the clue, padded with unused values to a power of two (2^L symbols of L bits each).
// Runs use the clue escape.
inline std::vector<uint8_t> huff_encode(const std::vector<uint8_t>& data, bool use_runs = true) {
    std::vector<uint8_t> symbols;
    for (uint8_t b : data) if (std::find(symbols.begin(), symbols.end(), b) == symbols.end()) symbols.push_back(b);
    uint8_t clue = 0xFE;
    while (std::find(symbols.begin(), symbols.end(), clue) != symbols.end()) --clue;
    symbols.push_back(clue);
    int L = 1;
    while ((1u << L) < symbols.size()) ++L;
    const size_t total = size_t(1) << L;
    for (uint32_t pad = 0; symbols.size() < total; ++pad) if (std::find(symbols.begin(), symbols.end(), uint8_t(pad)) == symbols.end()) symbols.push_back(uint8_t(pad));
    std::sort(symbols.begin(), symbols.end());
    BitWriter w;
    w.bits(0x30FB, 16);
    w.bits(uint32_t(data.size()) >> 16, 8);
    w.bits(uint32_t(data.size()) & 0xFFFF, 16);
    w.bits(clue, 8);
    for (int len = 1; len < L; ++len) w.num(0);             // no codes shorter than L bits
    w.num(uint32_t(total));                                 // all codes have L bits
    int prev = -1;
    for (uint8_t s : symbols) { w.num(uint32_t(int(s) - prev - 1)); prev = s; }
    auto code = [&](uint8_t s) { return int(std::find(symbols.begin(), symbols.end(), s) - symbols.begin()); };
    for (size_t i = 0; i < data.size();) {
        w.bits(uint64_t(code(data[i])), L);
        size_t j = i + 1;
        if (use_runs) {
            while (j < data.size() && data[j] == data[i]) ++j;
            if (j - i - 1 >= 2) { w.bits(uint64_t(code(clue)), L); w.num(uint32_t(j - i - 1)); }
            else j = i + 1;
        }
        i = j;
    }
    w.bits(uint64_t(code(clue)), L); w.num(0); w.bit(1);    // end of stream
    std::vector<uint8_t> out = {'H', 'U', 'F', 'F', 1, 0x10, 0, 0};
    put32(out, uint32_t(data.size()));
    put32(out, uint32_t(w.bytes.size()));
    out.insert(out.end(), w.bytes.begin(), w.bytes.end());
    return out;
}

// ------------------------------------------------------------------------------------------------ solids
struct SolidSpec {
    const char* name;
    uint32_t grid;                  // grid x grid vertices, 2 triangles per cell
    bool normals;                   // 36 byte vertices (flag 0x80) or 24 byte ones
    uint32_t vertex_filler;         // 0x11 bytes in front of the vertices
    uint32_t mesh_header_filler;    // 0x11 words in front of the mesh header
    float offset;                   // moves the grid along x
    uint32_t header_filler = 0;     // 0x11 words in front of the solid header (shifts its fields)
};

struct SolidData {
    std::vector<float> pos, nrm, uv;
    std::vector<uint8_t> rgba;
    std::vector<uint32_t> idx;
    uint32_t run0_count = 0;        // indices of the first material run
};

inline SolidData solid_data(const SolidSpec& s) {
    SolidData d;
    const uint32_t n = s.grid;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            d.pos.push_back(float(x) * 0.5f + s.offset); d.pos.push_back(float(y) * 0.25f); d.pos.push_back(float((x * 7 + y * 3) % 5) * 0.125f);
            d.nrm.push_back(0); d.nrm.push_back(0); d.nrm.push_back(1);
            d.uv.push_back(float(x) / float(n - 1)); d.uv.push_back(float(y) / float(n - 1));
            d.rgba.push_back(uint8_t(x * 20)); d.rgba.push_back(uint8_t(y * 20)); d.rgba.push_back(uint8_t(x * y)); d.rgba.push_back(255);
        }
    for (uint32_t y = 0; y + 1 < n; ++y)
        for (uint32_t x = 0; x + 1 < n; ++x) {
            const uint32_t a = y * n + x, b = a + 1, c = a + n, e = c + 1;
            d.idx.insert(d.idx.end(), {a, b, e, a, e, c});
        }
    d.run0_count = uint32_t(d.idx.size() / 6) * 3;           // first half of the triangles (a multiple of 3)
    return d;
}

inline void put_f32(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, u); }

inline void add_solid(Bun& bun, const SolidSpec& s, uint64_t mat0, uint64_t mat1) {
    const SolidData d = solid_data(s);
    const uint32_t vc = uint32_t(d.pos.size() / 3), ic = uint32_t(d.idx.size());
    size_t g = bun.begin(0x80134010u);
    const size_t fo = size_t(s.header_filler) * 4;
    std::vector<uint8_t> hdr(192 + fo, 0);
    std::fill(hdr.begin(), hdr.begin() + std::ptrdiff_t(fo), uint8_t(0x11));
    auto setf = [&](size_t off, float f) { uint32_t u; std::memcpy(&u, &f, 4); for (int i = 0; i < 4; ++i) hdr[fo + off + i] = uint8_t(u >> (8 * i)); };
    for (int i = 0; i < 4; ++i) setf(0x40 + size_t(i) * 20, 1.0f);                  // identity
    setf(0x70, s.offset); setf(0x74, 2.0f); setf(0x78, 3.0f);                        // translation in the last row (row vectors)
    for (size_t i = 0; s.name[i]; ++i) hdr[fo + 0xA4 + i] = uint8_t(s.name[i]);
    bun.chunk(0x00134011u, hdr);
    std::vector<uint8_t> mats, shd;
    put32(mats, uint32_t(mat0)); put32(mats, 0); put32(mats, uint32_t(mat1)); put32(mats, 0);
    put32(shd, 0x11111111u); put32(shd, 0); put32(shd, 0x22222222u); put32(shd, 0);
    bun.chunk(0x00134012u, mats);
    bun.chunk(0x00134013u, shd);
    size_t grp = bun.begin(0x80134100u);
    std::vector<uint8_t> mh(s.mesh_header_filler * 4, 0x11);
    std::vector<uint32_t> f(17, 0);
    f[2] = 0x10; f[3] = s.normals ? 0xC180u : 0u; f[9] = ic / 3; f[13] = vc;
    for (uint32_t x : f) put32(mh, x);
    bun.chunk(0x00134900u, mh);
    std::vector<uint8_t> vb(s.vertex_filler, 0x11);
    for (uint32_t i = 0; i < vc; ++i) {
        for (int a = 0; a < 3; ++a) put_f32(vb, d.pos[i * 3 + a]);
        if (s.normals) for (int a = 0; a < 3; ++a) put_f32(vb, d.nrm[i * 3 + a]);
        vb.push_back(d.rgba[i * 4 + 2]); vb.push_back(d.rgba[i * 4 + 1]); vb.push_back(d.rgba[i * 4 + 0]); vb.push_back(d.rgba[i * 4 + 3]);   // stored B,G,R,A
        put_f32(vb, d.uv[i * 2]); put_f32(vb, d.uv[i * 2 + 1]);
    }
    bun.chunk(0x00134B01u, vb);
    std::vector<uint8_t> rg(12, 0x11);
    auto entry = [&](uint32_t count, uint32_t matidx, uint32_t shdidx, uint32_t offset) {
        std::vector<uint32_t> e(15, 0);
        e[3] = count; e[7] = matidx; e[8] = shdidx; e[13] = offset;
        for (uint32_t x : e) put32(rg, x);
    };
    entry(d.run0_count, 0, 0, 0);
    entry(ic - d.run0_count, 1, 1, d.run0_count);
    bun.chunk(0x00134B02u, rg);
    std::vector<uint8_t> ib(4, 0x11);
    for (uint32_t i : d.idx) { ib.push_back(uint8_t(i)); ib.push_back(uint8_t(i >> 8)); }
    ib.push_back(0); ib.push_back(0);
    bun.chunk(0x00134B03u, ib);
    bun.end(grp);
    bun.end(g);
}

inline std::vector<SolidSpec> default_solids() {
    return {
        {"CAR_BASE_A", 8, true, 12, 0, 0.0f},
        {"CAR_BASE_B", 3, true, 4, 2, 10.0f},       // two filler words before the mesh header
        {"WORLD_WALL", 6, false, 8, 0, 20.0f},
        {"WORLD_TINY", 2, false, 100, 0, 30.0f},    // 24 byte vertices behind so much filler that 36 byte ones would fit too: the header flag decides
        {"CAR_DECAL", 4, true, 0, 0, 40.0f, 3},      // three filler words in front of the solid header
    };
}

// A BUN with solids and a texture pack whose textures the solids' first material run points at (hash 0xAAAA0001 = TEX_DXT1_16).
inline std::vector<uint8_t> make_model_bun() {
    Bun bun;
    size_t sec = bun.begin(0x80134000u);
    bun.chunk(0x80134001u, std::vector<uint8_t>());
    for (const SolidSpec& s : default_solids()) add_solid(bun, s, 0xAAAA0001u, 0x12345678u);
    bun.chunk(0x80134008u, std::vector<uint8_t>());
    bun.end(sec);
    bun.pad(128);
    add_tpk(bun, "MODELTEX", 0x1234u, default_textures());
    return bun.b;
}

// ------------------------------------------------------------------------------------------------ car texture pack (per texture compressed blobs)
struct CarTex {
    const char* name; uint32_t hash; uint8_t ct; uint32_t w, h, mips; uint32_t seed;
    int codec;                       // 0 JDLZ, 1 HUFF
};

inline std::vector<uint8_t> car_blob(const CarTex& t, std::vector<uint8_t>* pixels, std::vector<uint8_t>* palette) {
    TexSpec s{t.name, t.hash, t.ct, t.w, t.h, t.mips, t.seed};
    const uint64_t image = chain_bytes(s);
    // few distinct values so that the 4 symbol HUFF stream can carry it
    std::vector<uint8_t> px;
    px.resize(size_t(image));
    uint32_t r = t.seed;
    for (auto& b : px) b = uint8_t("\x00\x40\x80\xC0"[rng_next(r) & 3]);
    std::vector<uint8_t> pal;
    if (t.ct == 0x08) for (int i = 0; i < 1024; ++i) pal.push_back(uint8_t((i * 3) & 0xC0));
    if (pixels) *pixels = px;
    if (palette) *palette = pal;
    // decoded layout: pixels at 0, the palette behind them, then padding up to the record, which sits at (size - 0x100 + 0x64)
    std::vector<uint8_t> pool = px;
    const uint32_t pal_at = uint32_t(pool.size());
    pool.insert(pool.end(), pal.begin(), pal.end());
    pool.resize(pool.size() + 0x100, 0);                     // room for the record, which sits at (decoded size - 0x100 + 0x64)
    const uint32_t final_size = uint32_t(pool.size());
    const size_t rec2 = size_t(final_size) - 0x100 + 0x64;
    std::vector<uint8_t> r7(0x7C, 0);
    auto w32 = [&](size_t o, uint32_t v) { for (int k = 0; k < 4; ++k) r7[o + k] = uint8_t(v >> (8 * k)); };
    for (size_t i = 0; t.name[i] && i < 23; ++i) r7[0x0C + i] = uint8_t(t.name[i]);
    w32(0x24, t.hash);
    w32(0x30, 0x1000);                                       // image placement (any value: only the palette distance matters)
    w32(0x34, pal.empty() ? 0u : 0x1000 + pal_at);
    w32(0x38, uint32_t(px.size()));
    w32(0x3C, uint32_t(pal.size()));
    w32(0x40, uint32_t(level_bytes(t.ct, t.w, t.h)));
    r7[0x44] = uint8_t(t.w); r7[0x45] = uint8_t(t.w >> 8); r7[0x46] = uint8_t(t.h); r7[0x47] = uint8_t(t.h >> 8);
    r7[0x4A] = t.ct; r7[0x4E] = uint8_t(t.mips);
    std::copy(r7.begin(), r7.end(), pool.begin() + std::ptrdiff_t(rec2));
    std::vector<uint8_t> blob;
    if (t.codec == 0) {
        blob.resize(nue_jdlz_compress_bound(pool.size()));
        size_t n = 0;
        nue_jdlz_compress(pool.data(), pool.size(), 6, blob.data(), blob.size(), &n);
        blob.resize(n);
    } else {
        blob = huff_encode(pool);
    }
    return blob;
}

// A BUN holding a car style pack: descriptors (hash, absolute offset, size, decoded size, 0x100, 0) and the blobs one after the other.
inline std::vector<uint8_t> make_car_pack(const std::vector<CarTex>& tex, std::vector<std::vector<uint8_t>>* pixels, std::vector<std::vector<uint8_t>>* palettes) {
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<uint32_t> outs;
    for (const CarTex& t : tex) {
        std::vector<uint8_t> px, pal;
        blobs.push_back(car_blob(t, &px, &pal));
        if (pixels) pixels->push_back(px);
        if (palettes) palettes->push_back(pal);
        // decoded size = unc size from the blob header
        outs.push_back(asstest::get32(&blobs.back()[8]));
    }
    Bun bun;
    size_t tpk = bun.begin(0xB3300000u);
    bun.pad(64);
    size_t info = bun.begin(0xB3310000u);
    std::vector<uint8_t> hdr(0x7C, 0);
    hdr[0] = 5;
    const char* nm = "CARTEXTURES";
    for (size_t i = 0; nm[i]; ++i) hdr[4 + i] = uint8_t(nm[i]);
    bun.chunk(0x33310001u, hdr);
    std::vector<uint8_t> hashes, desc;
    // first pass to know where the data chunk will be: header sizes are fixed, so build the descriptors after laying out
    for (const CarTex& t : tex) { put32(hashes, t.hash); put32(hashes, 0); }
    bun.chunk(0x33310002u, hashes);
    const size_t desc_pos = bun.b.size();
    bun.chunk(0x33310003u, std::vector<uint8_t>(tex.size() * 24, 0));          // patched below
    bun.end(info);
    bun.pad(128);
    size_t dgrp = bun.begin(0xB3320000u);
    bun.chunk(0x33320001u, std::vector<uint8_t>(24, 0));
    bun.pad(128);
    size_t dc = bun.begin(0x33320002u);
    std::vector<uint32_t> abs(tex.size());
    for (size_t i = 0; i < tex.size(); ++i) {
        while (bun.b.size() % 16) bun.b.push_back(0);
        abs[i] = uint32_t(bun.b.size());
        bun.b.insert(bun.b.end(), blobs[i].begin(), blobs[i].end());
    }
    bun.end(dc);
    bun.end(dgrp);
    bun.end(tpk);
    for (size_t i = 0; i < tex.size(); ++i) {
        uint8_t* p = &bun.b[desc_pos + 8 + i * 24];
        auto w = [&](int o, uint32_t v) { for (int k = 0; k < 4; ++k) p[o + k] = uint8_t(v >> (8 * k)); };
        w(0, tex[i].hash); w(4, abs[i]); w(8, uint32_t(blobs[i].size())); w(12, outs[i]); w(16, 0x100); w(20, 0);
    }
    return bun.b;
}

}  // namespace asstest
