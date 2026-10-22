// .nuepack: the engine's resource package (writer, reader, export back to a game container). Layout in docs/NUEPACK.md.
#include "internal.hpp"

#include <algorithm>
#include <map>

using namespace nuea;

namespace {

constexpr uint32_t kHeaderSize = 128, kEntrySize = 136, kNodeSize = 48;
constexpr uint32_t kDataAlign = 64;
constexpr uint32_t kFlagTree = 1u;
constexpr uint32_t kNoEntry = 0xFFFFFFFFu;
constexpr uint32_t kCodecNone = 0, kCodecJdlz = 1;
constexpr uint32_t kNodeFlagDerived = 1u << 8;      // tree node flag: payload is generated from the textures (no bytes stored)

struct PEntry {
    uint32_t kind = 0, variant = 0, flags = 0, name_off = 0;
    uint64_t name_hash = 0, data_off = 0, stored = 0, raw = 0;
    uint32_t codec = 0, crc = 0, tex_format = 0, width = 0, height = 0, depth = 0, mips = 0, layers = 0, faces = 0, native = 0;
    uint32_t tree_node = kNoEntry, sub_index = 0;
    uint64_t res_hash = 0;
    uint8_t sha[32] = {0};
};

struct PNode {
    uint64_t id = 0;
    uint32_t parent = NUE_ASSET_NO_NODE, kind = 0, flags = 0, header_size = 0, align = 0, entry = kNoEntry;
    uint64_t size = 0;
    uint32_t name_off = kNoEntry;
};

void encode_entry(const PEntry& e, uint8_t* p) {
    std::memset(p, 0, kEntrySize);
    wr32(p + 0, e.kind); wr32(p + 4, e.variant); wr32(p + 8, e.flags); wr32(p + 12, e.name_off);
    wr64(p + 16, e.name_hash); wr64(p + 24, e.data_off); wr64(p + 32, e.stored); wr64(p + 40, e.raw);
    wr32(p + 48, e.codec); wr32(p + 52, e.crc); wr32(p + 56, e.tex_format);
    wr32(p + 60, e.width); wr32(p + 64, e.height); wr32(p + 68, e.depth); wr32(p + 72, e.mips); wr32(p + 76, e.layers); wr32(p + 80, e.faces);
    wr32(p + 84, e.native); wr32(p + 88, e.tree_node); wr32(p + 92, e.sub_index);
    std::memcpy(p + 96, e.sha, 32);
    wr64(p + 128, e.res_hash);
}

void decode_entry(const uint8_t* p, PEntry* e) {
    e->kind = rd32(p + 0); e->variant = rd32(p + 4); e->flags = rd32(p + 8); e->name_off = rd32(p + 12);
    e->name_hash = rd64(p + 16); e->data_off = rd64(p + 24); e->stored = rd64(p + 32); e->raw = rd64(p + 40);
    e->codec = rd32(p + 48); e->crc = rd32(p + 52); e->tex_format = rd32(p + 56);
    e->width = rd32(p + 60); e->height = rd32(p + 64); e->depth = rd32(p + 68); e->mips = rd32(p + 72); e->layers = rd32(p + 76); e->faces = rd32(p + 80);
    e->native = rd32(p + 84); e->tree_node = rd32(p + 88); e->sub_index = rd32(p + 92);
    std::memcpy(e->sha, p + 96, 32);
    e->res_hash = rd64(p + 128);
}

void encode_node(const PNode& n, uint8_t* p) {
    std::memset(p, 0, kNodeSize);
    wr64(p + 0, n.id); wr32(p + 8, n.parent); wr32(p + 12, n.kind); wr32(p + 16, n.flags); wr32(p + 20, n.header_size);
    wr32(p + 24, n.align); wr32(p + 28, n.entry); wr64(p + 32, n.size); wr32(p + 40, n.name_off);
}

void decode_node(const uint8_t* p, PNode* n) {
    n->id = rd64(p + 0); n->parent = rd32(p + 8); n->kind = rd32(p + 12); n->flags = rd32(p + 16); n->header_size = rd32(p + 20);
    n->align = rd32(p + 24); n->entry = rd32(p + 28); n->size = rd64(p + 32); n->name_off = rd32(p + 40);
}

void copy_fixed(char* dst, size_t cap, const std::string& s) { std::memset(dst, 0, cap); std::memcpy(dst, s.data(), std::min(s.size(), cap - 1)); }

// ---------------------------------------------------------------------------------------------------- image <-> bytes
// All layers, faces and mip levels of an image in the library's own order (layer major, then face, then level).
nue_result image_to_bytes(nue_tex_image im, std::vector<uint8_t>* out, nue_tex_image_desc* desc) {
    NUE_STRUCT_INIT(desc);
    nue_result r = nue_tex_image_get_desc(im, desc);
    if (r < 0) return r;
    out->clear();
    out->reserve(size_t(nue_tex_image_total_size(im)));
    for (uint32_t layer = 0; layer < desc->layers; ++layer)
        for (uint32_t face = 0; face < desc->faces; ++face)
            for (uint32_t l = 0; l < desc->mip_levels; ++l) {
                nue_tex_level_view v;
                NUE_STRUCT_INIT(&v);
                r = nue_tex_image_get_level(im, l, layer, face, &v);
                if (r < 0) return r;
                const uint8_t* p = static_cast<const uint8_t*>(v.data);
                out->insert(out->end(), p, p + v.size);
            }
    return NUE_OK;
}

nue_result bytes_to_image(const PEntry& e, const uint8_t* data, uint64_t size, nue_tex_image* out) {
    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    d.format = nue_tex_format(e.tex_format);
    d.width = e.width; d.height = e.height; d.depth = e.depth ? e.depth : 1; d.mip_levels = e.mips; d.layers = e.layers ? e.layers : 1; d.faces = e.faces ? e.faces : 1;
    if (d.faces == 6) d.flags |= NUE_TEX_IMAGE_CUBE;
    if (e.flags & NUE_PACK_ENTRY_PALETTED) {
        nue_asset_native_texture n;
        NUE_STRUCT_INIT(&n);
        n.native_format = e.native; n.tex_format = e.tex_format; n.width = e.width; n.height = e.height; n.depth = 1; n.mip_levels = e.mips;
        n.flags = NUE_ASSET_RESFLAG_PALETTED; n.palette_size = 1024; n.data = data; n.size = size;
        if (size < 1024) return NUE_ERR_CORRUPT;
        return native_to_image(n, out);
    }
    nue_tex_image im = nullptr;
    nue_result r = nue_tex_image_create(&d, &im);
    if (r < 0) return r;
    uint64_t off = 0;
    for (uint32_t layer = 0; layer < d.layers; ++layer)
        for (uint32_t face = 0; face < d.faces; ++face)
            for (uint32_t l = 0; l < d.mip_levels; ++l) {
                nue_tex_level_view v;
                NUE_STRUCT_INIT(&v);
                nue_tex_image_get_level(im, l, layer, face, &v);
                if (off + v.size > size) { nue_tex_image_destroy(im); return NUE_ERR_CORRUPT; }
                std::memcpy(v.data, data + off, size_t(v.size));
                off += v.size;
            }
    *out = im;
    return NUE_OK;
}

// Which platform class a variant is for.
nue_tex_device_class class_for_variant(nue_pack_variant v) {
    switch (v) {
        case NUE_PACK_VARIANT_BC: return NUE_TEX_DEVICE_DESKTOP_BC;
        case NUE_PACK_VARIANT_ASTC: return NUE_TEX_DEVICE_ANDROID_ASTC;
        case NUE_PACK_VARIANT_ETC2: return NUE_TEX_DEVICE_ANDROID_ETC2;
        case NUE_PACK_VARIANT_PVRTC: return NUE_TEX_DEVICE_APPLE_PVRTC;
        default: return NUE_TEX_DEVICE_SOFTWARE;
    }
}

}  // namespace

NUE_ASSETS_API const char* NUE_CALL nue_pack_variant_name(nue_pack_variant v) {
    static const char* names[] = {"source", "bc", "astc", "etc2", "pvrtc", "rgba8"};
    return unsigned(v) < NUE_PACK_VARIANT_COUNT ? names[v] : "?";
}
NUE_ASSETS_API nue_pack_variant NUE_CALL nue_pack_variant_from_name(const char* name) {
    for (unsigned i = 0; i < NUE_PACK_VARIANT_COUNT; ++i) if (name && iequals(name, nue_pack_variant_name(nue_pack_variant(i)))) return nue_pack_variant(i);
    return NUE_PACK_VARIANT_COUNT;
}
NUE_ASSETS_API nue_pack_variant NUE_CALL nue_pack_variant_for_device_class(nue_tex_device_class c) {
    switch (c) {
        case NUE_TEX_DEVICE_DESKTOP_BC: case NUE_TEX_DEVICE_DESKTOP_BC_ETC2: case NUE_TEX_DEVICE_ANDROID_ETC2_BC: case NUE_TEX_DEVICE_APPLE_SILICON_MAC: return NUE_PACK_VARIANT_BC;
        case NUE_TEX_DEVICE_ANDROID_ASTC: case NUE_TEX_DEVICE_APPLE_ASTC: return NUE_PACK_VARIANT_ASTC;
        case NUE_TEX_DEVICE_ANDROID_ETC2: return NUE_PACK_VARIANT_ETC2;
        case NUE_TEX_DEVICE_APPLE_PVRTC: return NUE_PACK_VARIANT_PVRTC;
        default: return NUE_PACK_VARIANT_RGBA8;
    }
}

// ======================================================================================================== builder
struct nue_pack_builder_s {
    Ctx* ctx = nullptr;
    std::string source_name, format_name, codec_name;
    bool has_tree = false;
    uint32_t variant_mask = 1u << NUE_PACK_VARIANT_SOURCE;
    std::vector<PEntry> entries;
    std::vector<Buffer> data;                               // per entry: stored bytes (empty for an entry that shares another's data)
    std::vector<uint32_t> shares;                           // per entry: index of the entry whose data it shares, or kNoEntry
    std::vector<PNode> nodes;
    std::string strings = std::string(1, '\0');
    std::map<std::string, uint32_t> string_ids;
    std::map<std::string, uint32_t> dedupe;                 // "sha:codec" -> first entry
    bool partial = false;

    uint32_t intern(const std::string& s) {
        if (s.empty()) return 0;
        auto it = string_ids.find(s);
        if (it != string_ids.end()) return it->second;
        const uint32_t off = uint32_t(strings.size());
        strings += s;
        strings.push_back('\0');
        string_ids[s] = off;
        return off;
    }

    // Adds an entry holding `raw` bytes (compressed with `codec` when it makes them smaller and compress is allowed).
    nue_result add_entry(PEntry e, const uint8_t* raw, size_t size, bool allow_compress, uint32_t* out_index) {
        e.raw = size;
        e.crc = nue_crc32(0, raw, size);
        nue_sha256(raw, size, e.sha);
        char hex[65];
        nue_hex_encode(e.sha, 32, hex, sizeof(hex));
        std::string key = std::string(hex) + ":";
        Buffer stored;
        e.codec = kCodecNone;
        const uint8_t* src = raw;
        size_t stored_size = size;
        if (allow_compress && size >= 256 && ctx->find_codec("jdlz")) {
            const size_t cap = nue_jdlz_compress_bound(size);
            Buffer packed;
            nue_result r = packed.alloc(ctx, cap);
            if (r < 0) return r;
            size_t n = 0;
            if (nue_jdlz_compress(raw, size, 6, packed.data(), cap, &n) == NUE_OK && n + 16 < size) { packed.set_size(n); stored = std::move(packed); e.codec = kCodecJdlz; src = stored.data(); stored_size = n; }
        }
        key += std::to_string(e.codec);
        e.stored = stored_size;
        const uint32_t index = uint32_t(entries.size());
        auto it = dedupe.find(key);
        Buffer own;
        if (it != dedupe.end()) { shares.push_back(it->second); }
        else {
            shares.push_back(kNoEntry);
            dedupe[key] = index;
            if (e.codec == kCodecJdlz) own = std::move(stored);
            else { nue_result r = own.alloc(ctx, size); if (r < 0) return r; if (size) std::memcpy(own.data(), src, size); }
        }
        entries.push_back(e);
        data.push_back(std::move(own));
        if (out_index) *out_index = index;
        return NUE_OK;
    }
};

NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_create(nue_assets_context ctx, const nue_pack_builder_desc* desc, nue_pack_builder* out) {
    if (!ctx || !out) return NUE_ERR_INVALID_ARGUMENT;
    std::unique_ptr<nue_pack_builder_s> b(new nue_pack_builder_s());
    b->ctx = ctx_of(ctx);
    if (desc && desc->source_name) b->source_name = desc->source_name;
    *out = reinterpret_cast<nue_pack_builder>(b.release());
    return NUE_OK;
}

NUE_ASSETS_API void NUE_CALL nue_pack_builder_destroy(nue_pack_builder b) { delete reinterpret_cast<nue_pack_builder_s*>(b); }

namespace {

nue_pack_builder_s* bld(nue_pack_builder b) { return reinterpret_cast<nue_pack_builder_s*>(b); }

// Convert for one variant. *same = true when the variant is exactly the source (nothing to store twice).
nue_result make_variant(nue_tex_image src, nue_pack_variant variant, const nue_pack_convert_options& o, nue_tex_image* out, bool* same, bool* lossy) {
    *out = nullptr; *same = false; *lossy = false;
    nue_tex_image_desc d;
    NUE_STRUCT_INIT(&d);
    nue_result r = nue_tex_image_get_desc(src, &d);
    if (r < 0) return r;
    nue_tex_format target = NUE_TEX_RGBA8_UNORM;
    uint32_t skip = 0;
    if (variant != NUE_PACK_VARIANT_RGBA8) {
        nue_tex_caps caps;
        NUE_STRUCT_INIT(&caps);
        r = nue_tex_caps_for_class(class_for_variant(variant), &caps);
        if (r < 0) return r;
        nue_tex_policy pol;
        nue_tex_policy_default(&pol);
        pol.max_dimension = o.max_dimension;
        pol.quality = uint32_t(o.quality) + 1;
        nue_tex_plan_request req;
        NUE_STRUCT_INIT(&req);
        req.source_format = d.format; req.width = d.width; req.height = d.height; req.depth = d.depth ? d.depth : 1; req.mip_levels = d.mip_levels;
        req.layers = d.layers; req.faces = d.faces; req.policy = &pol;
        if (d.faces == 6) req.flags |= NUE_TEX_REQ_CUBE;
        nue_tex_plan plan;
        NUE_STRUCT_INIT(&plan);
        r = nue_tex_make_plan(&caps, &req, &plan);
        if (r < 0 || plan.action == NUE_TEX_ACTION_UNSUPPORTED) return r < 0 ? r : NUE_ERR_UNSUPPORTED;
        target = plan.target_format;
        skip = plan.skip_mips;
        *lossy = (plan.flags & NUE_TEX_PLAN_LOSSY) != 0;
    } else if (o.max_dimension) {
        while (skip + 1 < d.mip_levels && std::max(nue_tex_mip_extent(d.width, skip), nue_tex_mip_extent(d.height, skip)) > o.max_dimension) ++skip;
    }
    if (target == d.format && skip == 0) { *same = true; return NUE_OK; }
    nue_tex_image cur = nullptr;
    if (target != d.format) {
        nue_tex_encode_params ep;
        nue_tex_encode_params_default(&ep);
        ep.quality = o.quality;
        ep.executor = o.executor;
        nue_tex_mip_params mp;
        nue_tex_mip_params_default(&mp);
        r = nue_tex_image_convert(src, target, d.mip_levels, &ep, &mp, &cur);
        if (r < 0) return r;
    }
    if (skip) {
        nue_tex_image dropped = nullptr;
        r = nue_tex_image_drop_top_mips(cur ? cur : src, skip, &dropped);
        if (cur) nue_tex_image_destroy(cur);
        if (r < 0) return r;
        cur = dropped;
    }
    *out = cur;
    return NUE_OK;
}

nue_result add_texture(nue_pack_builder_s* b, const std::string& name, uint64_t res_hash, uint32_t tree_node, uint32_t sub_index, nue_tex_image image,
                       const nue_asset_native_texture* native, const nue_pack_convert_options& o) {
    // SOURCE variant: the game's own bytes when we have them, else the image as it is
    PEntry e;
    e.kind = NUE_ASSET_RES_TEXTURE;
    e.variant = NUE_PACK_VARIANT_SOURCE;
    e.name_off = b->intern(name);
    e.name_hash = name_hash64(name.c_str());
    e.tree_node = tree_node;
    e.sub_index = sub_index;
    e.res_hash = res_hash;
    nue_tex_image owned = nullptr;
    if (!image && native) {
        nue_result r = native_to_image(*native, &owned);
        if (r < 0) return r;
        image = owned;
    }
    if (!image) return NUE_ERR_INVALID_ARGUMENT;
    nue_tex_image_desc d;
    std::vector<uint8_t> bytes;
    nue_result r = image_to_bytes(image, &bytes, &d);
    if (r < 0) { if (owned) nue_tex_image_destroy(owned); return r; }
    if (native) {
        e.tex_format = native->tex_format; e.width = native->width; e.height = native->height; e.depth = native->depth ? native->depth : 1; e.mips = native->mip_levels;
        e.layers = 1; e.faces = 1; e.native = native->native_format;
        if (native->flags & NUE_ASSET_RESFLAG_PALETTED) e.flags |= NUE_PACK_ENTRY_PALETTED;
        bytes.assign(static_cast<const uint8_t*>(native->data), static_cast<const uint8_t*>(native->data) + size_t(native->size));
    } else {
        e.tex_format = d.format; e.width = d.width; e.height = d.height; e.depth = d.depth ? d.depth : 1; e.mips = d.mip_levels; e.layers = d.layers; e.faces = d.faces;
    }
    r = b->add_entry(e, bytes.data(), bytes.size(), false, nullptr);
    if (r < 0) { if (owned) nue_tex_image_destroy(owned); return r; }
    // the platform variants
    for (uint32_t v = NUE_PACK_VARIANT_BC; v < NUE_PACK_VARIANT_COUNT; ++v) {
        if (!(o.variants & (1u << v))) continue;
        nue_tex_image out = nullptr;
        bool same = false, lossy = false;
        nue_result vr = make_variant(image, nue_pack_variant(v), o, &out, &same, &lossy);
        PEntry ve = e;
        ve.variant = v;
        ve.flags = 0;
        ve.native = 0;
        std::vector<uint8_t> vb;
        if (vr < 0) {                                                // not transcodable: this variant falls back to RGBA8 pixels
            b->partial = true;
            nue_tex_encode_params ep;
            nue_tex_encode_params_default(&ep);
            nue_tex_mip_params mp;
            nue_tex_mip_params_default(&mp);
            vr = nue_tex_image_convert(image, NUE_TEX_RGBA8_UNORM, d.mip_levels, &ep, &mp, &out);
            if (vr < 0) { if (owned) nue_tex_image_destroy(owned); return vr; }
            same = false;
        }
        if (same) {                                                  // identical to the source pixels: same bytes (deduplicated by hash when the source was not paletted)
            vb = bytes;
            nue_tex_image_desc sd = d;
            if (native && (native->flags & NUE_ASSET_RESFLAG_PALETTED)) { std::vector<uint8_t> t; image_to_bytes(image, &t, &sd); vb = t; }
            ve.tex_format = sd.format; ve.width = sd.width; ve.height = sd.height; ve.depth = sd.depth ? sd.depth : 1; ve.mips = sd.mip_levels; ve.layers = sd.layers; ve.faces = sd.faces;
        } else {
            nue_tex_image_desc vd;
            r = image_to_bytes(out, &vb, &vd);
            if (r < 0) { nue_tex_image_destroy(out); if (owned) nue_tex_image_destroy(owned); return r; }
            ve.tex_format = vd.format; ve.width = vd.width; ve.height = vd.height; ve.depth = vd.depth ? vd.depth : 1; ve.mips = vd.mip_levels; ve.layers = vd.layers; ve.faces = vd.faces;
            nue_tex_image_destroy(out);
        }
        r = b->add_entry(ve, vb.data(), vb.size(), false, nullptr);
        if (r < 0) { if (owned) nue_tex_image_destroy(owned); return r; }
        b->variant_mask |= 1u << v;
    }
    if (owned) nue_tex_image_destroy(owned);
    return NUE_OK;
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_add_blob(nue_pack_builder builder, const char* name, const void* data, size_t size, uint32_t flags) {
    nue_pack_builder_s* b = bld(builder);
    if (!b || !name || (!data && size)) return NUE_ERR_INVALID_ARGUMENT;
    PEntry e;
    e.kind = NUE_ASSET_RES_BLOB;
    e.name_off = b->intern(name);
    e.name_hash = name_hash64(name);
    return b->add_entry(e, static_cast<const uint8_t*>(data), size, !(flags & NUE_PACK_CONVERT_NO_COMPRESS), nullptr);
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_add_image(nue_pack_builder builder, const char* name, nue_tex_image image, const nue_pack_convert_options* options) {
    nue_pack_builder_s* b = bld(builder);
    if (!b || !name || !image) return NUE_ERR_INVALID_ARGUMENT;
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    o.quality = NUE_TEX_QUALITY_NORMAL;
    if (options) std::memcpy(&o, options, std::min<size_t>(options->struct_size, sizeof(o)));
    return add_texture(b, name, name_hash64(name), kNoEntry, 0, image, nullptr, o);
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_add_container(nue_pack_builder builder, nue_asset_container container, const nue_pack_convert_options* options) {
    nue_pack_builder_s* b = bld(builder);
    nue_asset_container_s* c = cont(container);
    if (!b || !c) return NUE_ERR_INVALID_ARGUMENT;
    if (b->has_tree) { nue_set_last_error("assets: a package holds the tree of one container only"); return NUE_ERR_ALREADY_EXISTS; }
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    o.quality = NUE_TEX_QUALITY_NORMAL;
    if (options) std::memcpy(&o, options, std::min<size_t>(options->struct_size, sizeof(o)));
    b->has_tree = true;
    b->format_name = c->format_name;
    b->codec_name = c->codec ? c->codec->name : "";

    // nodes whose payload the format regenerates from textures
    std::vector<bool> derived(c->nodes.size(), false);
    for (auto& r : c->res) if (r.info.kind == NUE_ASSET_RES_TEXTURE_PACK && r.info.derived_node < derived.size()) derived[r.info.derived_node] = true;

    const uint32_t base_nodes = uint32_t(b->nodes.size());
    for (uint32_t i = 0; i < c->nodes.size(); ++i) {
        const NodeRec& n = c->nodes[i];
        PNode pn;
        pn.id = n.info.id;
        pn.parent = n.info.parent == NUE_ASSET_NO_NODE ? NUE_ASSET_NO_NODE : n.info.parent + base_nodes;
        pn.kind = n.info.kind;
        pn.flags = n.info.flags & ~uint32_t(NUE_ASSET_NODE_EDITED);
        pn.header_size = n.info.header_size;
        pn.align = n.info.align;
        pn.size = n.info.size;
        pn.name_off = (n.info.flags & NUE_ASSET_NODE_HAS_NAME) ? b->intern(n.name) : kNoEntry;
        if (derived[i]) pn.flags |= kNodeFlagDerived;
        if (n.info.kind != NUE_ASSET_NODE_GROUP && !derived[i] && n.info.size > 0) {
            std::vector<uint8_t> bytes(size_t(n.info.size));
            size_t got = 0;
            nue_result r = nue_asset_node_read(container, i, 0, bytes.data(), bytes.size(), &got);
            if (r < 0) return r;
            if (got != bytes.size()) return NUE_ERR_CORRUPT;
            const bool zeros = std::all_of(bytes.begin(), bytes.end(), [](uint8_t x) { return x == 0; });
            if (!(n.info.kind == NUE_ASSET_NODE_PADDING && zeros)) {          // padding made of zeros is stored as its size only
                PEntry e;
                e.kind = NUE_ASSET_RES_BLOB;
                e.flags = NUE_PACK_ENTRY_TREE_BLOB;
                e.tree_node = i + base_nodes;
                const std::string nm = "node/" + std::to_string(i);
                e.name_off = b->intern(nm);
                e.name_hash = name_hash64(nm.c_str());
                uint32_t idx = 0;
                r = b->add_entry(e, bytes.data(), bytes.size(), !(o.flags & NUE_PACK_CONVERT_NO_COMPRESS), &idx);
                if (r < 0) return r;
                pn.entry = idx;
            }
        }
        b->nodes.push_back(pn);
    }

    // textures
    for (uint32_t i = 0; i < c->res.size(); ++i) {
        const ResRec& rr = c->res[i];
        if (rr.info.kind != NUE_ASSET_RES_TEXTURE) continue;
        std::string name = rr.name;
        if (rr.info.parent != NUE_ASSET_NO_NODE && rr.info.parent < c->res.size() && !c->res[rr.info.parent].name.empty()) name = c->res[rr.info.parent].name + "/" + rr.name;
        nue_asset_native_texture nt;
        void* mem = nullptr;
        nue_tex_image image = nullptr;
        const nue_asset_native_texture* use_native = nullptr;
        nue_result r = nue_asset_resource_get_native(container, i, &nt, &mem);
        if (r >= 0) use_native = &nt;
        else {
            r = nue_asset_resource_get_image(container, i, &image);
            if (r < 0) { nue_set_last_error("assets: texture '%s' cannot be read", name.c_str()); return r; }
        }
        // the full image for the transcoded variants
        if (use_native && o.variants) {
            r = nue_asset_resource_get_image(container, i, &image);
            if (r < 0) { nue_free(mem); return r; }
        }
        r = add_texture(b, name, rr.info.hash, rr.info.node + base_nodes, rr.info.sub_index, image, use_native, o);
        if (image) nue_tex_image_destroy(image);
        if (mem) nue_free(mem);
        if (r < 0) return r;
    }
    return b->partial ? NUE_OK_PARTIAL : NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_builder_write(nue_pack_builder builder, const char* path, void** out_data, size_t* out_size) {
    nue_pack_builder_s* b = bld(builder);
    if (!b || (!path && !out_data)) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t source_name_off = b->intern(b->source_name);               // before the layout: it lives in the string table
    const uint64_t n_entries = b->entries.size(), n_nodes = b->nodes.size();
    const uint64_t entry_off = kHeaderSize;
    const uint64_t node_off = nuea::align_up(entry_off + n_entries * kEntrySize, 8);
    const uint64_t str_off = nuea::align_up(node_off + n_nodes * kNodeSize, 8);
    const uint64_t str_size = b->strings.size();
    uint64_t pos = nuea::align_up(str_off + str_size, kDataAlign);

    std::vector<uint64_t> offsets(b->entries.size(), 0);
    for (size_t i = 0; i < b->entries.size(); ++i) {
        if (b->shares[i] != kNoEntry) { offsets[i] = offsets[b->shares[i]]; continue; }
        offsets[i] = pos;
        pos = nuea::align_up(pos + b->entries[i].stored, kDataAlign);
    }
    const uint64_t file_size = b->entries.empty() ? nuea::align_up(str_off + str_size, kDataAlign) : pos;

    MemSink sink(b->ctx);
    Buffer& out = sink.buffer();
    nue_result r = out.alloc(b->ctx, size_t(file_size));
    if (r < 0) return r;
    uint8_t* f = out.data();
    for (size_t i = 0; i < b->entries.size(); ++i) {
        PEntry e = b->entries[i];
        e.data_off = offsets[i];
        encode_entry(e, f + entry_off + i * kEntrySize);
        const size_t src_index = b->shares[i] != kNoEntry ? b->shares[i] : i;
        if (b->shares[i] == kNoEntry && e.stored) std::memcpy(f + e.data_off, b->data[src_index].data(), size_t(e.stored));
    }
    for (size_t i = 0; i < b->nodes.size(); ++i) {
        PNode n = b->nodes[i];
        encode_node(n, f + node_off + i * kNodeSize);
    }
    std::memcpy(f + str_off, b->strings.data(), b->strings.size());

    // header
    std::memset(f, 0, kHeaderSize);
    wr32(f + 0, NUE_PACK_MAGIC);
    wr16(f + 4, uint16_t(NUE_PACK_VERSION_MAJOR)); wr16(f + 6, uint16_t(NUE_PACK_VERSION_MINOR));
    wr32(f + 8, kHeaderSize);
    wr32(f + 12, b->has_tree ? kFlagTree : 0);
    wr64(f + 16, file_size);
    wr64(f + 24, entry_off); wr32(f + 32, uint32_t(n_entries)); wr32(f + 36, kEntrySize);
    wr64(f + 40, str_off); wr64(f + 48, str_size);
    wr64(f + 56, node_off); wr32(f + 64, uint32_t(n_nodes)); wr32(f + 68, kNodeSize);
    wr32(f + 72, b->variant_mask);
    wr32(f + 76, kDataAlign);
    wr32(f + 80, source_name_off);
    copy_fixed(reinterpret_cast<char*>(f + 88), 24, b->format_name);
    copy_fixed(reinterpret_cast<char*>(f + 112), 8, b->codec_name);
    wr32(f + 124, nue_crc32(0, f + entry_off, size_t(str_off + str_size - entry_off)));
    wr32(f + 120, 0);
    wr32(f + 120, nue_crc32(0, f, kHeaderSize));
    out.set_size(size_t(file_size));
    if (path) {
        r = nue_file_save(path, f, size_t(file_size));
        if (r < 0) nue_set_last_error("assets: cannot write '%s'", path);
        return r;
    }
    size_t n = 0;
    uint8_t* mem = out.detach(&n);
    *out_data = mem;
    if (out_size) *out_size = n;
    return NUE_OK;
}

// ======================================================================================================== reader
struct nue_pack_s {
    Ctx* ctx = nullptr;
    std::unique_ptr<nue_asset_source> src;
    FileSource* file_src = nullptr;                         // when opened from a path (the file handle serves asynchronous reads); owned by `src`
    nue_file mapped_file = 0;
    nue_file_map_info map{};
    bool mapped = false;
    const uint8_t* base = nullptr;                          // whole file when in memory / mapped
    uint64_t size = 0;
    uint32_t vmajor = 0, vminor = 0, flags = 0, variant_mask = 0;
    std::vector<PEntry> entries;
    std::vector<PNode> nodes;
    std::string strings;
    std::string source_name, format, codec;
    std::vector<std::pair<uint64_t, uint32_t>> by_hash;
    ~nue_pack_s() { if (mapped_file) { if (mapped) nue_file_unmap(mapped_file, &map); nue_file_close(mapped_file); } }
    const char* str(uint32_t off) const { return off < strings.size() ? strings.c_str() + off : ""; }
};

namespace {

nue_pack_s* pk(nue_pack p) { return reinterpret_cast<nue_pack_s*>(p); }

class MappedSource : public nue_asset_source {
public:
    MappedSource(const uint8_t* base, uint64_t size) : base_(base), size_(size) {}
    nue_result read(uint64_t offset, void* buffer, size_t size, size_t* out_read) const override {
        size_t got = offset < size_ ? size_t(std::min<uint64_t>(size, size_ - offset)) : 0;
        if (got) std::memcpy(buffer, base_ + offset, got);
        if (out_read) *out_read = got;
        return got == size ? NUE_OK : NUE_OK_PARTIAL;
    }
    uint64_t size() const override { return size_; }
    const void* peek(uint64_t offset, size_t size) const override { return (offset <= size_ && size <= size_ - offset) ? base_ + offset : nullptr; }
private:
    const uint8_t* base_;
    uint64_t size_;
};

nue_result read_exact(const nue_asset_source* s, uint64_t off, void* buf, size_t n) {
    size_t got = 0;
    nue_result r = s->read(off, buf, n, &got);
    if (r < 0) return r;
    return got == n ? NUE_OK : NUE_ERR_CORRUPT;
}

nue_result parse_pack(nue_pack_s* p, bool verify) {
    const nue_asset_source* s = p->src.get();
    p->size = s->size();
    uint8_t h[kHeaderSize];
    if (p->size < kHeaderSize) { nue_set_last_error("nuepack: file too short"); return NUE_ERR_CORRUPT; }
    nue_result r = read_exact(s, 0, h, kHeaderSize);
    if (r < 0) return r;
    if (rd32(h) != NUE_PACK_MAGIC) { nue_set_last_error("nuepack: bad magic"); return NUE_ERR_CORRUPT; }
    p->vmajor = rd16(h + 4); p->vminor = rd16(h + 6);
    if (p->vmajor != NUE_PACK_VERSION_MAJOR) { nue_set_last_error("nuepack: version %u.%u is not supported", p->vmajor, p->vminor); return NUE_ERR_VERSION_MISMATCH; }
    if (rd32(h + 8) != kHeaderSize) return NUE_ERR_CORRUPT;
    uint8_t hc[kHeaderSize];
    std::memcpy(hc, h, kHeaderSize);
    const uint32_t header_crc = rd32(h + 120);
    wr32(hc + 120, 0);
    if (nue_crc32(0, hc, kHeaderSize) != header_crc) { nue_set_last_error("nuepack: header checksum mismatch"); return NUE_ERR_CORRUPT; }
    p->flags = rd32(h + 12);
    if (rd64(h + 16) != p->size) { nue_set_last_error("nuepack: size in the header does not match the file"); return NUE_ERR_CORRUPT; }
    const uint64_t entry_off = rd64(h + 24), str_off = rd64(h + 40), str_size = rd64(h + 48), node_off = rd64(h + 56);
    const uint32_t n_entries = rd32(h + 32), esize = rd32(h + 36), n_nodes = rd32(h + 64), nsize = rd32(h + 68);
    p->variant_mask = rd32(h + 72);
    const uint32_t align = rd32(h + 76);
    if (esize != kEntrySize || nsize != kNodeSize || !nuea::is_pow2(align)) return NUE_ERR_CORRUPT;
    uint64_t e_end, n_end, s_end, tmp;
    if (!mul_ok(n_entries, esize, &tmp) || !add_ok(entry_off, tmp, &e_end) || !mul_ok(n_nodes, nsize, &tmp) || !add_ok(node_off, tmp, &n_end) || !add_ok(str_off, str_size, &s_end)) return NUE_ERR_CORRUPT;
    if (entry_off < kHeaderSize || e_end > p->size || node_off < e_end || n_end > p->size || str_off < n_end || s_end > p->size) { nue_set_last_error("nuepack: tables outside the file"); return NUE_ERR_CORRUPT; }
    // table checksum over entries..strings
    {
        std::vector<uint8_t> t(size_t(s_end - entry_off));
        r = read_exact(s, entry_off, t.data(), t.size());
        if (r < 0) return r;
        if (nue_crc32(0, t.data(), t.size()) != rd32(h + 124)) { nue_set_last_error("nuepack: table checksum mismatch"); return NUE_ERR_CORRUPT; }
        p->entries.resize(n_entries);
        for (uint32_t i = 0; i < n_entries; ++i) decode_entry(t.data() + size_t(i) * esize, &p->entries[i]);
        p->nodes.resize(n_nodes);
        for (uint32_t i = 0; i < n_nodes; ++i) decode_node(t.data() + (node_off - entry_off) + size_t(i) * nsize, &p->nodes[i]);
        p->strings.assign(reinterpret_cast<const char*>(t.data() + (str_off - entry_off)), size_t(str_size));
    }
    if (p->strings.empty() || p->strings.back() != '\0') return NUE_ERR_CORRUPT;
    char buf[33];
    std::memcpy(buf, h + 88, 24); buf[24] = 0; p->format = buf;
    std::memcpy(buf, h + 112, 8); buf[8] = 0; p->codec = buf;
    p->source_name = p->str(rd32(h + 80));
    for (uint32_t i = 0; i < n_entries; ++i) {
        const PEntry& e = p->entries[i];
        uint64_t end;
        if (!add_ok(e.data_off, e.stored, &end) || end > p->size || (e.stored && e.data_off % align != 0) || e.name_off >= p->strings.size()) { nue_set_last_error("nuepack: entry %u is outside the file", i); return NUE_ERR_CORRUPT; }
        if (e.codec > kCodecJdlz || (e.codec == kCodecNone && e.raw != e.stored)) return NUE_ERR_CORRUPT;
        p->by_hash.push_back({e.name_hash, i});
    }
    for (uint32_t i = 0; i < n_nodes; ++i) {
        const PNode& n = p->nodes[i];
        if ((n.parent != NUE_ASSET_NO_NODE && n.parent >= i) || (n.entry != kNoEntry && n.entry >= n_entries)) { nue_set_last_error("nuepack: tree node %u is invalid", i); return NUE_ERR_CORRUPT; }
    }
    std::sort(p->by_hash.begin(), p->by_hash.end());
    (void)verify;
    return NUE_OK;
}

nue_result finish_open(nue_pack_s* p, nue_pack_open_flags flags, nue_pack* out) {
    nue_result r = parse_pack(p, (flags & NUE_PACK_OPEN_VERIFY) != 0);
    if (r < 0) return r;
    nue_pack h = reinterpret_cast<nue_pack>(p);
    if (flags & NUE_PACK_OPEN_VERIFY) {
        uint32_t bad = 0;
        r = nue_pack_verify(h, &bad);
        if (r < 0 || bad) { nue_set_last_error("nuepack: %u entries fail their hash", bad); return NUE_ERR_CORRUPT; }
    }
    *out = h;
    return NUE_OK;
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_pack_open_memory(nue_assets_context ctx, const void* data, size_t size, nue_pack_open_flags flags, nue_pack* out) {
    if (!ctx || !out || (!data && size)) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<nue_pack_s> p(new nue_pack_s());
    p->ctx = ctx_of(ctx);
    p->src.reset(new MemSource(data, size));
    p->base = static_cast<const uint8_t*>(data);
    nue_pack out_h = nullptr;
    nue_result r = finish_open(p.get(), flags, &out_h);
    if (r < 0) return r;
    p.release();
    *out = out_h;
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_open_file(nue_assets_context ctx, const char* path, nue_pack_open_flags flags, nue_pack* out) {
    if (!ctx || !path || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<nue_pack_s> p(new nue_pack_s());
    p->ctx = ctx_of(ctx);
    if (flags & NUE_PACK_OPEN_MAP) {
        nue_file f = 0;
        nue_result r = nue_file_open(path, NUE_OPEN_READ | NUE_OPEN_RANDOM, &f);
        if (r < 0) return r;
        p->mapped_file = f;
        NUE_STRUCT_INIT(&p->map);
        r = nue_file_map(f, 0, 0, &p->map);
        if (r < 0) return r;
        p->mapped = true;
        p->base = static_cast<const uint8_t*>(p->map.data);
        p->src.reset(new MappedSource(p->base, p->map.size));
    } else {
        std::unique_ptr<FileSource> fs;
        nue_result r = FileSource::open(path, &fs);
        if (r < 0) { nue_set_last_error("nuepack: cannot open '%s'", path); return r; }
        p->file_src = fs.get();
        p->src = std::move(fs);
    }
    nue_pack out_h = nullptr;
    nue_result r = finish_open(p.get(), flags, &out_h);
    if (r < 0) return r;
    p.release();
    *out = out_h;
    return NUE_OK;
}

NUE_ASSETS_API void NUE_CALL nue_pack_close(nue_pack pack) { delete pk(pack); }

NUE_ASSETS_API nue_result NUE_CALL nue_pack_get_info(nue_pack pack, nue_pack_info* out) {
    nue_pack_s* p = pk(pack);
    if (!p || !out) return NUE_ERR_INVALID_ARGUMENT;
    nue_pack_info i;
    NUE_STRUCT_INIT(&i);
    i.version_major = p->vmajor; i.version_minor = p->vminor;
    i.entry_count = uint32_t(p->entries.size()); i.tree_node_count = uint32_t(p->nodes.size());
    i.total_size = p->size;
    i.variant_mask = p->variant_mask;
    i.source_name = p->source_name.c_str();
    i.has_tree = (p->flags & kFlagTree) != 0;
    i.verified_header = NUE_TRUE;
    copy_fixed(i.format_name, sizeof(i.format_name), p->format);
    copy_fixed(i.codec_name, sizeof(i.codec_name), p->codec);
    std::memcpy(out, &i, std::min<size_t>(out->struct_size ? out->struct_size : sizeof(i), sizeof(i)));
    return NUE_OK;
}

NUE_ASSETS_API uint32_t NUE_CALL nue_pack_entry_count(nue_pack pack) { return pack ? uint32_t(pk(pack)->entries.size()) : 0; }

NUE_ASSETS_API nue_result NUE_CALL nue_pack_entry_get(nue_pack pack, uint32_t index, nue_pack_entry_info* out) {
    nue_pack_s* p = pk(pack);
    if (!p || !out) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= p->entries.size()) return NUE_ERR_NOT_FOUND;
    const PEntry& e = p->entries[index];
    nue_pack_entry_info i;
    NUE_STRUCT_INIT(&i);
    i.index = index; i.kind = e.kind; i.variant = e.variant; i.flags = e.flags; i.tex_format = e.tex_format;
    i.width = e.width; i.height = e.height; i.depth = e.depth; i.mip_levels = e.mips; i.layers = e.layers; i.faces = e.faces;
    i.name_hash = e.name_hash; i.name = p->str(e.name_off);
    i.stored_size = e.stored; i.raw_size = e.raw; i.offset = e.data_off; i.codec = e.codec; i.crc32 = e.crc;
    std::memcpy(i.sha256, e.sha, 32);
    std::memcpy(out, &i, std::min<size_t>(out->struct_size ? out->struct_size : sizeof(i), sizeof(i)));
    return NUE_OK;
}

NUE_ASSETS_API uint32_t NUE_CALL nue_pack_entry_find(nue_pack pack, const char* name, nue_pack_variant preferred) {
    nue_pack_s* p = pk(pack);
    if (!p || !name) return NUE_ASSET_NO_NODE;
    const uint64_t h = name_hash64(name);
    auto it = std::lower_bound(p->by_hash.begin(), p->by_hash.end(), std::make_pair(h, uint32_t(0)));
    uint32_t best = NUE_ASSET_NO_NODE;
    int best_rank = 99;
    for (; it != p->by_hash.end() && it->first == h; ++it) {
        const PEntry& e = p->entries[it->second];
        if (!iequals(p->str(e.name_off), name)) continue;
        // rank: the preferred variant, then the variants in order of how widely usable they are, SOURCE last
        int rank = e.variant == uint32_t(preferred) ? 0 : e.variant == NUE_PACK_VARIANT_SOURCE ? 8 : (e.variant == NUE_PACK_VARIANT_RGBA8 ? 7 : 5);
        if (e.kind != NUE_ASSET_RES_TEXTURE) rank = 0;
        if (rank < best_rank) { best_rank = rank; best = it->second; }
    }
    return best;
}

namespace {

// Bytes of an entry: a pointer into memory when possible, else a copy / decoded buffer owned by the caller (nue_free).
nue_result entry_bytes(nue_pack_s* p, uint32_t index, const void** out, size_t* out_size, nue_bool* owned) {
    if (index >= p->entries.size()) return NUE_ERR_NOT_FOUND;
    const PEntry& e = p->entries[index];
    if (e.raw > SIZE_MAX / 2) return NUE_ERR_OUT_OF_MEMORY;
    *owned = NUE_FALSE;
    *out_size = size_t(e.raw);
    if (e.codec == kCodecNone) {
        if (const void* ptr = p->src->peek(e.data_off, size_t(e.stored))) { *out = ptr; return NUE_OK; }
        void* mem = nue_alloc(e.stored ? size_t(e.stored) : 1, 16);
        if (!mem) return NUE_ERR_OUT_OF_MEMORY;
        nue_result r = read_exact(p->src.get(), e.data_off, mem, size_t(e.stored));
        if (r < 0) { nue_free(mem); return r; }
        *out = mem; *owned = NUE_TRUE;
        return NUE_OK;
    }
    std::vector<uint8_t> tmp;
    const void* in = p->src->peek(e.data_off, size_t(e.stored));
    if (!in) { tmp.resize(size_t(e.stored)); nue_result r = read_exact(p->src.get(), e.data_off, tmp.data(), tmp.size()); if (r < 0) return r; in = tmp.data(); }
    void* mem = nue_alloc(e.raw ? size_t(e.raw) : 1, 16);
    if (!mem) return NUE_ERR_OUT_OF_MEMORY;
    size_t written = 0;
    nue_result r = nue_jdlz_decompress(in, size_t(e.stored), mem, size_t(e.raw), &written);
    if (r < 0 || written != e.raw) { nue_free(mem); return r < 0 ? r : NUE_ERR_CORRUPT; }
    *out = mem; *owned = NUE_TRUE;
    return NUE_OK;
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_pack_entry_data(nue_pack pack, uint32_t index, const void** out_data, size_t* out_size, nue_bool* out_owned) {
    nue_pack_s* p = pk(pack);
    if (!p || !out_data || !out_size || !out_owned) return NUE_ERR_INVALID_ARGUMENT;
    return entry_bytes(p, index, out_data, out_size, out_owned);
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_entry_image(nue_pack pack, uint32_t index, nue_tex_image* out_image) {
    nue_pack_s* p = pk(pack);
    if (!p || !out_image) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= p->entries.size()) return NUE_ERR_NOT_FOUND;
    const PEntry& e = p->entries[index];
    if (e.kind != NUE_ASSET_RES_TEXTURE) return NUE_ERR_UNSUPPORTED;
    const void* data = nullptr;
    size_t size = 0;
    nue_bool owned = NUE_FALSE;
    nue_result r = entry_bytes(p, index, &data, &size, &owned);
    if (r < 0) return r;
    r = bytes_to_image(e, static_cast<const uint8_t*>(data), size, out_image);
    if (owned) nue_free(const_cast<void*>(data));
    return r;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_verify(nue_pack pack, uint32_t* out_bad) {
    nue_pack_s* p = pk(pack);
    if (!p) return NUE_ERR_INVALID_ARGUMENT;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < p->entries.size(); ++i) {
        const void* data = nullptr;
        size_t size = 0;
        nue_bool owned = NUE_FALSE;
        nue_result r = entry_bytes(p, i, &data, &size, &owned);
        bool ok = r >= 0;
        if (ok) {
            uint8_t sha[32];
            nue_sha256(data, size, sha);
            ok = std::memcmp(sha, p->entries[i].sha, 32) == 0 && nue_crc32(0, data, size) == p->entries[i].crc;
        }
        if (owned) nue_free(const_cast<void*>(data));
        if (!ok) ++bad;
    }
    if (out_bad) *out_bad = bad;
    return bad ? NUE_ERR_CORRUPT : NUE_OK;
}

// ======================================================================================================== export
NUE_ASSETS_API nue_result NUE_CALL nue_pack_export_container(nue_pack pack, nue_asset_container* out) {
    nue_pack_s* p = pk(pack);
    if (!p || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!(p->flags & kFlagTree)) { nue_set_last_error("nuepack: this package holds no container tree (it cannot be exported)"); return NUE_ERR_UNSUPPORTED; }
    Ctx* x = p->ctx;
    const FormatRec* fmt = x->find_format(p->format.c_str());
    if (!fmt) { nue_set_last_error("nuepack: format '%s' is not registered", p->format.c_str()); return NUE_ERR_NOT_FOUND; }

    std::unique_ptr<nue_asset_container_s> c(new nue_asset_container_s());
    c->ctx = x;
    container_attach_format(c.get(), fmt);
    if (!p->codec.empty()) c->codec = x->find_codec(p->codec.c_str());

    // the payloads of the tree, in tree order, in one memory block
    Buffer store;
    nue_result r = store.alloc(x, 0);
    if (r < 0) return r;
    std::vector<uint64_t> offsets(p->nodes.size(), 0);
    for (uint32_t i = 0; i < p->nodes.size(); ++i) {
        const PNode& n = p->nodes[i];
        offsets[i] = store.size();
        if (n.kind == NUE_ASSET_NODE_GROUP || (n.flags & kNodeFlagDerived)) continue;
        if (n.entry != kNoEntry) {
            const void* data = nullptr;
            size_t size = 0;
            nue_bool owned = NUE_FALSE;
            r = entry_bytes(p, n.entry, &data, &size, &owned);
            if (r < 0) return r;
            if (size != n.size) { if (owned) nue_free(const_cast<void*>(data)); return NUE_ERR_CORRUPT; }
            r = store.append(data, size);
            if (owned) nue_free(const_cast<void*>(data));
            if (r < 0) return r;
        } else if (n.size) {
            r = store.resize(store.size() + size_t(n.size));                      // zero padding stored as its size only
            if (r < 0) return r;
        }
    }
    c->file_size = store.size();
    c->content.reset(new MemSource(std::move(store)));
    for (uint32_t i = 0; i < p->nodes.size(); ++i) {
        const PNode& n = p->nodes[i];
        nue_asset_node_info ni;
        NUE_STRUCT_INIT(&ni);
        ni.parent = n.parent;
        ni.kind = n.kind;
        ni.flags = n.flags & 0xFFu;
        ni.header_size = n.header_size;
        ni.align = n.align;
        ni.id = n.id;
        ni.offset = offsets[i];
        ni.size = n.size;
        ni.name = n.name_off == kNoEntry ? nullptr : p->str(n.name_off);
        r = container_add_node(c.get(), &ni, nullptr);
        if (r < 0) return r;
    }
    r = container_build_index(c.get());
    if (r < 0) return r;
    // the textures, in the game's own encoding
    for (uint32_t i = 0; i < p->entries.size(); ++i) {
        const PEntry& e = p->entries[i];
        if (e.kind != NUE_ASSET_RES_TEXTURE || e.variant != NUE_PACK_VARIANT_SOURCE || e.tree_node == kNoEntry) continue;
        uint32_t res = NUE_ASSET_NO_NODE;
        for (auto& rr : c->res) if (rr.info.kind == NUE_ASSET_RES_TEXTURE && rr.info.node == e.tree_node && rr.info.sub_index == e.sub_index) { res = rr.info.index; break; }
        if (res == NUE_ASSET_NO_NODE) { nue_set_last_error("nuepack: texture '%s' has no place in the rebuilt container", p->str(e.name_off)); return NUE_ERR_CORRUPT; }
        const void* data = nullptr;
        size_t size = 0;
        nue_bool owned = NUE_FALSE;
        r = entry_bytes(p, i, &data, &size, &owned);
        if (r < 0) return r;
        if (e.native == 0) {                                                     // not in the game's encoding (a loose or edited image): a plain image edit
            nue_tex_image im = nullptr;
            r = bytes_to_image(e, static_cast<const uint8_t*>(data), size, &im);
            if (owned) nue_free(const_cast<void*>(data));
            if (r < 0) return r;
            r = nue_asset_resource_set_image(wrap(c.get()), res, im);
            nue_tex_image_destroy(im);
            if (r < 0) return r;
            continue;
        }
        nue_asset_native_texture nt;
        NUE_STRUCT_INIT(&nt);
        nt.native_format = e.native; nt.tex_format = e.tex_format; nt.width = e.width; nt.height = e.height; nt.depth = e.depth ? e.depth : 1; nt.mip_levels = e.mips;
        nt.flags = (e.flags & NUE_PACK_ENTRY_PALETTED) ? NUE_ASSET_RESFLAG_PALETTED : 0;
        nt.palette_size = (e.flags & NUE_PACK_ENTRY_PALETTED) ? 1024 : 0;
        nt.data = data; nt.size = size;
        r = nue_asset_resource_set_native(wrap(c.get()), res, &nt);
        if (owned) nue_free(const_cast<void*>(data));
        if (r < 0) return r;
    }
    c->dirty = true;                                                           // everything is serialised from the tree
    *out = wrap(c.release());
    return NUE_OK;
}

// ======================================================================================================== mount support
namespace nuea {
nue_file pack_backing_file(nue_pack pack) {
    nue_pack_s* p = pk(pack);
    return p ? (p->file_src ? p->file_src->handle() : p->mapped_file) : 0;
}
const void* pack_entry_peek(nue_pack pack, uint32_t index) {
    nue_pack_s* p = pk(pack);
    if (!p || index >= p->entries.size() || p->entries[index].codec != kCodecNone) return nullptr;
    return p->src ? p->src->peek(p->entries[index].data_off, size_t(p->entries[index].stored)) : nullptr;
}
}  // namespace nuea
