// Model export: the meshes of containers as one glTF binary (.glb) or Wavefront OBJ + MTL + PNG textures.
#include "internal.hpp"

#include <cinttypes>
#include <cstdio>
#include <map>
#include <set>

using namespace nuea;

namespace {

struct Part {
    nue_asset_mesh mesh;
    void* memory = nullptr;
    std::string name;
};

struct TexSrc { nue_asset_container container; uint32_t resource; std::string name; };

std::string ilower(std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; }

std::string fnum(float v) { char b[40]; std::snprintf(b, sizeof(b), "%.9g", double(v)); return b; }

std::string safe(const std::string& n) {
    std::string r;
    for (char c : n) r += (std::isalnum(uint8_t(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
    return r.empty() ? "_" : r;
}

// "BASE_A" -> "BASE" and 'A'; names without a trailing _A.._D give '\0'
std::string lod_base(const std::string& n, char* lod) {
    *lod = 0;
    if (n.size() >= 2 && n[n.size() - 2] == '_' && n.back() >= 'A' && n.back() <= 'D') { *lod = n.back(); return n.substr(0, n.size() - 2); }
    return n;
}

struct Material { std::string name; int texture = -1; bool blend = false; };

class Exporter {
public:
    Exporter(const nue_asset_model_desc& d) : d_(d) {}

    nue_result run(nue_asset_model_stats* st) {
        nue_result r = gather();
        if (r < 0) return r;
        if (parts_.empty()) { nue_set_last_error("assets: no meshes to export"); return NUE_ERR_NOT_FOUND; }
        r = d_.format == NUE_ASSET_MODEL_OBJ ? write_obj() : write_glb();
        if (r < 0) return r;
        if (st) {
            st->meshes = uint32_t(parts_.size());
            st->textures = uint32_t(images_.size());
            st->meshes_skipped = skipped_;
            for (auto& p : parts_) { st->vertices += p.mesh.vertex_count; st->triangles += p.mesh.index_count / 3; }
            st->bytes_written = written_;
        }
        return NUE_OK;
    }

    ~Exporter() { for (auto& p : parts_) if (p.memory) nue_free(p.memory); for (auto& i : images_) (void)i; }

private:
    const nue_asset_model_desc& d_;
    std::vector<Part> parts_;
    std::map<uint64_t, TexSrc> textures_;
    uint32_t skipped_ = 0;
    uint64_t written_ = 0;
    // materials and images shared by all meshes
    std::vector<Material> materials_;
    std::map<uint64_t, int> material_of_hash_;
    std::vector<std::vector<uint8_t>> images_;               // PNG bytes
    std::vector<std::string> image_names_;

    nue_result gather() {
        struct Cand { nue_asset_container c; uint32_t res; std::string name; };
        std::vector<Cand> cands;
        const std::string filter = d_.name_filter ? ilower(d_.name_filter) : "";
        for (uint32_t ci = 0; ci < d_.container_count; ++ci) {
            nue_asset_container c = d_.containers[ci];
            for (uint32_t i = 0, n = nue_asset_resource_count(c); i < n; ++i) {
                nue_asset_resource_info ri;
                NUE_STRUCT_INIT(&ri);
                nue_asset_resource_get(c, i, &ri);
                if (ri.kind == NUE_ASSET_RES_TEXTURE && !textures_.count(ri.hash)) textures_[ri.hash] = {c, i, ri.name ? ri.name : ""};
                if (ri.kind != NUE_ASSET_RES_MESH) continue;
                std::string name = ri.name ? ri.name : "";
                if (!filter.empty() && ilower(name).find(filter) == std::string::npos) continue;
                cands.push_back({c, i, name});
            }
        }
        if (d_.flags & NUE_ASSET_MODEL_BEST_LOD) {
            std::map<std::string, char> best;
            for (auto& k : cands) { char l; std::string b = lod_base(k.name, &l); if (!l) continue; auto it = best.find(b); if (it == best.end() || l < it->second) best[b] = l; }
            std::vector<Cand> kept;
            for (auto& k : cands) { char l; std::string b = lod_base(k.name, &l); if (!l || best[b] == l) kept.push_back(k); }
            cands.swap(kept);
        }
        for (auto& k : cands) {
            if (d_.max_meshes && parts_.size() >= d_.max_meshes) break;
            Part p;
            nue_result r = nue_asset_resource_get_mesh(k.c, k.res, &p.mesh, &p.memory);
            if (r < 0) { ++skipped_; continue; }
            p.name = k.name.empty() ? "mesh" : k.name;
            p.mesh.name = nullptr;
            parts_.push_back(std::move(p));
        }
        return NUE_OK;
    }

    // material index for a run (creates it, and its texture, on first use)
    int material_for(uint64_t hash) {
        auto it = material_of_hash_.find(hash);
        if (it != material_of_hash_.end()) return it->second;
        Material m;
        char nm[40];
        std::snprintf(nm, sizeof(nm), "mat_%08" PRIx64, hash);
        m.name = nm;
        auto t = textures_.find(hash);
        if (!(d_.flags & NUE_ASSET_MODEL_NO_TEXTURES) && hash && t != textures_.end()) {
            nue_tex_image im = nullptr, rgba = nullptr;
            if (nue_asset_resource_get_image(t->second.container, t->second.resource, &im) >= 0) {
                nue_tex_encode_params ep;
                nue_tex_encode_params_default(&ep);
                if (nue_tex_image_convert(im, NUE_TEX_RGBA8_UNORM, 1, &ep, nullptr, &rgba) >= 0) {
                    nue_tex_level_view v;
                    NUE_STRUCT_INIT(&v);
                    nue_tex_image_get_level(rgba, 0, 0, 0, &v);
                    const uint8_t* px = static_cast<const uint8_t*>(v.data);
                    for (uint64_t i = 0; i < uint64_t(v.width) * v.height; ++i) if (px[i * 4 + 3] < 250) { m.blend = true; break; }
                    void* png = nullptr;
                    size_t n = 0;
                    if (nue_tex_image_save_memory(rgba, NUE_TEX_FILE_PNG, 0, &png, &n) >= 0) {
                        images_.emplace_back(static_cast<uint8_t*>(png), static_cast<uint8_t*>(png) + n);
                        image_names_.push_back(safe(t->second.name) + "_" + (nm + 4));
                        m.texture = int(images_.size() - 1);
                        nue_tex_free(png);
                    }
                    nue_tex_image_destroy(rgba);
                }
                nue_tex_image_destroy(im);
            }
        }
        materials_.push_back(m);
        material_of_hash_[hash] = int(materials_.size() - 1);
        return int(materials_.size() - 1);
    }

    // ------------------------------------------------------------------------------------------------ glTF binary
    static void pad4(std::vector<uint8_t>& v, uint8_t fill = 0) { while (v.size() % 4) v.push_back(fill); }

    nue_result write_glb() {
        std::vector<uint8_t> bin;
        std::string views, accessors, meshes, nodes, nodelist;
        int view_count = 0, accessor_count = 0;
        auto add_view = [&](const void* data, size_t size, int target) {
            pad4(bin);
            char b[160];
            std::snprintf(b, sizeof(b), "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu%s}", view_count ? "," : "", bin.size(), size, target ? (target == 1 ? ",\"target\":34962" : ",\"target\":34963") : "");
            views += b;
            bin.insert(bin.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            return view_count++;
        };
        auto add_accessor = [&](int view, size_t offset, int comp, uint32_t count, const char* type, bool normalized, const std::string& minmax) {
            std::string a = accessor_count ? "," : "";
            a += "{\"bufferView\":" + std::to_string(view) + ",\"byteOffset\":" + std::to_string(offset) + ",\"componentType\":" + std::to_string(comp) + ",\"count\":" + std::to_string(count) +
                 ",\"type\":\"" + type + "\"" + (normalized ? ",\"normalized\":true" : "") + minmax + "}";
            accessors += a;
            return accessor_count++;
        };
        for (size_t pi = 0; pi < parts_.size(); ++pi) {
            const nue_asset_mesh& m = parts_[pi].mesh;
            const int vp = add_view(m.positions, size_t(m.vertex_count) * 12, 1);
            std::string mm = ",\"min\":[" + fnum(m.bbox_min[0]) + "," + fnum(m.bbox_min[1]) + "," + fnum(m.bbox_min[2]) + "],\"max\":[" + fnum(m.bbox_max[0]) + "," + fnum(m.bbox_max[1]) + "," + fnum(m.bbox_max[2]) + "]";
            const int a_pos = add_accessor(vp, 0, 5126, m.vertex_count, "VEC3", false, mm);
            int a_nrm = -1, a_uv = -1, a_col = -1;
            if (m.normals) a_nrm = add_accessor(add_view(m.normals, size_t(m.vertex_count) * 12, 1), 0, 5126, m.vertex_count, "VEC3", false, "");
            if (m.uvs) a_uv = add_accessor(add_view(m.uvs, size_t(m.vertex_count) * 8, 1), 0, 5126, m.vertex_count, "VEC2", false, "");
            if (m.colors) {                                                      // vertex colours only when they carry information
                bool varied = false;
                for (uint32_t v = 1; v < m.vertex_count && !varied; ++v) varied = std::memcmp(m.colors, m.colors + size_t(v) * 4, 4) != 0;
                if (varied) a_col = add_accessor(add_view(m.colors, size_t(m.vertex_count) * 4, 1), 0, 5121, m.vertex_count, "VEC4", true, "");
            }
            const int vi = add_view(m.indices, size_t(m.index_count) * 4, 2);
            std::string prims;
            for (uint32_t k = 0; k < m.run_count; ++k) {
                const nue_asset_mesh_run& run = m.runs[k];
                const int a_idx = add_accessor(vi, size_t(run.index_offset) * 4, 5125, run.index_count, "SCALAR", false, "");
                std::string pr = k ? "," : "";
                pr += "{\"attributes\":{\"POSITION\":" + std::to_string(a_pos);
                if (a_nrm >= 0) pr += ",\"NORMAL\":" + std::to_string(a_nrm);
                if (a_uv >= 0) pr += ",\"TEXCOORD_0\":" + std::to_string(a_uv);
                if (a_col >= 0) pr += ",\"COLOR_0\":" + std::to_string(a_col);
                pr += "},\"indices\":" + std::to_string(a_idx) + ",\"material\":" + std::to_string(material_for(run.material_hash)) + ",\"mode\":4}";
                prims += pr;
            }
            meshes += std::string(pi ? "," : "") + "{\"name\":\"" + json_escape(parts_[pi].name) + "\",\"primitives\":[" + prims + "]}";
            std::string node = std::string(pi ? "," : "") + "{\"name\":\"" + json_escape(parts_[pi].name) + "\",\"mesh\":" + std::to_string(pi);
            bool identity = true;
            for (int i = 0; i < 16; ++i) if (m.transform[i] != ((i % 5 == 0) ? 1.0f : 0.0f)) identity = false;
            if (!identity) { node += ",\"matrix\":["; for (int i = 0; i < 16; ++i) node += (i ? "," : "") + fnum(m.transform[i]); node += "]"; }
            nodes += node + "}";
            nodelist += (pi ? "," : "") + std::to_string(pi + 1);
        }
        // images
        std::string images, textures, mats;
        for (size_t i = 0; i < images_.size(); ++i) {
            const int v = add_view(images_[i].data(), images_[i].size(), 0);
            images += std::string(i ? "," : "") + "{\"bufferView\":" + std::to_string(v) + ",\"mimeType\":\"image/png\",\"name\":\"" + json_escape(image_names_[i]) + "\"}";
            textures += std::string(i ? "," : "") + "{\"source\":" + std::to_string(i) + "}";
        }
        for (size_t i = 0; i < materials_.size(); ++i) {
            const Material& m = materials_[i];
            std::string x = std::string(i ? "," : "") + "{\"name\":\"" + m.name + "\",\"doubleSided\":true,\"pbrMetallicRoughness\":{";
            if (m.texture >= 0) x += "\"baseColorTexture\":{\"index\":" + std::to_string(m.texture) + "},";
            x += "\"metallicFactor\":0,\"roughnessFactor\":1}";
            if (m.blend) x += ",\"alphaMode\":\"BLEND\"";
            mats += x + "}";
        }
        pad4(bin);
        // node 0 is the root (Z up -> Y up), the parts hang below it
        std::string root = "{\"name\":\"root\",\"children\":[" + nodelist + "]";
        if (!(d_.flags & NUE_ASSET_MODEL_KEEP_Z_UP)) root += ",\"rotation\":[-0.70710678,0,0,0.70710678]";
        root += "}";
        std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"nue_pack\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + root + "," + nodes + "],\"meshes\":[" + meshes +
                           "],\"materials\":[" + mats + "],\"accessors\":[" + accessors + "],\"bufferViews\":[" + views + "],\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]";
        if (!images.empty()) json += ",\"images\":[" + images + "],\"textures\":[" + textures + "]";
        json += "}";
        while (json.size() % 4) json.push_back(' ');
        std::vector<uint8_t> out;
        auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
        p32(0x46546C67u); p32(2); p32(uint32_t(12 + 8 + json.size() + 8 + bin.size()));
        p32(uint32_t(json.size())); p32(0x4E4F534Au);
        out.insert(out.end(), json.begin(), json.end());
        p32(uint32_t(bin.size())); p32(0x004E4942u);
        out.insert(out.end(), bin.begin(), bin.end());
        written_ = out.size();
        return nue_file_save(d_.path, out.data(), out.size());
    }

    static std::string json_escape(const std::string& s) {
        std::string r;
        for (char c : s) { if (c == '"' || c == '\\') { r += '\\'; r += c; } else if (uint8_t(c) < 32) r += '_'; else r += c; }
        return r;
    }

    // ------------------------------------------------------------------------------------------------ OBJ
    nue_result write_obj() {
        std::string path = d_.path;
        const size_t dot = path.rfind('.');
        const std::string stem = dot == std::string::npos ? path : path.substr(0, dot);
        size_t slash = stem.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? stem : stem.substr(slash + 1);
        const bool zup = (d_.flags & NUE_ASSET_MODEL_KEEP_Z_UP) != 0;
        std::string obj = "# exported by nue_pack\nmtllib " + base + ".mtl\n";
        uint64_t voff = 1;
        char b[160];
        for (auto& part : parts_) {
            const nue_asset_mesh& m = part.mesh;
            obj += "o " + safe(part.name) + "\n";
            const float* t = m.transform;                                   // OBJ has no node transforms: the solid's matrix (row vectors, translation in the last row) is applied here
            for (uint32_t v = 0; v < m.vertex_count; ++v) {
                const float* q = m.positions + v * 3;
                const float p[3] = {q[0] * t[0] + q[1] * t[4] + q[2] * t[8] + t[12], q[0] * t[1] + q[1] * t[5] + q[2] * t[9] + t[13], q[0] * t[2] + q[1] * t[6] + q[2] * t[10] + t[14]};
                if (zup) std::snprintf(b, sizeof(b), "v %s %s %s\n", fnum(p[0]).c_str(), fnum(p[1]).c_str(), fnum(p[2]).c_str());
                else std::snprintf(b, sizeof(b), "v %s %s %s\n", fnum(p[0]).c_str(), fnum(p[2]).c_str(), fnum(-p[1]).c_str());      // Z up -> Y up
                obj += b;
            }
            if (m.uvs) for (uint32_t v = 0; v < m.vertex_count; ++v) { std::snprintf(b, sizeof(b), "vt %s %s\n", fnum(m.uvs[v * 2]).c_str(), fnum(1.0f - m.uvs[v * 2 + 1]).c_str()); obj += b; }
            if (m.normals) for (uint32_t v = 0; v < m.vertex_count; ++v) {
                const float* q = m.normals + v * 3;
                const float n[3] = {q[0] * t[0] + q[1] * t[4] + q[2] * t[8], q[0] * t[1] + q[1] * t[5] + q[2] * t[9], q[0] * t[2] + q[1] * t[6] + q[2] * t[10]};
                if (zup) std::snprintf(b, sizeof(b), "vn %s %s %s\n", fnum(n[0]).c_str(), fnum(n[1]).c_str(), fnum(n[2]).c_str());
                else std::snprintf(b, sizeof(b), "vn %s %s %s\n", fnum(n[0]).c_str(), fnum(n[2]).c_str(), fnum(-n[1]).c_str());
                obj += b;
            }
            for (uint32_t k = 0; k < m.run_count; ++k) {
                const nue_asset_mesh_run& run = m.runs[k];
                obj += "usemtl " + materials_[size_t(material_for(run.material_hash))].name + "\n";
                for (uint32_t i = run.index_offset; i + 3 <= run.index_offset + run.index_count && i + 3 <= m.index_count; i += 3) {
                    std::string f = "f";
                    for (int q = 0; q < 3; ++q) {
                        const uint64_t v = voff + m.indices[i + q];
                        f += " " + std::to_string(v);
                        if (m.uvs && m.normals) f += "/" + std::to_string(v) + "/" + std::to_string(v);
                        else if (m.uvs) f += "/" + std::to_string(v);
                        else if (m.normals) f += "//" + std::to_string(v);
                    }
                    obj += f + "\n";
                }
            }
            voff += m.vertex_count;
        }
        std::string mtl = "# exported by nue_pack\n";
        const std::string tex_dir = base + "_textures";
        for (auto& m : materials_) {
            mtl += "newmtl " + m.name + "\nKd 1 1 1\n";
            if (m.texture >= 0) mtl += "map_Kd " + tex_dir + "/" + image_names_[size_t(m.texture)] + ".png\n";
            if (m.blend && m.texture >= 0) mtl += "map_d " + tex_dir + "/" + image_names_[size_t(m.texture)] + ".png\n";
        }
        const std::string dir = slash == std::string::npos ? std::string() : stem.substr(0, slash + 1);
        nue_result r = nue_file_save(d_.path, obj.data(), obj.size());
        if (r < 0) return r;
        r = nue_file_save((stem + ".mtl").c_str(), mtl.data(), mtl.size());
        if (r < 0) return r;
        written_ = obj.size() + mtl.size();
        if (!images_.empty()) {
            nue_vfs_create_directory((dir + tex_dir).c_str());
            for (size_t i = 0; i < images_.size(); ++i) {
                r = nue_file_save((dir + tex_dir + "/" + image_names_[i] + ".png").c_str(), images_[i].data(), images_[i].size());
                if (r < 0) return r;
                written_ += images_[i].size();
            }
        }
        return NUE_OK;
    }
};

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_asset_export_model(const nue_asset_model_desc* desc, nue_asset_model_stats* out_stats) {
    if (!desc || !desc->path || !desc->containers || desc->container_count == 0) return NUE_ERR_INVALID_ARGUMENT;
    if (desc->format != NUE_ASSET_MODEL_GLB && desc->format != NUE_ASSET_MODEL_OBJ) return NUE_ERR_INVALID_ARGUMENT;
    if (out_stats) { uint32_t sz = out_stats->struct_size; std::memset(out_stats, 0, sizeof(*out_stats)); out_stats->struct_size = sz ? sz : uint32_t(sizeof(*out_stats)); }
    Exporter e(*desc);
    return e.run(out_stats);
}
