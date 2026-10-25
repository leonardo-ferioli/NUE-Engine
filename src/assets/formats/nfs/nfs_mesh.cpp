// NFS solids (chunk 0x80134010): the geometry of cars (GEOMETRY.BIN) and of the city (STREAM*.BUN). Layout as observed, docs/ASSET_FORMATS.md section 5.
#include "nfs.hpp"

#include <algorithm>
#include <cstring>

namespace nfs {

namespace {

constexpr uint32_t kIdSolid = 0x80134010u, kIdSolidHeader = 0x00134011u, kIdMaterials = 0x00134012u, kIdShaders = 0x00134013u;
constexpr uint32_t kIdMeshGroup = 0x80134100u, kIdMeshHeader = 0x00134900u, kIdVertices = 0x00134B01u, kIdRanges = 0x00134B02u, kIdIndices = 0x00134B03u;
constexpr uint32_t kRangeSize = 60;

struct SolidRefs {
    uint32_t header = NUE_ASSET_NO_NODE, materials = NUE_ASSET_NO_NODE, shaders = NUE_ASSET_NO_NODE, mesh = NUE_ASSET_NO_NODE;
    uint32_t vertices = NUE_ASSET_NO_NODE, ranges = NUE_ASSET_NO_NODE, indices = NUE_ASSET_NO_NODE;
};

void scan_children(nue_asset_container c, uint32_t parent, SolidRefs* r, int depth) {
    nue_asset_node_info n;
    NUE_STRUCT_INIT(&n);
    if (nue_asset_node_get(c, parent, &n) < 0) return;
    for (uint32_t i = n.first_child; i != NUE_ASSET_NO_NODE;) {
        nue_asset_node_info k;
        NUE_STRUCT_INIT(&k);
        nue_asset_node_get(c, i, &k);
        switch (k.id) {
            case kIdSolidHeader: if (r->header == NUE_ASSET_NO_NODE) r->header = i; break;
            case kIdMaterials: if (r->materials == NUE_ASSET_NO_NODE) r->materials = i; break;
            case kIdShaders: if (r->shaders == NUE_ASSET_NO_NODE) r->shaders = i; break;
            case kIdMeshHeader: if (r->mesh == NUE_ASSET_NO_NODE) r->mesh = i; break;
            case kIdVertices: if (r->vertices == NUE_ASSET_NO_NODE) r->vertices = i; break;
            case kIdRanges: if (r->ranges == NUE_ASSET_NO_NODE) r->ranges = i; break;
            case kIdIndices: if (r->indices == NUE_ASSET_NO_NODE) r->indices = i; break;
            case kIdMeshGroup: if (depth < 3) scan_children(c, i, r, depth + 1); break;
            default: break;
        }
        i = k.next_sibling;
    }
}

bool read_node(nue_asset_container c, uint32_t node, std::vector<uint8_t>* out, uint64_t max = 0xFFFFFFFFull) {
    nue_asset_node_info n;
    NUE_STRUCT_INIT(&n);
    if (node == NUE_ASSET_NO_NODE || nue_asset_node_get(c, node, &n) < 0 || n.size > max) return false;
    out->resize(size_t(n.size));
    size_t got = 0;
    if (n.size && (nue_asset_node_read(c, node, 0, out->data(), out->size(), &got) < 0 || got != out->size())) return false;
    return true;
}

// the leading 0x11 filler words some chunks start with
size_t skip_filler_words(const std::vector<uint8_t>& d) {
    size_t o = 0;
    while (o + 4 <= d.size() && d[o] == 0x11 && d[o + 1] == 0x11 && d[o + 2] == 0x11 && d[o + 3] == 0x11) o += 4;
    return o;
}

struct Counts { uint32_t triangles = 0, vertices = 0, format = 0; bool ok = false; };

Counts mesh_counts(const std::vector<uint8_t>& hdr) {
    Counts c;
    const size_t o = skip_filler_words(hdr);
    if (hdr.size() < o + 56) return c;
    c.triangles = rd32(hdr.data() + o + 36);               // u32 field 9
    c.vertices = rd32(hdr.data() + o + 52);                // u32 field 13
    c.format = rd32(hdr.data() + o + 12);                  // u32 field 3: vertex format flags, bit 0x80 = the vertices carry a normal
    c.ok = true;
    return c;
}

// The header of a solid may start with 0x11 filler words (0..3 of them in the files), which shift every field: o = their size.
// The name sits at o + 0xA4 of the header; elsewhere the longest printable run is used.
std::string solid_name(const std::vector<uint8_t>& h) {
    auto printable = [](uint8_t b) { return b > 32 && b < 127; };
    const size_t o = skip_filler_words(h), at = o + 0xA4;
    if (h.size() > at && printable(h[at])) {
        size_t e = at;
        while (e < h.size() && printable(h[e])) ++e;
        return std::string(reinterpret_cast<const char*>(h.data()) + at, e - at);
    }
    size_t best_start = 0, best_len = 0;
    for (size_t i = 0; i < h.size();) {
        if (!printable(h[i])) { ++i; continue; }
        size_t j = i;
        while (j < h.size() && printable(h[j])) ++j;
        if (j - i > best_len) { best_start = i; best_len = j - i; }
        i = j;
    }
    return std::string(reinterpret_cast<const char*>(h.data()) + best_start, best_len);
}

}  // namespace

nue_result index_meshes(nue_asset_container c, const nue_asset_resource_ops* ops, void* builder) {
    const uint32_t count = nue_asset_node_count(c);
    std::vector<uint8_t> hdr, mesh;
    for (uint32_t i = 0; i < count; ++i) {
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        nue_asset_node_get(c, i, &n);
        if (n.id != kIdSolid || n.kind != NUE_ASSET_NODE_GROUP) continue;
        SolidRefs r;
        scan_children(c, i, &r, 0);
        if (r.mesh == NUE_ASSET_NO_NODE || r.vertices == NUE_ASSET_NO_NODE || r.indices == NUE_ASSET_NO_NODE) continue;      // a dummy or mount point
        if (!read_node(c, r.mesh, &mesh, 4096)) continue;
        const Counts k = mesh_counts(mesh);
        if (!k.ok || k.vertices == 0 || k.triangles == 0) continue;
        std::string name;
        if (r.header != NUE_ASSET_NO_NODE && read_node(c, r.header, &hdr, 4096)) name = solid_name(hdr);
        nue_asset_resource_info ri;
        NUE_STRUCT_INIT(&ri);
        ri.kind = NUE_ASSET_RES_MESH;
        ri.node = i;
        ri.parent = NUE_ASSET_NO_NODE;
        ri.derived_node = NUE_ASSET_NO_NODE;
        ri.name = name.c_str();
        ri.width = k.vertices;
        ri.height = k.triangles;
        ri.depth = 1;
        ri.data_size = n.size;
        nue_result res = ops->add_resource(builder, &ri, nullptr);
        if (res < 0) return res;
    }
    return NUE_OK;
}

nue_result NUE_CALL get_mesh(void*, nue_asset_container c, void*, uint32_t resource, nue_asset_mesh* out, void** out_memory) {
    nue_asset_resource_info ri;
    NUE_STRUCT_INIT(&ri);
    nue_result r = nue_asset_resource_get(c, resource, &ri);
    if (r < 0) return r;
    SolidRefs refs;
    scan_children(c, ri.node, &refs, 0);
    std::vector<uint8_t> hdr, mesh, vb, ib, rg, mat, shd;
    if (!read_node(c, refs.mesh, &mesh, 4096) || !read_node(c, refs.vertices, &vb) || !read_node(c, refs.indices, &ib)) return NUE_ERR_CORRUPT;
    const Counts k = mesh_counts(mesh);
    if (!k.ok || k.vertices == 0 || k.triangles == 0) return NUE_ERR_CORRUPT;
    const uint64_t vc = k.vertices, ic = uint64_t(k.triangles) * 3;
    if (vc > 0x10000u) { nue_set_last_error("nfs: mesh '%s' has %llu vertices (16 bit indices)", ri.name, (unsigned long long)vc); return NUE_ERR_CORRUPT; }

    // vertex layout: flag 0x80 of header field 3 says the vertex has a normal (36 bytes: position, normal, colour, uv; cars), otherwise 24 bytes (position,
    // colour, uv; the city). The vertices are the LAST bytes of the buffer, behind 0x11 filler.
    const uint32_t stride = (k.format & 0x80u) ? 36 : 24;
    if (vc * stride > vb.size()) { nue_set_last_error("nfs: mesh '%s': vertex buffer too small for %llu vertices", ri.name, (unsigned long long)vc); return NUE_ERR_CORRUPT; }
    const size_t vbase = vb.size() - size_t(vc) * stride;

    // indices: u16, read forward after the leading filler (never more filler than the slack)
    const uint64_t need = ic * 2;
    if (need > ib.size()) return NUE_ERR_CORRUPT;
    size_t lead = 0;
    while (lead < ib.size() && ib[lead] == 0x11 && lead < ib.size() - need) ++lead;
    if (lead + need > ib.size()) return NUE_ERR_CORRUPT;

    const bool normals = stride == 36;
    const size_t sz_pos = size_t(vc) * 12, sz_nrm = normals ? size_t(vc) * 12 : 0, sz_uv = size_t(vc) * 8, sz_col = size_t(vc) * 4, sz_idx = size_t(ic) * 4;

    // material runs
    struct Run { uint32_t offset, count, matidx, shaderidx; };
    std::vector<Run> runs;
    if (refs.ranges != NUE_ASSET_NO_NODE && read_node(c, refs.ranges, &rg)) {
        const size_t n = rg.size() / kRangeSize, start = rg.size() - n * kRangeSize;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* e = rg.data() + start + i * kRangeSize;
            Run u{rd32(e + 52), rd32(e + 12), rd32(e + 28), rd32(e + 32)};
            if (u.count == 0 || u.offset > ic || u.count > ic - u.offset) continue;
            runs.push_back(u);
        }
    }
    std::vector<uint64_t> mats, shds;
    auto hashes = [&](uint32_t node, std::vector<uint64_t>* v) {
        std::vector<uint8_t> d;
        if (node != NUE_ASSET_NO_NODE && read_node(c, node, &d, 1u << 20)) for (size_t i = 0; i + 8 <= d.size(); i += 8) v->push_back(rd32(d.data() + i));
    };
    hashes(refs.materials, &mats);
    hashes(refs.shaders, &shds);
    if (runs.empty()) runs.push_back({0, uint32_t(ic), 0xFFFFFFFFu, 0xFFFFFFFFu});

    const size_t sz_runs = runs.size() * sizeof(nue_asset_mesh_run);
    std::string name = ri.name ? ri.name : "";
    const size_t total = sz_pos + sz_nrm + sz_uv + sz_col + sz_idx + sz_runs + name.size() + 1 + 64;
    uint8_t* mem = static_cast<uint8_t*>(nue_alloc(total, 16));
    if (!mem) return NUE_ERR_OUT_OF_MEMORY;
    uint8_t* cur = mem;
    auto take = [&](size_t n) { uint8_t* p = cur; cur += (n + 7) & ~size_t(7); return p; };
    float* pos = reinterpret_cast<float*>(take(sz_pos));
    float* nrm = normals ? reinterpret_cast<float*>(take(sz_nrm)) : nullptr;
    float* uv = reinterpret_cast<float*>(take(sz_uv));
    uint8_t* col = take(sz_col);
    uint32_t* idx = reinterpret_cast<uint32_t*>(take(sz_idx));
    nue_asset_mesh_run* rr = reinterpret_cast<nue_asset_mesh_run*>(take(sz_runs));
    char* nm = reinterpret_cast<char*>(take(name.size() + 1));

    float bmin[3] = {1e30f, 1e30f, 1e30f}, bmax[3] = {-1e30f, -1e30f, -1e30f};
    for (uint64_t v = 0; v < vc; ++v) {
        const uint8_t* p = vb.data() + vbase + v * stride;
        uint32_t bits[9];
        for (int q = 0; q < (stride == 36 ? 9 : 6); ++q) bits[q] = rd32(p + q * 4);
        auto fl = [](uint32_t b) { float x; std::memcpy(&x, &b, 4); return x; };
        for (int a = 0; a < 3; ++a) { pos[v * 3 + a] = fl(bits[a]); bmin[a] = std::min(bmin[a], pos[v * 3 + a]); bmax[a] = std::max(bmax[a], pos[v * 3 + a]); }
        size_t o = 12;
        if (normals) { for (int a = 0; a < 3; ++a) nrm[v * 3 + a] = fl(bits[3 + a]); o = 24; }
        col[v * 4 + 0] = p[o + 2]; col[v * 4 + 1] = p[o + 1]; col[v * 4 + 2] = p[o + 0]; col[v * 4 + 3] = p[o + 3];     // stored B,G,R,A
        uv[v * 2 + 0] = fl(rd32(p + o + 4));
        uv[v * 2 + 1] = fl(rd32(p + o + 8));
    }
    for (uint64_t i = 0; i < ic; ++i) {
        const uint32_t v = rd16(ib.data() + lead + i * 2);
        if (v >= vc) { nue_free(mem); nue_set_last_error("nfs: mesh '%s': index %u beyond %llu vertices", ri.name, v, (unsigned long long)vc); return NUE_ERR_CORRUPT; }
        idx[i] = v;
    }
    for (size_t i = 0; i < runs.size(); ++i) {
        rr[i].index_offset = runs[i].offset;
        rr[i].index_count = runs[i].count;
        rr[i].material_hash = runs[i].matidx < mats.size() ? mats[runs[i].matidx] : 0;
        rr[i].shader_hash = runs[i].shaderidx < shds.size() ? shds[runs[i].shaderidx] : 0;
    }
    std::memcpy(nm, name.c_str(), name.size() + 1);

    NUE_STRUCT_INIT(out);
    out->name = nm;
    out->flags = NUE_ASSET_MESH_UVS | NUE_ASSET_MESH_COLORS | (normals ? NUE_ASSET_MESH_NORMALS : 0);
    out->vertex_count = uint32_t(vc);
    out->index_count = uint32_t(ic);
    out->positions = pos; out->normals = nrm; out->uvs = uv; out->colors = col; out->indices = idx;
    out->run_count = uint32_t(runs.size());
    out->runs = rr;
    for (int i = 0; i < 16; ++i) out->transform[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    if (refs.header != NUE_ASSET_NO_NODE && read_node(c, refs.header, &hdr, 4096)) {
        const size_t o = skip_filler_words(hdr);
        if (hdr.size() >= o + 0x80)
            for (int i = 0; i < 16; ++i) { uint32_t b = rd32(hdr.data() + o + 0x40 + i * 4); std::memcpy(&out->transform[i], &b, 4); }
    }
    for (int a = 0; a < 3; ++a) { out->bbox_min[a] = bmin[a]; out->bbox_max[a] = bmax[a]; }
    *out_memory = mem;
    return NUE_OK;
}

}  // namespace nfs
