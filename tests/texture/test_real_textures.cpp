// Optional: run the planner and the converters over a directory of real textures (DDS / KTX2 / PNG).
//   nue_texture_test_real_textures <directory> [device_class] [max_textures] [stride]      or     NUE_TEX_REAL_DIR=<directory>
// Prints, per source -> target format, how many textures, the memory and the quality (PSNR / SSIM / CIEDE2000) of the converted result against the decoded source.
// Without a directory it reports "skipped" and passes: game data is never stored in the repository.
#include "tex_common.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;

// Run-time coverage: with NUE_TEX_REAL_CACHE=<cooked cache folder> every texture is replayed through the Direct3D 9 seam exactly as the layer would
// (CreateTexture + one convert_level per mip level with the level bytes) on a context that may NOT encode; reports levels served from the cold cache,
// placeholders, and the time per level. Done twice: with the alpha hint a smart layer would pass, and with "unknown".
static void replay_seam(const char* dir, const char* cache, const char* cls, uint32_t max_n, uint32_t stride) {
    nue_tex_caps caps; nue_tex_caps_for_class(nue_tex_device_class_from_name(cls), &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.cache_dir = cache;
    const auto t_open = std::chrono::steady_clock::now();
    nue_tex_context cx = nullptr;
    if (nue_tex_context_create(&cd, &cx) < 0) { std::printf("cannot open cache\n"); return; }
    nue_tex_runtime_stats rs; NUE_STRUCT_INIT(&rs); nue_tex_context_get_runtime_stats(cx, &rs);
    std::printf("cold cache opened in %.2f s, %llu source levels indexed\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t_open).count(), (unsigned long long)rs.indexed_levels);
    for (int pass = 0; pass < 2; ++pass) {
        uint64_t hits0 = rs.level_hits, miss0 = rs.level_misses;
        uint32_t seen = 0, n = 0, planned_lossy = 0;
        uint64_t levels = 0, bytes = 0;
        double secs = 0;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file() || n >= max_n || e.path().extension() != ".dds") continue;
            if ((seen++ % stride) != 0) continue;
            nue_tex_image im = nullptr;
            if (nue_tex_image_load_file(e.path().string().c_str(), NUE_TEX_FILE_AUTO, 0, &im) < 0) continue;
            ++n;
            nue_tex_image_desc d; NUE_STRUCT_INIT(&d); nue_tex_image_get_desc(im, &d);
            nue_tex_level_view v0; NUE_STRUCT_INIT(&v0); nue_tex_image_get_level(im, 0, 0, 0, &v0);
            nue_tex_scan_result sr; NUE_STRUCT_INIT(&sr); nue_tex_scan_level(d.format, d.width, d.height, v0.data, size_t(v0.size), &sr);
            nue_tex_d3d9_texture_desc td; NUE_STRUCT_INIT(&td);
            td.d3d_format = nue_tex_format_to_d3d9(d.format); td.width = d.width; td.height = d.height; td.levels = d.mip_levels; td.pool = 1;
            td.alpha_hint = pass == 0 ? sr.alpha_info : NUE_TEX_ALPHA_INFO_UNKNOWN;
            nue_tex_d3d9_texture t = nullptr; nue_tex_d3d9_texture_info info; NUE_STRUCT_INIT(&info);
            if (!td.d3d_format || nue_tex_d3d9_texture_create(cx, &td, &t, &info, nullptr) < 0) { nue_tex_image_destroy(im); continue; }
            if (info.action == NUE_TEX_ACTION_ENCODE || info.action == NUE_TEX_ACTION_TRANSCODE) {
                ++planned_lossy;
                for (uint32_t l = info.skip_mips; l < d.mip_levels; ++l) {
                    nue_tex_level_view v; NUE_STRUCT_INIT(&v); nue_tex_image_get_level(im, l, 0, 0, &v);
                    std::vector<uint8_t> out(nue_tex_d3d9_level_gpu_size(t, l, nullptr));
                    uint32_t pitch; size_t written;
                    const auto t0 = std::chrono::steady_clock::now();
                    nue_tex_d3d9_convert_level(t, l, 0, nullptr, v.data, v.row_pitch, out.data(), out.size(), &pitch, &written);
                    secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    ++levels; bytes += out.size();
                }
            }
            nue_tex_d3d9_texture_destroy(t);
            nue_tex_image_destroy(im);
        }
        nue_tex_context_get_runtime_stats(cx, &rs);
        std::printf("replay (%s alpha hint): %u textures, %u needed a cooked variant, %llu levels: %llu from the cache, %llu placeholders, %llu encodes; %.2f s, %.1f us per level, %.1f MB copied\n",
                    pass == 0 ? "scanned" : "unknown", n, planned_lossy, (unsigned long long)levels, (unsigned long long)(rs.level_hits - hits0), (unsigned long long)(rs.level_misses - miss0),
                    (unsigned long long)rs.runtime_encodes, secs, levels ? secs * 1e6 / double(levels) : 0.0, bytes / 1048576.0);
    }
    nue_tex_context_destroy(cx);
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : std::getenv("NUE_TEX_REAL_DIR");
    if (!dir || !*dir) { std::printf("skipped: no texture directory given (argument or NUE_TEX_REAL_DIR)\n"); return 0; }
    const char* cls = argc > 2 ? argv[2] : "android_astc";
    if (const char* cache = std::getenv("NUE_TEX_REAL_CACHE")) {
        replay_seam(dir, cache, cls, argc > 3 ? uint32_t(std::atoi(argv[3])) : 400, argc > 4 ? std::max(1, std::atoi(argv[4])) : 1);
        return 0;
    }
    nue_tex_caps caps; nue_tex_caps_for_class(nue_tex_device_class_from_name(cls), &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.flags = NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE;
    nue_tex_context cx = nullptr;
    if (nue_tex_context_create(&cd, &cx) < 0) return 1;
    const uint32_t max_n = argc > 3 ? uint32_t(std::atoi(argv[3])) : 400;
    const uint32_t stride = argc > 4 ? std::max(1, std::atoi(argv[4])) : 1;
    struct Agg { uint32_t n = 0; double psnr = 0, ssim = 0, de = 0, worst = 99, secs = 0; uint64_t src = 0, dst = 0; uint32_t below30 = 0; };
    std::map<std::string, Agg> agg;
    uint32_t seen = 0;
    uint32_t n = 0, bad = 0, lossy = 0;
    uint64_t src_bytes = 0, dst_bytes = 0;
    double worst_psnr = 99, sum_psnr = 0, secs = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || n >= max_n) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".dds" && ext != ".ktx2" && ext != ".png") continue;
        if ((seen++ % stride) != 0) continue;
        nue_tex_image im = nullptr;
        if (nue_tex_image_load_file(e.path().string().c_str(), NUE_TEX_FILE_AUTO, 0, &im) < 0) { ++bad; std::printf("  unreadable: %s\n", e.path().string().c_str()); continue; }
        ++n;
        nue_tex_convert_request rq; NUE_STRUCT_INIT(&rq); rq.source = im;
        nue_tex_image out = nullptr; nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
        const auto t0 = std::chrono::steady_clock::now();
        const nue_result r = nue_tex_context_convert(cx, &rq, &out, &plan, nullptr);
        secs += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (r < 0) { ++bad; std::printf("  FAIL %s: error %d\n    %s\n", e.path().string().c_str(), (int)r, plan.explanation); nue_tex_image_destroy(im); continue; }
        src_bytes += plan.source_bytes; dst_bytes += nue_tex_image_total_size(out);
        if (plan.action != NUE_TEX_ACTION_UPLOAD_AS_IS) {
            ++lossy;
            nue_tex_image_desc a, b; NUE_STRUCT_INIT(&a); NUE_STRUCT_INIT(&b);
            nue_tex_image_get_desc(im, &a); nue_tex_image_get_desc(out, &b);
            if (a.width == b.width && a.height == b.height) {
                nue_tex_metrics m; NUE_STRUCT_INIT(&m);
                if (nue_tex_compare_images(im, out, &m) >= 0) {
                    worst_psnr = std::min(worst_psnr, m.psnr_db); sum_psnr += m.psnr_db;
                    Agg& g = agg[std::string(nue_tex_format_name(a.format)) + " -> " + nue_tex_format_name(b.format)];
                    ++g.n; g.psnr += m.psnr_db; g.ssim += m.ssim; g.de += m.delta_e_mean; g.worst = std::min(g.worst, m.psnr_db);
                    g.src += plan.source_bytes; g.dst += nue_tex_image_total_size(out);
                    if (m.psnr_db < 30.0) { ++g.below30; if (std::getenv("NUE_TEX_VERBOSE")) std::printf("    low: %.1f dB %s  %ux%u -> %s\n", m.psnr_db, e.path().filename().string().c_str(), a.width, a.height, plan.explanation); }
                }
            }
        }
        nue_tex_image_destroy(im); nue_tex_image_destroy(out);
    }
    for (const auto& kv : agg)
        std::printf("  %-40s %5u textures  %7.1f -> %7.1f MB  psnr mean %.1f worst %.1f dB (%u below 30)  ssim %.3f  deltaE %.2f\n", kv.first.c_str(), kv.second.n, kv.second.src / 1048576.0,
                    kv.second.dst / 1048576.0, kv.second.psnr / kv.second.n, kv.second.worst, kv.second.below30, kv.second.ssim / kv.second.n, kv.second.de / kv.second.n);
    std::printf("%u textures (%u failed), %u converted for %s: %.1f MB -> %.1f MB, %.1f s, mean psnr %.1f dB, worst %.1f dB\n", n, bad, lossy, cls, src_bytes / 1048576.0, dst_bytes / 1048576.0,
                secs, lossy ? sum_psnr / lossy : 0.0, worst_psnr);
    nue_tex_context_destroy(cx);
    return bad ? 1 : 0;
}
