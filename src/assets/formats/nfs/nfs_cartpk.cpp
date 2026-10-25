// Car texture packs (TEXTURES.BIN, VINYLS.BIN): a texture pack whose textures are separate JDLZ or HUFF compressed blobs, listed by 24 byte descriptors.
// Layout as observed, docs/ASSET_FORMATS.md section 4.6. Read only: the writer does not rebuild this layout.
#include "nfs.hpp"

#include <algorithm>
#include <cstring>

namespace nfs {

namespace {

constexpr uint32_t kIdCarDescriptors = 0x33310003u;
constexpr uint32_t kDescriptorSize = 24;
constexpr uint32_t kRecordInBlob = 0x64;                // the 0x7C byte texture record sits at (decoded size - header distance + 0x64)
constexpr uint64_t kMaxBlobOutput = 512ull << 20;

struct CarEntry { uint32_t hash, abs_offset, size, out_size, from_end; };

uint32_t child(nue_asset_container c, uint32_t parent, uint32_t id) {
    nue_asset_node_info n;
    NUE_STRUCT_INIT(&n);
    if (nue_asset_node_get(c, parent, &n) < 0) return NUE_ASSET_NO_NODE;
    for (uint32_t i = n.first_child; i != NUE_ASSET_NO_NODE;) {
        nue_asset_node_info k;
        NUE_STRUCT_INIT(&k);
        nue_asset_node_get(c, i, &k);
        if (k.id == id) return i;
        i = k.next_sibling;
    }
    return NUE_ASSET_NO_NODE;
}

struct CarPack {
    uint32_t header_node = NUE_ASSET_NO_NODE, data_node = NUE_ASSET_NO_NODE;
    uint64_t data_offset = 0, data_size = 0;
    std::vector<CarEntry> entries;
};

bool load_car_pack(nue_asset_container c, uint32_t tpk, CarPack* out) {
    const uint32_t info = child(c, tpk, kIdTpkInfo), dgrp = child(c, tpk, kIdTpkDataGroup);
    if (info == NUE_ASSET_NO_NODE || dgrp == NUE_ASSET_NO_NODE) return false;
    const uint32_t desc = child(c, info, kIdCarDescriptors);
    out->header_node = child(c, info, kIdTpkHeader);
    out->data_node = child(c, dgrp, kIdTpkData);
    if (desc == NUE_ASSET_NO_NODE || out->data_node == NUE_ASSET_NO_NODE) return false;
    nue_asset_node_info dn, ds;
    NUE_STRUCT_INIT(&dn);
    NUE_STRUCT_INIT(&ds);
    nue_asset_node_get(c, out->data_node, &dn);
    nue_asset_node_get(c, desc, &ds);
    if (ds.size == 0 || ds.size % kDescriptorSize != 0 || ds.size > (1ull << 28)) return false;
    out->data_offset = dn.offset;
    out->data_size = dn.size;
    std::vector<uint8_t> d(size_t(ds.size));
    size_t got = 0;
    if (nue_asset_node_read(c, desc, 0, d.data(), d.size(), &got) < 0 || got != d.size()) return false;
    out->entries.resize(d.size() / kDescriptorSize);
    for (size_t i = 0; i < out->entries.size(); ++i) {
        const uint8_t* p = d.data() + i * kDescriptorSize;
        out->entries[i] = {rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12), rd32(p + 16)};
    }
    return true;
}

// blob -> decoded pool + the texture record inside it
nue_result decode_entry(nue_asset_container c, const CarPack& p, const CarEntry& e, TexRec* x, std::vector<uint8_t>* pool) {
    if (e.abs_offset < p.data_offset || uint64_t(e.abs_offset) - p.data_offset + e.size > p.data_size || e.size < 16) return NUE_ERR_CORRUPT;
    std::vector<uint8_t> blob(e.size);
    size_t got = 0;
    nue_result r = nue_asset_node_read(c, p.data_node, uint64_t(e.abs_offset) - p.data_offset, blob.data(), blob.size(), &got);
    if (r < 0) return r;
    if (got != blob.size()) return NUE_ERR_CORRUPT;
    uint32_t unc = 0, stream = 0;
    const bool jdlz = nue_jdlz_identify(blob.data(), blob.size(), &unc, &stream) == NUE_OK;
    if (!jdlz && nue_huff_identify(blob.data(), blob.size(), &unc, &stream) != NUE_OK) return NUE_ERR_UNSUPPORTED;     // neither codec: raw blobs are not known
    if (unc > kMaxBlobOutput || unc < e.out_size) return NUE_ERR_CORRUPT;
    pool->assign(unc, 0);
    size_t written = 0;
    r = jdlz ? nue_jdlz_decompress(blob.data(), blob.size(), pool->data(), pool->size(), &written) : nue_huff_decompress(blob.data(), blob.size(), pool->data(), pool->size(), &written);
    if (r < 0) return r;
    if (e.from_end > e.out_size) return NUE_ERR_CORRUPT;
    const uint64_t rec = uint64_t(e.out_size) - e.from_end + kRecordInBlob;
    if (rec + kRecSize > pool->size()) return NUE_ERR_CORRUPT;
    parse_texrec(pool->data() + rec, x);
    if (x->hash != e.hash) return NUE_ERR_CORRUPT;                       // the record's own hash must name the descriptor's texture: the self check
    if (!ct_known(x->ct) || x->width == 0 || x->height == 0 || x->mips == 0 || x->mips > 16 || chain_size(x->ct, x->width, x->height, x->mips) != x->image_size ||
        x->image_size > pool->size()) return NUE_ERR_CORRUPT;
    if (ct_paletted(x->ct)) {
        const uint32_t off = x->pal_off - x->placement;                  // the palette's place inside this blob
        if (x->pal_size != 1024 || uint64_t(off) + 1024 > pool->size()) return NUE_ERR_CORRUPT;
    }
    return NUE_OK;
}

}  // namespace

nue_result car_load_texture(nue_asset_container c, uint32_t tpk_node, uint32_t sub_index, TexRec* x, std::vector<uint8_t>* image, std::vector<uint8_t>* palette) {
    CarPack p;
    if (!load_car_pack(c, tpk_node, &p) || sub_index >= p.entries.size()) return NUE_ERR_CORRUPT;
    std::vector<uint8_t> pool;
    nue_result r = decode_entry(c, p, p.entries[sub_index], x, &pool);
    if (r < 0) return r;
    image->assign(pool.begin(), pool.begin() + std::ptrdiff_t(x->image_size));
    palette->clear();
    if (ct_paletted(x->ct)) {
        const uint32_t off = x->pal_off - x->placement;
        palette->assign(pool.begin() + off, pool.begin() + off + 1024);
    }
    return NUE_OK;
}

nue_result index_car_pack(nue_asset_container c, uint32_t tpk_node, uint32_t pack_resource, const nue_asset_resource_ops* ops, void* builder) {
    CarPack p;
    if (!load_car_pack(c, tpk_node, &p)) return NUE_OK;
    std::vector<uint8_t> pool;
    for (size_t k = 0; k < p.entries.size(); ++k) {
        TexRec x;
        if (decode_entry(c, p, p.entries[k], &x, &pool) < 0) continue;             // a texture whose blob or record does not check out is left out of the list
        nue_asset_resource_info tr;
        NUE_STRUCT_INIT(&tr);
        tr.kind = NUE_ASSET_RES_TEXTURE;
        tr.node = tpk_node;
        tr.parent = pack_resource;
        tr.sub_index = uint32_t(k);
        tr.derived_node = NUE_ASSET_NO_NODE;
        tr.hash = x.hash;
        tr.name = x.name;
        tr.format = ct_paletted(x.ct) ? NUE_TEX_BGRA8_UNORM : format_of_ct(x.ct);
        tr.width = x.width;
        tr.height = x.height;
        tr.depth = 1;
        tr.mip_levels = x.mips;
        tr.native_format = x.ct;
        tr.flags = NUE_ASSET_RESFLAG_READONLY | (ct_paletted(x.ct) ? NUE_ASSET_RESFLAG_PALETTED : 0);
        tr.data_size = uint64_t(x.image_size) + x.pal_size;
        nue_result r = ops->add_resource(builder, &tr, nullptr);
        if (r < 0) return r;
    }
    return NUE_OK;
}

}  // namespace nfs
