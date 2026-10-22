// NFS texture packs ("TPK", chunk 0xB3300000): the texture table, the pixel data layout, reading textures as images and rebuilding the pack when
// textures change. Layout as observed on the files, see docs/ASSET_FORMATS.md section "Texture pack".
#include "nfs.hpp"

#include <algorithm>
#include <cstring>

namespace nfs {

using nuea::align_up;

// ---------------------------------------------------------------------------------------------------- pixel formats
bool ct_paletted(uint8_t ct) { return ct == kCtP8 || ct == kCtP8_16 || ct == kCtP8_64; }
bool ct_known(uint8_t ct) { return ct == kCtBgra8 || ct == kCtDxt1 || ct == kCtDxt3 || ct == kCtDxt5 || ct_paletted(ct); }

nue_tex_format format_of_ct(uint8_t ct) {
    switch (ct) {
        case kCtBgra8: return NUE_TEX_BGRA8_UNORM;
        case kCtDxt1: return NUE_TEX_BC1_UNORM;
        case kCtDxt3: return NUE_TEX_BC2_UNORM;
        case kCtDxt5: return NUE_TEX_BC3_UNORM;
        default: return ct_paletted(ct) ? NUE_TEX_R8_UNORM : NUE_TEX_FORMAT_UNKNOWN;
    }
}

static bool ct_for_format(nue_tex_format f, uint8_t* ct, uint32_t* d3d_code) {
    switch (f) {
        case NUE_TEX_BGRA8_UNORM: *ct = kCtBgra8; *d3d_code = 21; return true;
        case NUE_TEX_BC1_UNORM: *ct = kCtDxt1; *d3d_code = 0x31545844u; return true;
        case NUE_TEX_BC2_UNORM: *ct = kCtDxt3; *d3d_code = 0x33545844u; return true;
        case NUE_TEX_BC3_UNORM: *ct = kCtDxt5; *d3d_code = 0x35545844u; return true;
        default: return false;
    }
}

uint64_t chain_size(uint8_t ct, uint32_t w, uint32_t h, uint32_t mips) {
    const nue_tex_format f = format_of_ct(ct);
    uint64_t total = 0;
    for (uint32_t l = 0; l < mips; ++l) total += nue_tex_level_size(f, nue_tex_mip_extent(w, l), nue_tex_mip_extent(h, l), 1);
    return total;
}

// ---------------------------------------------------------------------------------------------------- locating and parsing
namespace {

uint32_t find_child(nue_asset_container c, uint32_t parent, uint32_t id) {
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

bool parse_rec(const uint8_t* p, TexRec* t) {
    std::memcpy(t->raw, p, kRecSize);
    std::memcpy(t->name, p + 0x0C, 24);
    t->name[24] = 0;
    t->hash = rd32(p + 0x24);
    t->cls = rd32(p + 0x28);
    t->placement = rd32(p + 0x30);
    t->pal_off = rd32(p + 0x34);
    t->image_size = rd32(p + 0x38);
    t->pal_size = rd32(p + 0x3C);
    t->top_size = rd32(p + 0x40);
    t->width = rd16(p + 0x44);
    t->height = rd16(p + 0x46);
    t->ct = p[0x4A];
    t->mips = p[0x4E];
    return true;
}

bool valid_rec(const TexRec& t, uint64_t data_size) {
    if (!ct_known(t.ct) || t.width == 0 || t.height == 0 || t.mips == 0 || t.mips > 16) return false;
    if (chain_size(t.ct, t.width, t.height, t.mips) != t.image_size) return false;
    if (t.image_size == 0 || uint64_t(t.placement) + t.image_size > data_size) return false;
    if (ct_paletted(t.ct) && t.pal_size != 1024) return false;
    if (!ct_paletted(t.ct) && t.pal_size != 0) return false;
    if (t.pal_size && uint64_t(t.pal_off) + t.pal_size > data_size) return false;
    return true;
}

}  // namespace

bool load_tpk(nue_asset_container c, uint32_t node, Tpk* out) {
    Tpk t;
    t.node = node;
    const uint32_t info = find_child(c, node, kIdTpkInfo);
    const uint32_t dgrp = find_child(c, node, kIdTpkDataGroup);
    if (info == NUE_ASSET_NO_NODE || dgrp == NUE_ASSET_NO_NODE) return false;
    t.header_node = find_child(c, info, kIdTpkHeader);
    t.hash_node = find_child(c, info, kIdTpkHashes);
    t.rec_node = find_child(c, info, kIdTpkRecords);
    t.fmt_node = find_child(c, info, kIdTpkFormats);
    t.data_group = dgrp;
    t.data_node = find_child(c, dgrp, kIdTpkData);
    if (t.rec_node == NUE_ASSET_NO_NODE || t.data_node == NUE_ASSET_NO_NODE) return false;

    nue_asset_node_info rn, dn;
    NUE_STRUCT_INIT(&rn);
    NUE_STRUCT_INIT(&dn);
    nue_asset_node_get(c, t.rec_node, &rn);
    nue_asset_node_get(c, t.data_node, &dn);
    if (rn.size == 0 || rn.size % kRecSize != 0) return false;
    const uint64_t n = rn.size / kRecSize;
    if (n > 0xFFFFFFu) return false;
    if (t.hash_node != NUE_ASSET_NO_NODE) {
        nue_asset_node_info hn;
        NUE_STRUCT_INIT(&hn);
        nue_asset_node_get(c, t.hash_node, &hn);
        if (hn.size != n * 8) return false;
    }
    if (t.fmt_node != NUE_ASSET_NO_NODE) {
        nue_asset_node_info fn;
        NUE_STRUCT_INIT(&fn);
        nue_asset_node_get(c, t.fmt_node, &fn);
        if (fn.size != n * kFmtSize) return false;
    }
    t.data_size = dn.size;
    t.base_rel = align_up(dn.offset, kDataAlign) - dn.offset;
    std::vector<uint8_t> recs(size_t(rn.size));
    size_t got = 0;
    if (nue_asset_node_read(c, t.rec_node, 0, recs.data(), recs.size(), &got) < 0 || got != recs.size()) return false;
    t.tex.resize(size_t(n));
    for (size_t i = 0; i < n; ++i) {
        parse_rec(recs.data() + i * kRecSize, &t.tex[i]);
        if (!valid_rec(t.tex[i], t.data_size)) return false;
    }
    if (t.header_node != NUE_ASSET_NO_NODE) {
        nue_asset_node_info hn;
        NUE_STRUCT_INIT(&hn);
        nue_asset_node_get(c, t.header_node, &hn);
        uint8_t h[0x64] = {0};
        size_t g = 0;
        nue_asset_node_read(c, t.header_node, 0, h, std::min<uint64_t>(sizeof(h), hn.size), &g);
        char nm[29];
        std::memcpy(nm, h + 4, 28);
        nm[28] = 0;
        t.name = nm;
        if (g >= 0x64) t.hash = rd32(h + 0x60);
    }
    *out = std::move(t);
    return true;
}

// ---------------------------------------------------------------------------------------------------- resources
nue_result NUE_CALL index(void*, nue_asset_container c, void*, const nue_asset_resource_ops* ops, void* builder) {
    const uint32_t count = nue_asset_node_count(c);
    for (uint32_t i = 0; i < count; ++i) {
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        nue_asset_node_get(c, i, &n);
        if (n.id != kIdTpk || n.kind != NUE_ASSET_NODE_GROUP) continue;
        Tpk t;
        const bool ok = load_tpk(c, i, &t);
        nue_asset_resource_info r;
        NUE_STRUCT_INIT(&r);
        r.kind = NUE_ASSET_RES_TEXTURE_PACK;
        r.node = i;
        r.parent = NUE_ASSET_NO_NODE;
        r.derived_node = ok ? t.data_node : NUE_ASSET_NO_NODE;
        if (!ok) {                                                  // header may still name it
            const uint32_t info = find_child(c, i, kIdTpkInfo);
            const uint32_t hdr = info == NUE_ASSET_NO_NODE ? NUE_ASSET_NO_NODE : find_child(c, info, kIdTpkHeader);
            if (hdr != NUE_ASSET_NO_NODE) { uint8_t h[0x64] = {0}; size_t g = 0; nue_asset_node_read(c, hdr, 0, h, sizeof(h), &g); char nm[29]; std::memcpy(nm, h + 4, 28); nm[28] = 0; t.name = nm; if (g >= 0x64) t.hash = rd32(h + 0x60); }
        }
        r.name = t.name.c_str();
        r.hash = t.hash;
        r.data_size = n.size;
        uint32_t pack_index = 0;
        nue_result res = ops->add_resource(builder, &r, &pack_index);
        if (res < 0) return res;
        if (!ok) continue;
        for (size_t k = 0; k < t.tex.size(); ++k) {
            const TexRec& x = t.tex[k];
            nue_asset_resource_info tr;
            NUE_STRUCT_INIT(&tr);
            tr.kind = NUE_ASSET_RES_TEXTURE;
            tr.node = i;
            tr.parent = pack_index;
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
            tr.flags = ct_paletted(x.ct) ? NUE_ASSET_RESFLAG_PALETTED : 0;
            tr.data_size = x.image_size + x.pal_size;
            res = ops->add_resource(builder, &tr, nullptr);
            if (res < 0) return res;
        }
    }
    return NUE_OK;
}

nue_result NUE_CALL get_image(void*, nue_asset_container c, void*, uint32_t resource, nue_tex_image* out) {
    nue_asset_resource_info ri;
    NUE_STRUCT_INIT(&ri);
    nue_result r = nue_asset_resource_get(c, resource, &ri);
    if (r < 0) return r;
    Tpk t;
    if (!load_tpk(c, ri.node, &t) || ri.sub_index >= t.tex.size()) return NUE_ERR_CORRUPT;
    const TexRec& x = t.tex[ri.sub_index];

    std::vector<uint8_t> raw(size_t(x.image_size));
    size_t got = 0;
    r = nue_asset_node_read(c, t.data_node, t.base_rel + x.placement, raw.data(), raw.size(), &got);
    if (r < 0) return r;
    if (got != raw.size()) { nue_set_last_error("nfs: texture '%s' lies outside its data chunk", x.name); return NUE_ERR_CORRUPT; }
    std::vector<uint8_t> pal;
    if (ct_paletted(x.ct)) {
        pal.resize(x.pal_size);
        r = nue_asset_node_read(c, t.data_node, t.base_rel + x.pal_off, pal.data(), pal.size(), &got);
        if (r < 0) return r;
        if (got != pal.size()) return NUE_ERR_CORRUPT;
    }

    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    d.format = ct_paletted(x.ct) ? NUE_TEX_BGRA8_UNORM : format_of_ct(x.ct);
    d.width = x.width;
    d.height = x.height;
    d.mip_levels = x.mips;
    nue_tex_image im = nullptr;
    r = nue_tex_image_create(&d, &im);
    if (r < 0) return r;
    uint64_t src = 0;
    for (uint32_t l = 0; l < x.mips; ++l) {
        nue_tex_level_view v;
        NUE_STRUCT_INIT(&v);
        r = nue_tex_image_get_level(im, l, 0, 0, &v);
        if (r < 0) { nue_tex_image_destroy(im); return r; }
        const uint64_t idx_bytes = ct_paletted(x.ct) ? uint64_t(v.width) * v.height : v.size;
        if (src + idx_bytes > raw.size()) { nue_tex_image_destroy(im); return NUE_ERR_CORRUPT; }
        if (ct_paletted(x.ct)) {
            uint8_t* dst = static_cast<uint8_t*>(v.data);
            for (uint64_t k = 0; k < idx_bytes; ++k) std::memcpy(dst + k * 4, pal.data() + size_t(raw[size_t(src + k)]) * 4, 4);
        } else {
            std::memcpy(v.data, raw.data() + src, size_t(v.size));
        }
        src += idx_bytes;
    }
    *out = im;
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------- rebuilding
static uint32_t log2_floor(uint32_t v) { uint32_t n = 0; while (v > 1) { v >>= 1; ++n; } return n; }

nue_result plan_tpk(nue_asset_container c, const Tpk& t, TpkPlan* out) {
    TpkPlan p;
    p.tpk = t.node;
    p.rec_node = t.rec_node;
    p.fmt_node = t.fmt_node;
    p.data_node = t.data_node;
    p.src_base_rel = t.base_rel;

    // which resources are these textures?
    std::vector<nue_tex_image> edits(t.tex.size(), nullptr);
    for (uint32_t r = 0, n = nue_asset_resource_count(c); r < n; ++r) {
        nue_asset_resource_info ri;
        NUE_STRUCT_INIT(&ri);
        nue_asset_resource_get(c, r, &ri);
        if (ri.kind == NUE_ASSET_RES_TEXTURE && ri.node == t.node && ri.sub_index < edits.size()) edits[ri.sub_index] = nue_asset_resource_edit(c, r);
    }

    p.tex.resize(t.tex.size());
    std::vector<uint8_t> fmts;
    if (t.fmt_node != NUE_ASSET_NO_NODE) {
        fmts.resize(t.tex.size() * kFmtSize);
        size_t got = 0;
        nue_result r = nue_asset_node_read(c, t.fmt_node, 0, fmts.data(), fmts.size(), &got);
        if (r < 0 || got != fmts.size()) return NUE_ERR_CORRUPT;
    }

    for (size_t k = 0; k < t.tex.size(); ++k) {
        const TexRec& x = t.tex[k];
        TexPlan& q = p.tex[k];
        std::memcpy(q.rec, x.raw, kRecSize);
        q.src_off = t.base_rel + x.placement;
        q.image_size = x.image_size;
        q.pal_size = x.pal_size;
        q.pal_src = t.base_rel + x.pal_off;
        if (!edits[k]) continue;

        nue_tex_image_desc d;
        NUE_STRUCT_INIT(&d);
        nue_result r = nue_tex_image_get_desc(edits[k], &d);
        if (r < 0) return r;
        if ((d.layers > 1) || (d.faces > 1) || (d.depth > 1)) { nue_set_last_error("nfs: texture '%s': arrays, cube maps and volumes are not stored in NFS packs", x.name); return NUE_ERR_UNSUPPORTED; }
        if (d.width == 0 || d.height == 0 || d.width > 65535 || d.height > 65535) return NUE_ERR_INVALID_ARGUMENT;
        uint8_t ct = 0;
        uint32_t code = 0;
        nue_tex_image use = edits[k], converted = nullptr;
        if (!ct_for_format(d.format, &ct, &code)) {
            // keep the family the game stored: DXT1 stays BC1, DXT3/DXT5 become BC3 (the texture module has no BC2 encoder; DXT5 is the same size and
            // a D3D9 format every driver accepts), everything else becomes BGRA8
            const nue_tex_format target = x.ct == kCtDxt1 ? NUE_TEX_BC1_UNORM : (x.ct == kCtDxt3 || x.ct == kCtDxt5) ? NUE_TEX_BC3_UNORM : NUE_TEX_BGRA8_UNORM;
            nue_tex_encode_params ep;
            nue_tex_encode_params_default(&ep);
            nue_tex_mip_params mp;
            nue_tex_mip_params_default(&mp);
            r = nue_tex_image_convert(edits[k], target, 0, &ep, &mp, &converted);
            if (r < 0) { nue_set_last_error("nfs: texture '%s': cannot convert %s to %s (%s)", x.name, nue_tex_format_name(d.format), nue_tex_format_name(target), nue_result_name(r)); return r; }
            use = converted;
            ct_for_format(target, &ct, &code);
            r = nue_tex_image_get_desc(use, &d);
            if (r < 0) { nue_tex_image_destroy(converted); return r; }
        }
        if (d.mip_levels > 16) { if (converted) nue_tex_image_destroy(converted); return NUE_ERR_UNSUPPORTED; }
        q.edited = true;
        q.image.clear();
        q.image.reserve(size_t(nue_tex_image_total_size(use)));
        for (uint32_t l = 0; l < d.mip_levels; ++l) {
            nue_tex_level_view v;
            NUE_STRUCT_INIT(&v);
            r = nue_tex_image_get_level(use, l, 0, 0, &v);
            if (r < 0) { if (converted) nue_tex_image_destroy(converted); return r; }
            q.image.insert(q.image.end(), static_cast<const uint8_t*>(v.data), static_cast<const uint8_t*>(v.data) + v.size);
        }
        if (converted) nue_tex_image_destroy(converted);
        q.image_size = q.image.size();
        q.pal_size = 0;
        const uint64_t top = nue_tex_level_size(format_of_ct(ct), d.width, d.height, 1);
        wr32(q.rec + 0x38, uint32_t(q.image_size));
        wr32(q.rec + 0x3C, 0);
        wr32(q.rec + 0x40, uint32_t(top));
        wr16(q.rec + 0x44, uint16_t(d.width));
        wr16(q.rec + 0x46, uint16_t(d.height));
        q.rec[0x48] = uint8_t(log2_floor(d.width));
        q.rec[0x49] = uint8_t(log2_floor(d.height));
        q.rec[0x4A] = ct;
        q.rec[0x4B] = 0;
        q.rec[0x4E] = uint8_t(d.mip_levels);
        if (!fmts.empty() && ct != x.ct) wr32(fmts.data() + k * kFmtSize + 0x14, code);
    }

    // layout: palettes first (a pool, in table order), then the images, each starting on a 128 byte boundary
    uint64_t pool = 0;
    for (auto& q : p.tex) {
        q.pal_off = uint32_t(pool);
        pool += q.pal_size;
    }
    p.pool_total = pool;
    uint64_t pos = align_up(pool, kDataAlign);
    for (auto& q : p.tex) {
        q.placement = uint32_t(pos);
        pos = align_up(pos + q.image_size, kDataAlign);
        wr32(q.rec + 0x30, q.placement);
        wr32(q.rec + 0x34, q.pal_off);
    }
    p.rec_bytes.resize(p.tex.size() * kRecSize);
    for (size_t k = 0; k < p.tex.size(); ++k) std::memcpy(p.rec_bytes.data() + k * kRecSize, p.tex[k].rec, kRecSize);
    p.fmt_bytes = std::move(fmts);
    *out = std::move(p);
    return NUE_OK;
}

namespace {

nue_result copy_from_node(nue_asset_container c, nue_asset_sink* sink, uint32_t node, uint64_t off, uint64_t size) {
    std::vector<uint8_t> tmp;
    for (uint64_t done = 0; done < size;) {
        if (const void* ptr = nue_asset_node_peek(c, node, off + done, size_t(size - done))) return nue_asset_sink_write(sink, ptr, size_t(size - done));
        tmp.resize(size_t(std::min<uint64_t>(size - done, 1u << 20)));
        size_t got = 0;
        nue_result r = nue_asset_node_read(c, node, off + done, tmp.data(), tmp.size(), &got);
        if (r < 0) return r;
        if (got != tmp.size()) return NUE_ERR_CORRUPT;
        r = nue_asset_sink_write(sink, tmp.data(), got);
        if (r < 0) return r;
        done += got;
    }
    return NUE_OK;
}

nue_result write_fill(nue_asset_sink* sink, uint8_t value, uint64_t count) {
    uint8_t buf[256];
    std::memset(buf, value, sizeof(buf));
    while (count) {
        const size_t k = size_t(std::min<uint64_t>(count, sizeof(buf)));
        nue_result r = nue_asset_sink_write(sink, buf, k);
        if (r < 0) return r;
        count -= k;
    }
    return NUE_OK;
}

}  // namespace

nue_result emit_tpk_data(nue_asset_container c, nue_asset_sink* sink, const TpkPlan& plan, uint64_t payload_pos) {
    const uint64_t base = align_up(payload_pos, kDataAlign);
    nue_result r = write_fill(sink, 0x11, base - payload_pos);          // the data starts on a 128 byte boundary of the file
    if (r < 0) return r;
    uint64_t cur = 0;                                                    // relative to base
    for (const TexPlan& q : plan.tex) {
        if (!q.pal_size) continue;
        r = copy_from_node(c, sink, plan.data_node, q.pal_src, q.pal_size);
        if (r < 0) return r;
        cur += q.pal_size;
    }
    for (const TexPlan& q : plan.tex) {
        r = write_fill(sink, 0, q.placement - cur);
        if (r < 0) return r;
        if (q.edited) r = nue_asset_sink_write(sink, q.image.data(), q.image.size());
        else r = copy_from_node(c, sink, plan.data_node, q.src_off, q.image_size);
        if (r < 0) return r;
        cur = q.placement + q.image_size;
    }
    return NUE_OK;
}

}  // namespace nfs
