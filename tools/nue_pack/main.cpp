// nue_pack - open, list, extract, convert and pack the resource files of a game, and build / read / export .nuepack packages.
//
//   nue_pack info <file>                              what a .BUN / .LZC / .nuepack is (format, codec, sizes, counts)
//   nue_pack list <file> [--nodes] [--textures]       resources (default), the chunk tree (--nodes), or only textures
//   nue_pack extract <file> -o DIR [--textures] [--raw] [--format dds|png|ktx2] [--filter TEXT]
//                                                     textures as DDS/PNG/KTX2 (default dds), raw chunk payloads (--raw)
//   nue_pack convert <in.bun|in.lzc> -o OUT.nuepack [--target LIST] [--quality Q] [--max-dim N] [--threads N] [--no-compress]
//                                                     LIST = bc,astc,etc2,pvrtc,rgba8 | all | a device class (desktop_bc, android_astc, apple_astc ...)
//   nue_pack pack OUT.nuepack <files or dirs...> [--target LIST] [--quality Q]
//                                                     loose files into a package; .dds/.png/.ktx2 become textures with variants, the rest blobs
//   nue_pack export <in.nuepack> -o OUT.bun|OUT.lzc [--level N]
//                                                     rebuild the game container from a package that kept its tree
//   nue_pack repack <in.bun|in.lzc> -o OUT --replace NAME=FILE [--replace NAME=FILE ...] [--replace-dir DIR] [--level N]
//                                                     put new textures (DDS/PNG/KTX2) into a game file; offsets, sizes and padding are rebuilt
//   nue_pack verify <file>                            .BUN/.LZC: open, rebuild and compare, every texture decodes; .nuepack: every hash
//   nue_pack jdlz c|d IN OUT [--level N]              the JDLZ codec on its own
//
// Names: NAME is a texture name ("TEX" or "PACK/TEX", case-insensitive) or 0xHASH. Nothing here sends data anywhere; game files stay where they are.
#include "nue/assets.h"
#include "nue/core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::vector<std::string>> opt;
    bool flag(const char* n) const { return opt.count(n) != 0; }
    const char* get(const char* n) const { auto it = opt.find(n); return it == opt.end() || it->second.empty() ? nullptr : it->second.back().c_str(); }
};

bool is_flag_only(const std::string& a) { return a == "--best-lod" || a == "--keep-z-up" || a == "--no-textures" || a == "--meshes" || a == "--model" || a == "--nodes" || a == "--textures" || a == "--raw" || a == "--no-compress" || a == "--help" || a == "-h" || a == "--all"; }

Args parse(int argc, char** argv, int first) {
    Args a;
    for (int i = first; i < argc; ++i) {
        std::string s = argv[i];
        if (s.size() > 1 && s[0] == '-' && !(s.size() > 1 && std::isdigit(uint8_t(s[1])))) {
            if (is_flag_only(s) || i + 1 >= argc) a.opt[s];
            else a.opt[s].push_back(argv[++i]);
        } else a.pos.push_back(s);
    }
    return a;
}

std::string safe_name(const std::string& n) {
    std::string r;
    for (char c : n) r += (std::strchr("<>:\"/\\|?*", c) || uint8_t(c) < 32) ? '_' : c;
    if (r.empty()) r = "_";
    return r;
}

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; }

bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const fs::path& p, const void* d, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(static_cast<const char*>(d), std::streamsize(n));
    return bool(f);
}

const char* tex_name(uint32_t f) { return nue_tex_format_name(nue_tex_format(f)); }

int fail(const char* what, nue_result r) {
    std::fprintf(stderr, "error: %s (%s%s%s)\n", what, nue_result_name(r), nue_get_last_error_message()[0] ? ": " : "", nue_get_last_error_message());
    return 1;
}

struct Engine {
    bool ok = false;
    std::string dir;
    Engine() {
        std::error_code ec;
        dir = (fs::temp_directory_path(ec) / "nue_pack_tool").generic_string();
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.app_name = "nue_pack";
        d.app_id = "nue-pack";
        d.org_name = "NUE";
        d.backend = NUE_BACKEND_NULL;
        d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CONFIG_LOAD;
        d.user_data_path = dir.c_str();
        d.log_level = NUE_LOG_WARN;
        ok = nue_init(&d) >= 0;
    }
    ~Engine() { if (ok) nue_shutdown(); }
};

nue_tex_quality parse_quality(const char* s) {
    if (!s) return NUE_TEX_QUALITY_NORMAL;
    std::string q = lower(s);
    if (q == "fastest") return NUE_TEX_QUALITY_FASTEST;
    if (q == "fast") return NUE_TEX_QUALITY_FAST;
    if (q == "high") return NUE_TEX_QUALITY_HIGH;
    if (q == "best") return NUE_TEX_QUALITY_BEST;
    return NUE_TEX_QUALITY_NORMAL;
}

// "bc,astc" / "all" / a device class name -> variant bit mask
bool parse_targets(const char* list, uint32_t* mask) {
    *mask = 0;
    if (!list) return true;
    std::string s = lower(list);
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        std::string t = s.substr(i, j - i);
        i = j + 1;
        if (t.empty()) continue;
        if (t == "all") { for (uint32_t v = NUE_PACK_VARIANT_BC; v < NUE_PACK_VARIANT_COUNT; ++v) *mask |= 1u << v; continue; }
        if (t == "none" || t == "source") continue;
        nue_pack_variant v = nue_pack_variant_from_name(t.c_str());
        if (v == NUE_PACK_VARIANT_COUNT) {
            nue_tex_device_class dc = nue_tex_device_class_from_name(t.c_str());
            if (dc == NUE_TEX_DEVICE_SOFTWARE && t != "software") { std::fprintf(stderr, "unknown target '%s' (variants: source bc astc etc2 pvrtc rgba8, or a device class)\n", t.c_str()); return false; }
            v = nue_pack_variant_for_device_class(dc);
        }
        *mask |= 1u << v;
    }
    return true;
}

struct Session {
    Engine engine;
    nue_assets_context ctx = nullptr;
    Session() { if (engine.ok) nue_assets_context_create(nullptr, &ctx); }
    ~Session() { nue_assets_context_destroy(ctx); }
};

bool open_container(Session& s, const std::string& path, nue_asset_container* out, uint32_t flags = 0) {
    nue_asset_open_desc d;
    NUE_STRUCT_INIT(&d);
    d.path = path.c_str();
    d.flags = flags;
    nue_result r = nue_asset_container_open(s.ctx, &d, out);
    if (r < 0) { fail(("cannot open " + path).c_str(), r); return false; }
    return true;
}

bool is_pack_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t m[4] = {0};
    size_t n = std::fread(m, 1, 4, f);
    std::fclose(f);
    return n == 4 && std::memcmp(m, "NUEP", 4) == 0;
}

std::string res_label(nue_asset_container c, const nue_asset_resource_info& r) {
    if (r.parent != NUE_ASSET_NO_NODE) {
        nue_asset_resource_info p;
        NUE_STRUCT_INIT(&p);
        nue_asset_resource_get(c, r.parent, &p);
        if (p.name && p.name[0]) return std::string(p.name) + "/" + r.name;
    }
    return r.name ? r.name : "";
}

// ---------------------------------------------------------------------------------------------------- info / list
int cmd_info(Args& a) {
    if (a.pos.empty()) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    const std::string& path = a.pos[0];
    if (is_pack_file(path)) {
        nue_pack p = nullptr;
        nue_result r = nue_pack_open_file(s.ctx, path.c_str(), 0, &p);
        if (r < 0) return fail("cannot open package", r);
        nue_pack_info i;
        NUE_STRUCT_INIT(&i);
        nue_pack_get_info(p, &i);
        std::printf("%s\n  nuepack %u.%u, %llu bytes, %u entries, tree %s (%u nodes)\n  source '%s', game format '%s', codec '%s'\n  variants:", path.c_str(), i.version_major, i.version_minor,
                    (unsigned long long)i.total_size, i.entry_count, i.has_tree ? "kept" : "none", i.tree_node_count, i.source_name, i.format_name, i.codec_name);
        for (uint32_t v = 0; v < NUE_PACK_VARIANT_COUNT; ++v) if (i.variant_mask & (1u << v)) std::printf(" %s", nue_pack_variant_name(nue_pack_variant(v)));
        std::printf("\n");
        nue_pack_close(p);
        return 0;
    }
    nue_asset_container c = nullptr;
    if (!open_container(s, path, &c)) return 1;
    nue_asset_container_info i;
    NUE_STRUCT_INIT(&i);
    nue_asset_container_get_info(c, &i);
    uint32_t packs = 0, tex = 0;
    uint64_t tex_bytes = 0;
    for (uint32_t k = 0; k < nue_asset_resource_count(c); ++k) {
        nue_asset_resource_info r;
        NUE_STRUCT_INIT(&r);
        nue_asset_resource_get(c, k, &r);
        if (r.kind == NUE_ASSET_RES_TEXTURE_PACK) ++packs;
        if (r.kind == NUE_ASSET_RES_TEXTURE) { ++tex; tex_bytes += r.data_size; }
    }
    std::printf("%s\n  format %s%s%s, %llu bytes on disk, %llu bytes of content\n  %u chunks, %u texture packs, %u textures (%.2f MiB of pixels)%s\n", path.c_str(), i.format, i.codec ? ", wrapped in " : "",
                i.codec ? i.codec : "", (unsigned long long)i.file_size, (unsigned long long)i.content_size, i.node_count, packs, tex, tex_bytes / 1048576.0, i.lenient_repairs ? "  (damaged parts kept as opaque bytes)" : "");
    nue_asset_container_close(c);
    return 0;
}

int cmd_list(Args& a) {
    if (a.pos.empty()) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    const std::string& path = a.pos[0];
    if (is_pack_file(path)) {
        nue_pack p = nullptr;
        nue_result r = nue_pack_open_file(s.ctx, path.c_str(), 0, &p);
        if (r < 0) return fail("cannot open package", r);
        for (uint32_t i = 0; i < nue_pack_entry_count(p); ++i) {
            nue_pack_entry_info e;
            NUE_STRUCT_INIT(&e);
            nue_pack_entry_get(p, i, &e);
            if (a.flag("--textures") && e.kind != NUE_ASSET_RES_TEXTURE) continue;
            if (e.kind == NUE_ASSET_RES_TEXTURE) std::printf("%5u  %-7s %-34s %5ux%-5u %-18s mips %-2u %10llu B\n", i, nue_pack_variant_name(nue_pack_variant(e.variant)), e.name, e.width, e.height, tex_name(e.tex_format), e.mip_levels, (unsigned long long)e.raw_size);
            else std::printf("%5u  blob    %-34s %10llu B (stored %llu%s)\n", i, e.name, (unsigned long long)e.raw_size, (unsigned long long)e.stored_size, e.codec ? ", jdlz" : "");
        }
        nue_pack_close(p);
        return 0;
    }
    nue_asset_container c = nullptr;
    if (!open_container(s, path, &c)) return 1;
    if (a.flag("--nodes")) {
        for (uint32_t i = 0; i < nue_asset_node_count(c); ++i) {
            nue_asset_node_info n;
            NUE_STRUCT_INIT(&n);
            nue_asset_node_get(c, i, &n);
            static const char* kinds[] = {"blob", "group", "pad", "tail", "derived"};
            std::printf("%*s#%u  id %08llx  %-5s  offset %llu  size %llu%s\n", int(n.depth) * 2, "", i, (unsigned long long)n.id, kinds[n.kind < 5 ? n.kind : 0], (unsigned long long)n.offset,
                        (unsigned long long)n.size, (n.flags & NUE_ASSET_NODE_OPAQUE) ? "  (opaque)" : "");
        }
    } else {
        for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
            nue_asset_resource_info r;
            NUE_STRUCT_INIT(&r);
            nue_asset_resource_get(c, i, &r);
            if (r.kind == NUE_ASSET_RES_TEXTURE_PACK) {
                if (a.flag("--textures")) continue;
                std::printf("pack  %-32s hash %08llx  %llu bytes%s\n", r.name, (unsigned long long)r.hash, (unsigned long long)r.data_size, r.derived_node == NUE_ASSET_NO_NODE ? "  (read only: kept as it is)" : "");
            } else if (r.kind == NUE_ASSET_RES_MESH) {
                if (a.flag("--textures")) continue;
                std::printf("  mesh %-40s %6u vertices %6u triangles\n", r.name, r.width, r.height);
            } else if (r.kind == NUE_ASSET_RES_TEXTURE) {
                if (a.flag("--meshes")) continue;
                std::printf("  tex %-26s %5ux%-5u %-12s mips %-2u hash %08llx native 0x%02x%s\n", r.name, r.width, r.height, tex_name(r.format), r.mip_levels, (unsigned long long)r.hash, r.native_format, (r.flags & NUE_ASSET_RESFLAG_PALETTED) ? " (palette)" : "");
            }
        }
    }
    nue_asset_container_close(c);
    return 0;
}

// ---------------------------------------------------------------------------------------------------- extract
// extract --model: the meshes of a file (or of a car folder: GEOMETRY.BIN + TEXTURES.BIN) as one .glb / .obj with its textures
int cmd_model(Args& a) {
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    std::vector<std::string> inputs;
    std::error_code ec;
    for (const std::string& p : a.pos) {
        if (fs::is_directory(p, ec)) {
            // a car folder: every .BIN that is a chunk stream, geometry first (VINYLS.BIN is skipped: it holds thousands of decals)
            std::vector<std::string> found;
            for (auto& e : fs::directory_iterator(p, ec)) {
                const std::string ext = lower(e.path().extension().string()), nm = lower(e.path().filename().string());
                if (!e.is_regular_file() || (ext != ".bin" && ext != ".bun" && ext != ".lzc") || nm == "vinyls.bin") continue;
                found.push_back(e.path().generic_string());
            }
            std::sort(found.begin(), found.end());
            inputs.insert(inputs.end(), found.begin(), found.end());
        } else {
            inputs.push_back(p);
        }
    }
    if (inputs.empty()) return 2;
    std::vector<nue_asset_container> containers;
    for (const std::string& in : inputs) {
        nue_asset_container c = nullptr;
        nue_asset_open_desc d;
        NUE_STRUCT_INIT(&d);
        d.path = in.c_str();
        d.flags = NUE_ASSET_OPEN_LENIENT;
        nue_result r = nue_asset_container_open(s.ctx, &d, &c);
        if (r < 0) { std::fprintf(stderr, "skipping %s (%s)\n", in.c_str(), nue_result_name(r)); continue; }
        containers.push_back(c);
    }
    const std::string fmt = a.get("--format") ? lower(a.get("--format")) : "glb";
    fs::path first = fs::path(a.pos[0]);
    std::string stem = fs::is_directory(first, ec) ? first.filename().string() : first.stem().string();
    if (stem.empty() || stem == ".") stem = fs::absolute(first, ec).parent_path().filename().string();
    const fs::path out = fs::path(a.get("-o")) / (safe_name(stem) + (fmt == "obj" ? ".obj" : ".glb"));
    fs::create_directories(out.parent_path(), ec);
    const std::string out_path = out.generic_string();
    nue_asset_model_desc md;
    NUE_STRUCT_INIT(&md);
    md.containers = containers.data();
    md.container_count = uint32_t(containers.size());
    md.format = fmt == "obj" ? NUE_ASSET_MODEL_OBJ : NUE_ASSET_MODEL_GLB;
    if (a.flag("--no-textures")) md.flags |= NUE_ASSET_MODEL_NO_TEXTURES;
    if (a.flag("--keep-z-up")) md.flags |= NUE_ASSET_MODEL_KEEP_Z_UP;
    if (a.flag("--best-lod")) md.flags |= NUE_ASSET_MODEL_BEST_LOD;
    md.name_filter = a.get("--filter");
    md.max_meshes = a.get("--max-meshes") ? uint32_t(std::atoi(a.get("--max-meshes"))) : 0;
    md.path = out_path.c_str();
    nue_asset_model_stats st;
    NUE_STRUCT_INIT(&st);
    const nue_result r = nue_asset_export_model(&md, &st);
    for (auto c : containers) nue_asset_container_close(c);
    if (r < 0) return fail("model export failed", r);
    std::printf("%s: %u meshes (%llu vertices, %llu triangles), %u textures, %llu bytes%s\n", out_path.c_str(), st.meshes, (unsigned long long)st.vertices, (unsigned long long)st.triangles, st.textures,
                (unsigned long long)st.bytes_written, st.meshes_skipped ? (", " + std::to_string(st.meshes_skipped) + " meshes could not be read").c_str() : "");
    return 0;
}

int cmd_extract(Args& a) {
    if (a.flag("--model")) return a.pos.empty() || !a.get("-o") ? 2 : cmd_model(a);
    if (a.pos.empty() || !a.get("-o")) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    const fs::path out = a.get("-o");
    const std::string filter = a.get("--filter") ? lower(a.get("--filter")) : "";
    const std::string fmt = a.get("--format") ? lower(a.get("--format")) : "dds";
    nue_tex_file_kind kind = fmt == "png" ? NUE_TEX_FILE_PNG : fmt == "ktx2" ? NUE_TEX_FILE_KTX2 : NUE_TEX_FILE_DDS;
    const std::string& path = a.pos[0];
    unsigned written = 0, failed = 0;
    if (is_pack_file(path)) {
        nue_pack p = nullptr;
        nue_result r = nue_pack_open_file(s.ctx, path.c_str(), 0, &p);
        if (r < 0) return fail("cannot open package", r);
        const bool all_variants = a.flag("--all");
        for (uint32_t i = 0; i < nue_pack_entry_count(p); ++i) {
            nue_pack_entry_info e;
            NUE_STRUCT_INIT(&e);
            nue_pack_entry_get(p, i, &e);
            if (!filter.empty() && lower(e.name).find(filter) == std::string::npos) continue;
            if (e.kind == NUE_ASSET_RES_TEXTURE) {
                if (!all_variants && e.variant != NUE_PACK_VARIANT_SOURCE) continue;
                nue_tex_image im = nullptr;
                if (nue_pack_entry_image(p, i, &im) < 0) { ++failed; continue; }
                fs::path f = out / (safe_name(e.name) + (all_variants ? std::string(".") + nue_pack_variant_name(nue_pack_variant(e.variant)) : "") + "." + fmt);
                std::error_code ec;
                fs::create_directories(f.parent_path(), ec);
                if (nue_tex_image_save_file(im, f.generic_string().c_str(), kind, 0) >= 0) ++written; else ++failed;
                nue_tex_image_destroy(im);
            } else if (a.flag("--raw") && !a.flag("--textures")) {
                const void* d = nullptr; size_t n = 0; nue_bool owned = NUE_FALSE;
                if (nue_pack_entry_data(p, i, &d, &n, &owned) >= 0) { if (write_file(out / "raw" / safe_name(e.name), d, n)) ++written; if (owned) nue_free(const_cast<void*>(d)); }
            }
        }
        nue_pack_close(p);
        std::printf("%u files written to %s (%u failed)\n", written, out.string().c_str(), failed);
        return failed ? 1 : 0;
    }
    nue_asset_container c = nullptr;
    if (!open_container(s, path, &c)) return 1;
    std::map<std::string, int> used;
    {
        for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
            nue_asset_resource_info r;
            NUE_STRUCT_INIT(&r);
            nue_asset_resource_get(c, i, &r);
            if (r.kind != NUE_ASSET_RES_TEXTURE) continue;
            const std::string label = res_label(c, r);
            if (!filter.empty() && lower(label).find(filter) == std::string::npos) continue;
            fs::path f = out / (safe_name(label));
            std::string key = lower(f.generic_string());
            int n = used[key]++;
            char suffix[16] = "";
            if (n) std::snprintf(suffix, sizeof(suffix), "_%08llx", (unsigned long long)r.hash);
            f = f.generic_string() + suffix + "." + fmt;
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            nue_result sr = nue_asset_resource_save_texture(c, i, f.generic_string().c_str(), kind);
            if (sr >= 0) ++written; else { ++failed; std::fprintf(stderr, "  %s: %s\n", label.c_str(), nue_result_name(sr)); }
        }
    }
    if (a.flag("--raw")) {
        for (uint32_t i = 0; i < nue_asset_node_count(c); ++i) {
            nue_asset_node_info n;
            NUE_STRUCT_INIT(&n);
            nue_asset_node_get(c, i, &n);
            if (n.kind == NUE_ASSET_NODE_GROUP || n.kind == NUE_ASSET_NODE_PADDING) continue;
            std::vector<uint8_t> bytes(size_t(n.size));
            size_t got = 0;
            if (nue_asset_node_read(c, i, 0, bytes.data(), bytes.size(), &got) < 0) { ++failed; continue; }
            char name[64];
            std::snprintf(name, sizeof(name), "chunks/%06u_%08llx.bin", i, (unsigned long long)n.id);
            if (write_file(out / name, bytes.data(), got)) ++written; else ++failed;
        }
    }
    nue_asset_container_close(c);
    std::printf("%u files written to %s (%u failed)\n", written, out.string().c_str(), failed);
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------- convert / pack / export / repack
int cmd_convert(Args& a) {
    if (a.pos.empty() || !a.get("-o")) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    if (!parse_targets(a.get("--target"), &o.variants)) return 2;
    o.quality = parse_quality(a.get("--quality"));
    o.max_dimension = a.get("--max-dim") ? uint32_t(std::atoi(a.get("--max-dim"))) : 0;
    if (a.flag("--no-compress")) o.flags |= NUE_PACK_CONVERT_NO_COMPRESS;
    nue_tex_executor ex = nullptr;
    nue_tex_executor_desc ed;
    NUE_STRUCT_INIT(&ed);
    ed.threads = a.get("--threads") ? uint32_t(std::atoi(a.get("--threads"))) : 0;
    if (nue_tex_executor_create(&ed, &ex) >= 0) o.executor = ex;

    nue_asset_container c = nullptr;
    if (!open_container(s, a.pos[0], &c)) return 1;
    nue_pack_builder b = nullptr;
    nue_pack_builder_desc bd;
    NUE_STRUCT_INIT(&bd);
    const std::string src = fs::path(a.pos[0]).filename().generic_string();
    bd.source_name = src.c_str();
    nue_pack_builder_create(s.ctx, &bd, &b);
    const auto t0 = std::chrono::steady_clock::now();
    nue_result r = nue_pack_builder_add_container(b, c, &o);
    if (r < 0) return fail("conversion failed", r);
    if (r == NUE_OK_PARTIAL) std::fprintf(stderr, "warning: some textures could not be transcoded to every variant and were stored as RGBA8 there\n");
    r = nue_pack_builder_write(b, a.get("-o"), nullptr, nullptr);
    if (r < 0) return fail("cannot write the package", r);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::error_code ec;
    std::printf("%s -> %s: %llu bytes (%.1f s)\n", a.pos[0].c_str(), a.get("-o"), (unsigned long long)fs::file_size(a.get("-o"), ec), secs);
    nue_pack_builder_destroy(b);
    nue_asset_container_close(c);
    if (ex) nue_tex_executor_destroy(ex);
    return 0;
}

bool is_texture_file(const fs::path& p) { std::string e = lower(p.extension().string()); return e == ".dds" || e == ".png" || e == ".ktx2"; }

int cmd_pack(Args& a) {
    if (a.pos.size() < 2) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    nue_pack_convert_options o;
    NUE_STRUCT_INIT(&o);
    if (!parse_targets(a.get("--target"), &o.variants)) return 2;
    o.quality = parse_quality(a.get("--quality"));
    if (a.flag("--no-compress")) o.flags |= NUE_PACK_CONVERT_NO_COMPRESS;
    nue_pack_builder b = nullptr;
    nue_pack_builder_create(s.ctx, nullptr, &b);
    std::vector<std::pair<std::string, fs::path>> files;   // name inside the package, path
    for (size_t i = 1; i < a.pos.size(); ++i) {
        fs::path in = a.pos[i];
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            std::vector<fs::path> list;
            for (auto it = fs::recursive_directory_iterator(in, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) if (it->is_regular_file()) list.push_back(it->path());
            std::sort(list.begin(), list.end());
            for (auto& f : list) files.push_back({fs::relative(f, in, ec).generic_string(), f});
        } else files.push_back({in.filename().generic_string(), in});
    }
    unsigned tex = 0, blobs = 0;
    for (auto& f : files) {
        if (is_texture_file(f.second)) {
            nue_tex_image im = nullptr;
            if (nue_tex_image_load_file(f.second.generic_string().c_str(), NUE_TEX_FILE_AUTO, 0, &im) >= 0) {
                std::string name = f.first.substr(0, f.first.size() - f.second.extension().string().size());
                nue_result r = nue_pack_builder_add_image(b, name.c_str(), im, &o);
                nue_tex_image_destroy(im);
                if (r < 0) return fail(("cannot add " + f.first).c_str(), r);
                ++tex;
                continue;
            }
        }
        std::vector<uint8_t> bytes;
        if (!read_file(f.second.generic_string(), bytes)) { std::fprintf(stderr, "cannot read %s\n", f.second.string().c_str()); return 1; }
        nue_result r = nue_pack_builder_add_blob(b, f.first.c_str(), bytes.data(), bytes.size(), o.flags);
        if (r < 0) return fail(("cannot add " + f.first).c_str(), r);
        ++blobs;
    }
    nue_result r = nue_pack_builder_write(b, a.pos[0].c_str(), nullptr, nullptr);
    if (r < 0) return fail("cannot write the package", r);
    std::printf("%s: %u textures, %u other files\n", a.pos[0].c_str(), tex, blobs);
    nue_pack_builder_destroy(b);
    return 0;
}

int cmd_export(Args& a) {
    if (a.pos.empty() || !a.get("-o")) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    nue_pack p = nullptr;
    nue_result r = nue_pack_open_file(s.ctx, a.pos[0].c_str(), NUE_PACK_OPEN_VERIFY, &p);
    if (r < 0) return fail("cannot open package", r);
    nue_asset_container c = nullptr;
    r = nue_pack_export_container(p, &c);
    if (r < 0) return fail("cannot rebuild the container", r);
    nue_asset_write_desc w;
    NUE_STRUCT_INIT(&w);
    w.path = a.get("-o");
    w.codec_level = a.get("--level") ? std::atoi(a.get("--level")) : 0;
    if (lower(fs::path(a.get("-o")).extension().string()) == ".bun") w.flags |= NUE_ASSET_WRITE_NO_CODEC;
    r = nue_asset_container_write(c, &w);
    if (r < 0) return fail("cannot write", r);
    std::error_code ec;
    std::printf("%s -> %s: %llu bytes\n", a.pos[0].c_str(), a.get("-o"), (unsigned long long)fs::file_size(a.get("-o"), ec));
    nue_asset_container_close(c);
    nue_pack_close(p);
    return 0;
}

uint32_t find_texture(nue_asset_container c, const std::string& name) {
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) return nue_asset_resource_find(c, nullptr, std::strtoull(name.c_str(), nullptr, 16));
    for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
        nue_asset_resource_info r;
        NUE_STRUCT_INIT(&r);
        nue_asset_resource_get(c, i, &r);
        if (r.kind != NUE_ASSET_RES_TEXTURE) continue;
        if (lower(r.name) == lower(name) || lower(res_label(c, r)) == lower(name) || lower(safe_name(res_label(c, r))) == lower(name)) return i;   // TEX, PACK/TEX or the extracted file name PACK_TEX
    }
    return NUE_ASSET_NO_NODE;
}

int cmd_repack(Args& a) {
    if (a.pos.empty() || !a.get("-o")) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    nue_asset_container c = nullptr;
    if (!open_container(s, a.pos[0], &c)) return 1;
    std::vector<std::pair<std::string, std::string>> jobs;
    auto it = a.opt.find("--replace");
    if (it != a.opt.end()) for (auto& spec : it->second) {
        size_t eq = spec.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "--replace wants NAME=FILE, got '%s'\n", spec.c_str()); return 2; }
        jobs.push_back({spec.substr(0, eq), spec.substr(eq + 1)});
    }
    if (a.get("--replace-dir")) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(a.get("--replace-dir"), ec)) if (e.is_regular_file() && is_texture_file(e.path())) jobs.push_back({e.path().stem().string(), e.path().generic_string()});
        std::sort(jobs.begin(), jobs.end());
    }
    unsigned done = 0, missing = 0;
    for (auto& j : jobs) {
        const uint32_t res = find_texture(c, j.first);
        if (res == NUE_ASSET_NO_NODE) { std::fprintf(stderr, "no texture named '%s'\n", j.first.c_str()); ++missing; continue; }
        nue_tex_image im = nullptr;
        nue_result r = nue_tex_image_load_file(j.second.c_str(), NUE_TEX_FILE_AUTO, 0, &im);
        if (r < 0) return fail(("cannot load " + j.second).c_str(), r);
        r = nue_asset_resource_set_image(c, res, im);
        nue_tex_image_destroy(im);
        if (r < 0) return fail(("cannot replace " + j.first).c_str(), r);
        ++done;
    }
    nue_asset_write_desc w;
    NUE_STRUCT_INIT(&w);
    w.path = a.get("-o");
    w.codec_level = a.get("--level") ? std::atoi(a.get("--level")) : 0;
    const std::string ext = lower(fs::path(a.get("-o")).extension().string());
    if (ext == ".lzc") w.flags |= NUE_ASSET_WRITE_FORCE_CODEC;
    else if (ext == ".bun") w.flags |= NUE_ASSET_WRITE_NO_CODEC;
    nue_result r = nue_asset_container_write(c, &w);
    if (r < 0) return fail("cannot write", r);
    std::error_code ec;
    std::printf("%u textures replaced (%u not found) -> %s: %llu bytes\n", done, missing, a.get("-o"), (unsigned long long)fs::file_size(a.get("-o"), ec));
    nue_asset_container_close(c);
    return missing ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------- verify
std::vector<uint8_t> write_mem(nue_asset_container c, uint32_t flags, nue_result* r) {
    void* data = nullptr;
    size_t size = 0;
    nue_asset_write_desc w;
    NUE_STRUCT_INIT(&w);
    w.flags = flags;
    w.out_data = &data;
    w.out_size = &size;
    *r = nue_asset_container_write(c, &w);
    std::vector<uint8_t> out;
    if (*r >= 0 && data) out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    if (data) nue_free(data);
    return out;
}

int cmd_verify(Args& a) {
    if (a.pos.empty()) return 2;
    Session s;
    if (!s.engine.ok) return fail("engine init", NUE_ERR_FAILED);
    int bad = 0;
    for (const std::string& path : a.pos) {
        if (is_pack_file(path)) {
            nue_pack p = nullptr;
            nue_result r = nue_pack_open_file(s.ctx, path.c_str(), 0, &p);
            uint32_t badn = 0;
            if (r >= 0) r = nue_pack_verify(p, &badn);
            std::printf("%-50s %s%s\n", path.c_str(), r >= 0 ? "OK" : "FAILED", r < 0 ? (" (" + std::to_string(badn) + " entries)").c_str() : "");
            if (r < 0) ++bad;
            if (p) nue_pack_close(p);
            continue;
        }
        std::vector<uint8_t> file;
        nue_asset_container c = nullptr;
        nue_asset_open_desc d;
        NUE_STRUCT_INIT(&d);
        d.path = path.c_str();
        nue_result r = nue_asset_container_open(s.ctx, &d, &c);
        if (r < 0) { std::printf("%-50s FAILED to open (%s: %s)\n", path.c_str(), nue_result_name(r), nue_get_last_error_message()); ++bad; continue; }
        nue_asset_container_info info;
        NUE_STRUCT_INIT(&info);
        nue_asset_container_get_info(c, &info);
        nue_result w;
        std::vector<uint8_t> content = write_mem(c, NUE_ASSET_WRITE_NO_CODEC, &w);
        bool ok = w >= 0;
        std::string why;
        if (ok) { auto rebuilt = write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_NO_CODEC, &w); if (w < 0 || rebuilt != content) { ok = false; why += " rebuild differs;"; } }
        if (ok) { auto regen = write_mem(c, NUE_ASSET_WRITE_REBUILD | NUE_ASSET_WRITE_REGENERATE | NUE_ASSET_WRITE_NO_CODEC, &w); if (w < 0 || regen != content) { ok = false; why += " texture regeneration differs;"; } }
        unsigned tex = 0, texbad = 0;
        for (uint32_t i = 0; i < nue_asset_resource_count(c); ++i) {
            nue_asset_resource_info ri;
            NUE_STRUCT_INIT(&ri);
            nue_asset_resource_get(c, i, &ri);
            if (ri.kind != NUE_ASSET_RES_TEXTURE) continue;
            nue_tex_image im = nullptr;
            if (nue_asset_resource_get_image(c, i, &im) >= 0) { ++tex; nue_tex_image_destroy(im); } else ++texbad;
        }
        if (texbad) { ok = false; why += " " + std::to_string(texbad) + " textures do not decode;"; }
        if (info.lenient_repairs) { ok = false; why += " damaged parts;"; }
        std::printf("%-50s %s  (%u chunks, %u textures)%s\n", path.c_str(), ok ? "OK" : "FAILED", info.node_count, tex, why.c_str());
        if (!ok) ++bad;
        nue_asset_container_close(c);
    }
    return bad ? 1 : 0;
}

int cmd_jdlz(Args& a) {
    if (a.pos.size() < 3) return 2;
    std::vector<uint8_t> in;
    if (!read_file(a.pos[1], in)) { std::fprintf(stderr, "cannot read %s\n", a.pos[1].c_str()); return 1; }
    std::vector<uint8_t> out;
    if (a.pos[0] == "c") {
        out.resize(nue_jdlz_compress_bound(in.size()));
        size_t n = 0;
        nue_result r = nue_jdlz_compress(in.data(), in.size(), a.get("--level") ? std::atoi(a.get("--level")) : 6, out.data(), out.size(), &n);
        if (r < 0) return fail("compress", r);
        out.resize(n);
    } else if (a.pos[0] == "d") {
        uint32_t unc = 0, stream = 0;
        nue_result r = nue_jdlz_identify(in.data(), in.size(), &unc, &stream);
        if (r < 0) return fail("not a JDLZ stream", r);
        out.resize(unc);
        size_t n = 0;
        r = nue_jdlz_decompress(in.data(), in.size(), out.data(), out.size(), &n);
        if (r < 0) return fail("decompress", r);
    } else return 2;
    if (!write_file(a.pos[2], out.data(), out.size())) { std::fprintf(stderr, "cannot write %s\n", a.pos[2].c_str()); return 1; }
    std::printf("%zu -> %zu bytes\n", in.size(), out.size());
    return 0;
}

void usage() {
    std::fprintf(stderr,
        "nue_pack - game resource files and .nuepack packages\n\n"
        "  nue_pack info <file>\n"
        "  nue_pack list <file> [--nodes] [--textures]\n"
        "  nue_pack extract <file> -o DIR [--textures] [--raw] [--format dds|png|ktx2] [--filter TEXT] [--all]\n"
        "  nue_pack convert <in.bun|in.lzc> -o OUT.nuepack [--target bc,astc,etc2,pvrtc,rgba8|all|<device class>] [--quality fastest|fast|normal|high|best]\n"
        "                   [--max-dim N] [--threads N] [--no-compress]\n"
        "  nue_pack pack OUT.nuepack <files or dirs...> [--target LIST] [--quality Q]\n"
        "  nue_pack export <in.nuepack> -o OUT.bun|OUT.lzc [--level N]\n"
        "  nue_pack repack <in.bun|in.lzc> -o OUT --replace NAME=FILE [...] [--replace-dir DIR] [--level N]\n"
        "  nue_pack verify <file...>\n"
        "  nue_pack jdlz c|d IN OUT [--level N]\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h")) { usage(); return argc < 2 ? 2 : 0; }
    const std::string cmd = argv[1];
    Args a = parse(argc, argv, 2);
    int rc;
    if (cmd == "info") rc = cmd_info(a);
    else if (cmd == "list") rc = cmd_list(a);
    else if (cmd == "extract") rc = cmd_extract(a);
    else if (cmd == "convert") rc = cmd_convert(a);
    else if (cmd == "pack") rc = cmd_pack(a);
    else if (cmd == "export") rc = cmd_export(a);
    else if (cmd == "repack") rc = cmd_repack(a);
    else if (cmd == "verify") rc = cmd_verify(a);
    else if (cmd == "jdlz") rc = cmd_jdlz(a);
    else { usage(); return 2; }
    if (rc == 2) usage();
    return rc;
}
