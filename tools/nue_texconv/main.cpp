// nue_texconv - inspect, convert, compare, plan and benchmark textures with the NUE texture module.
//
//   nue_texconv info <file>
//   nue_texconv convert <in> <out> [--format NAME] [--mips N|full|same] [--quality fastest|fast|normal|high|best] [--filter box|triangle|kaiser|lanczos]
//                                  [--normal] [--srgb] [--assume-srgb] [--alpha-coverage] [--threads N]
//   nue_texconv compare <reference> <other>
//   nue_texconv plan <file> --device CLASS [--priority balanced|quality|speed|memory] [--usage color|normal|data|hdr|ui] [--srgb] [--compress-raw]
//   nue_texconv bench [--size N] [--threads N] [--seconds S]
//   nue_texconv cook <srcdir> <cachedir> [--classes all|class,class,...] [--threads N] [--priority balanced|quality|speed|memory]
//                                        cold conversion of every DDS/KTX2/PNG under srcdir into every device format at once (the cache can be copied to other machines)
//   nue_texconv formats | classes | caps CLASS
//   nue_texconv scan <dir> [--device CLASS]          plan every DDS/KTX2/PNG under a directory and print the totals (real game textures)
#include "nue/texture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const char* arg_value(int argc, char** argv, const char* name) {
    for (int i = 0; i + 1 < argc; ++i) if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return nullptr;
}
static bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 0; i < argc; ++i) if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}
static nue_tex_quality parse_quality(const char* s) {
    if (!s) return NUE_TEX_QUALITY_NORMAL;
    if (!std::strcmp(s, "fastest")) return NUE_TEX_QUALITY_FASTEST;
    if (!std::strcmp(s, "fast")) return NUE_TEX_QUALITY_FAST;
    if (!std::strcmp(s, "high")) return NUE_TEX_QUALITY_HIGH;
    if (!std::strcmp(s, "best")) return NUE_TEX_QUALITY_BEST;
    return NUE_TEX_QUALITY_NORMAL;
}

static bool load(const char* path, nue_tex_image* out, uint32_t flags = 0) {
    const nue_result r = nue_tex_image_load_file(path, NUE_TEX_FILE_AUTO, flags, out);
    if (r < 0) { std::fprintf(stderr, "cannot load %s (error %d)\n", path, (int)r); return false; }
    return true;
}

static int cmd_info(int argc, char** argv) {
    if (argc < 3) return 2;
    nue_tex_image im;
    if (!load(argv[2], &im)) return 1;
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d);
    nue_tex_image_get_desc(im, &d);
    const nue_tex_format_info* fi = nue_tex_get_format_info(d.format);
    std::printf("%s\n  format   %s (%s, %.2f bpp, block %ux%u, %u bytes)\n  size     %u x %u x %u\n  mips     %u\n  layers   %u, faces %u\n  data     %llu bytes\n",
                argv[2], fi->name, fi->kind == NUE_TEX_KIND_BLOCK ? "block compressed" : "uncompressed", fi->bits_per_pixel, fi->block_width, fi->block_height, fi->block_bytes,
                d.width, d.height, d.depth, d.mip_levels, d.layers, d.faces, (unsigned long long)nue_tex_image_total_size(im));
    for (uint32_t l = 0; l < d.mip_levels; ++l) {
        nue_tex_level_view v; NUE_STRUCT_INIT(&v);
        nue_tex_image_get_level(im, l, 0, 0, &v);
        std::printf("    level %2u  %5u x %-5u  %10llu bytes  pitch %u\n", l, v.width, v.height, (unsigned long long)v.size, v.row_pitch);
    }
    nue_tex_level_view v; NUE_STRUCT_INIT(&v);
    nue_tex_image_get_level(im, 0, 0, 0, &v);
    nue_tex_scan_result sr; NUE_STRUCT_INIT(&sr);
    if (nue_tex_scan_level(d.format, v.width, v.height, v.data, size_t(v.size), &sr) >= 0) {
        static const char* names[] = {"unknown", "opaque", "binary (1-bit)", "smooth"};
        std::printf("  alpha    %s (min %u, max %u)%s\n", names[sr.alpha_info], sr.min_alpha, sr.max_alpha, sr.uses_punchthrough ? ", punch-through blocks" : "");
    }
    nue_tex_image_destroy(im);
    return 0;
}

static int cmd_convert(int argc, char** argv) {
    if (argc < 4) return 2;
    nue_tex_image src;
    if (!load(argv[2], &src, has_flag(argc, argv, "--assume-srgb") ? NUE_TEX_FILE_ASSUME_SRGB : 0)) return 1;
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d);
    nue_tex_image_get_desc(src, &d);
    nue_tex_format fmt = d.format;
    if (const char* f = arg_value(argc, argv, "--format")) {
        fmt = nue_tex_format_from_name(f);
        if (fmt == NUE_TEX_FORMAT_UNKNOWN) { std::fprintf(stderr, "unknown format %s (see: nue_texconv formats)\n", f); return 2; }
    }
    uint32_t mips = d.mip_levels;
    if (const char* m = arg_value(argc, argv, "--mips")) mips = !std::strcmp(m, "full") ? 0 : !std::strcmp(m, "same") ? d.mip_levels : (uint32_t)std::atoi(m);
    nue_tex_executor ex = nullptr;
    nue_tex_executor_desc ed; NUE_STRUCT_INIT(&ed);
    if (const char* t = arg_value(argc, argv, "--threads")) ed.threads = (uint32_t)std::atoi(t);
    nue_tex_executor_create(&ed, &ex);
    nue_tex_encode_params ep; nue_tex_encode_params_default(&ep);
    ep.quality = parse_quality(arg_value(argc, argv, "--quality"));
    ep.executor = ex;
    if (has_flag(argc, argv, "--normal")) ep.flags |= NUE_TEX_ENC_NORMAL_MAP; else ep.flags |= NUE_TEX_ENC_PERCEPTUAL;
    if (has_flag(argc, argv, "--srgb")) ep.flags |= NUE_TEX_ENC_SRGB;
    nue_tex_mip_params mp; nue_tex_mip_params_default(&mp);
    if (const char* f = arg_value(argc, argv, "--filter")) mp.filter = !std::strcmp(f, "triangle") ? NUE_TEX_MIP_TRIANGLE : !std::strcmp(f, "kaiser") ? NUE_TEX_MIP_KAISER : !std::strcmp(f, "lanczos") ? NUE_TEX_MIP_LANCZOS : NUE_TEX_MIP_BOX;
    if (has_flag(argc, argv, "--srgb")) mp.flags |= NUE_TEX_MIP_SRGB;
    if (has_flag(argc, argv, "--normal")) mp.flags |= NUE_TEX_MIP_NORMAL_MAP;
    if (has_flag(argc, argv, "--alpha-coverage")) mp.flags |= NUE_TEX_MIP_ALPHA_COVERAGE;
    const auto t0 = std::chrono::steady_clock::now();
    nue_tex_image out;
    const nue_result r = nue_tex_image_convert(src, fmt, mips, &ep, &mp, &out);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (r < 0) { std::fprintf(stderr, "conversion to %s failed (error %d)\n", nue_tex_format_name(fmt), (int)r); return 1; }
    const nue_result w = nue_tex_image_save_file(out, argv[3], NUE_TEX_FILE_AUTO, 0);
    if (w < 0) { std::fprintf(stderr, "cannot write %s (error %d)\n", argv[3], (int)w); return 1; }
    nue_tex_metrics m; NUE_STRUCT_INIT(&m);
    if (nue_tex_compare_images(src, out, &m) >= 0)
        std::printf("%s -> %s  %s  %.2f s  psnr %.2f dB  ssim %.4f  deltaE %.2f (max %.1f)\n", nue_tex_format_name(d.format), nue_tex_format_name(fmt), argv[3], secs, m.psnr_db, m.ssim, m.delta_e_mean, m.delta_e_max);
    nue_tex_image_destroy(src); nue_tex_image_destroy(out);
    nue_tex_executor_destroy(ex);
    return 0;
}

static int cmd_compare(int argc, char** argv) {
    if (argc < 4) return 2;
    nue_tex_image a, b;
    if (!load(argv[2], &a) || !load(argv[3], &b)) return 1;
    nue_tex_metrics m; NUE_STRUCT_INIT(&m);
    const nue_result r = nue_tex_compare_images(a, b, &m);
    if (r < 0) { std::fprintf(stderr, "cannot compare (error %d): sizes must match\n", (int)r); return 1; }
    std::printf("psnr %.2f dB (rgb %.2f, alpha %.2f)  ssim %.4f  deltaE mean %.3f max %.2f  max error %.0f/255\n", m.psnr_db, m.psnr_rgb_db, m.psnr_alpha_db, m.ssim, m.delta_e_mean, m.delta_e_max, m.max_abs_error);
    return 0;
}

static void fill_policy(int argc, char** argv, nue_tex_policy* p) {
    nue_tex_policy_default(p);
    if (const char* s = arg_value(argc, argv, "--priority")) p->priority = !std::strcmp(s, "quality") ? NUE_TEX_PRIORITY_QUALITY : !std::strcmp(s, "speed") ? NUE_TEX_PRIORITY_SPEED : !std::strcmp(s, "memory") ? NUE_TEX_PRIORITY_MEMORY : NUE_TEX_PRIORITY_BALANCED;
    if (has_flag(argc, argv, "--compress-raw")) p->flags |= NUE_TEX_POLICY_COMPRESS_RAW;
    if (const char* s = arg_value(argc, argv, "--max-dimension")) p->max_dimension = (uint32_t)std::atoi(s);
}

struct Agg { uint32_t count = 0; uint64_t src = 0, dst = 0, rgba8 = 0; };
static std::map<std::string, Agg> g_agg;
static uint64_t g_rgba8_total = 0;

static int plan_one(const char* path, const nue_tex_caps& caps, const nue_tex_policy& pol, nue_tex_usage usage, uint32_t flags, uint64_t* src_total, uint64_t* dst_total, bool verbose) {
    nue_tex_image im;
    if (nue_tex_image_load_file(path, NUE_TEX_FILE_AUTO, 0, &im) < 0) return 1;
    nue_tex_image_desc d; NUE_STRUCT_INIT(&d);
    nue_tex_image_get_desc(im, &d);
    nue_tex_level_view v; NUE_STRUCT_INIT(&v);
    nue_tex_image_get_level(im, 0, 0, 0, &v);
    nue_tex_scan_result sr; NUE_STRUCT_INIT(&sr);
    nue_tex_scan_level(d.format, v.width, v.height, v.data, size_t(v.size), &sr);
    nue_tex_plan_request rq; NUE_STRUCT_INIT(&rq);
    rq.source_format = d.format; rq.width = d.width; rq.height = d.height; rq.depth = d.depth; rq.mip_levels = d.mip_levels; rq.layers = d.layers; rq.faces = d.faces;
    rq.usage = usage; rq.alpha_info = sr.alpha_info; rq.flags = flags; rq.policy = &pol;
    nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
    const nue_result r = nue_tex_make_plan(&caps, &rq, &plan);
    if (verbose) std::printf("%s\n%s\n  => %s (%s), %llu -> %llu bytes\n", path, plan.explanation, nue_tex_format_name(plan.target_format), nue_tex_action_name(plan.action),
                             (unsigned long long)plan.source_bytes, (unsigned long long)plan.target_bytes);
    if (r >= 0) {
        *src_total += plan.source_bytes; *dst_total += plan.target_bytes;
        const uint64_t rgba8 = nue_tex_chain_size(NUE_TEX_RGBA8_UNORM, d.width, d.height, d.depth, d.mip_levels, d.layers, d.faces);
        g_rgba8_total += rgba8;
        Agg& a = g_agg[std::string(nue_tex_format_name(plan.source_format)) + " -> " + nue_tex_format_name(plan.target_format) + " (" + nue_tex_action_name(plan.action) + ")"];
        ++a.count; a.src += plan.source_bytes; a.dst += plan.target_bytes; a.rgba8 += rgba8;
    }
    nue_tex_image_destroy(im);
    return r < 0 ? 1 : 0;
}

static int cmd_plan(int argc, char** argv) {
    if (argc < 3) return 2;
    nue_tex_caps caps;
    nue_tex_caps_for_class(nue_tex_device_class_from_name(arg_value(argc, argv, "--device") ? arg_value(argc, argv, "--device") : "android_astc"), &caps);
    nue_tex_policy pol; fill_policy(argc, argv, &pol);
    nue_tex_usage usage = NUE_TEX_USAGE_COLOR;
    if (const char* u = arg_value(argc, argv, "--usage")) usage = !std::strcmp(u, "normal") ? NUE_TEX_USAGE_NORMAL_MAP : !std::strcmp(u, "data") ? NUE_TEX_USAGE_DATA : !std::strcmp(u, "hdr") ? NUE_TEX_USAGE_HDR : !std::strcmp(u, "ui") ? NUE_TEX_USAGE_UI : NUE_TEX_USAGE_COLOR;
    uint64_t a = 0, b = 0;
    return plan_one(argv[2], caps, pol, usage, has_flag(argc, argv, "--srgb") ? NUE_TEX_REQ_SRGB : 0u, &a, &b, true);
}

static int scan_class(int argc, char** argv, const nue_tex_caps& caps, const nue_tex_policy& pol) {
    g_agg.clear(); g_rgba8_total = 0;
    uint64_t a = 0, b = 0; uint32_t n = 0, failed = 0;
    for (const auto& e : fs::recursive_directory_iterator(argv[2])) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".dds" && ext != ".ktx2" && ext != ".png") continue;
        ++n;
        if (plan_one(e.path().string().c_str(), caps, pol, NUE_TEX_USAGE_COLOR, has_flag(argc, argv, "--srgb") ? NUE_TEX_REQ_SRGB : 0u, &a, &b, has_flag(argc, argv, "--verbose"))) ++failed;
    }
    std::printf("== %s: %u textures (%u unreadable): shipped %.1f MB -> %.1f MB (RGBA8 fallback of the current D3D9 layer: %.1f MB)\n", nue_tex_device_class_name(caps.device_class), n, failed,
                a / 1048576.0, b / 1048576.0, g_rgba8_total / 1048576.0);
    for (const auto& kv : g_agg)
        std::printf("   %-52s %5u textures  %8.1f -> %8.1f MB   (RGBA8: %.1f MB)\n", kv.first.c_str(), kv.second.count, kv.second.src / 1048576.0, kv.second.dst / 1048576.0, kv.second.rgba8 / 1048576.0);
    return 0;
}

static int cmd_scan(int argc, char** argv) {
    if (argc < 3) return 2;
    nue_tex_policy pol; fill_policy(argc, argv, &pol);
    const char* dev = arg_value(argc, argv, "--device") ? arg_value(argc, argv, "--device") : "android_astc";
    if (std::strcmp(dev, "all") == 0) {
        for (int i = 0; i < NUE_TEX_DEVICE_COUNT; ++i) { nue_tex_caps caps; nue_tex_caps_for_class(nue_tex_device_class(i), &caps); scan_class(argc, argv, caps, pol); }
        return 0;
    }
    nue_tex_caps caps;
    nue_tex_caps_for_class(nue_tex_device_class_from_name(dev), &caps);
    return scan_class(argc, argv, caps, pol);
}

static int cmd_cook(int argc, char** argv) {
    if (argc < 4) return 2;
    uint32_t mask = NUE_TEX_CLASS_MASK_ALL;
    if (const char* c = arg_value(argc, argv, "--classes")) {
        if (std::strcmp(c, "all") != 0) {
            mask = 0;
            std::string list = c;
            size_t pos = 0;
            while (pos <= list.size()) {
                const size_t e = list.find(',', pos);
                const std::string name = list.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                bool found = false;
                for (int i = 0; i < NUE_TEX_DEVICE_COUNT; ++i) if (name == nue_tex_device_class_name(nue_tex_device_class(i))) { mask |= NUE_TEX_CLASS_BIT(i); found = true; }
                if (!found) { std::fprintf(stderr, "unknown device class '%s' (see: nue_texconv classes)\n", name.c_str()); return 2; }
                if (e == std::string::npos) break;
                pos = e + 1;
            }
        }
    }
    nue_tex_caps caps;
    nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &caps);
    nue_tex_policy pol; fill_policy(argc, argv, &pol);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd);
    cd.caps = &caps; cd.policy = &pol; cd.cache_dir = argv[3];
    if (const char* t = arg_value(argc, argv, "--threads")) cd.worker_threads = (uint32_t)std::atoi(t);
    nue_tex_context cx;
    if (nue_tex_context_create(&cd, &cx) < 0) { std::fprintf(stderr, "cannot open the cache folder %s\n", argv[3]); return 1; }
    nue_tex_importer imp;
    nue_tex_importer_create(cx, [](void*, const nue_tex_import_progress* p) {
        std::printf("\r%u/%u textures, %u already cooked, %u failed, %.0f s, eta %.0f s   ", p->done + p->failed, p->total, p->skipped_cached, p->failed, p->elapsed_seconds, p->eta_seconds);
        std::fflush(stdout);
    }, nullptr, &imp);
    nue_tex_importer_set_classes(imp, mask);
    std::vector<std::string> paths;
    for (const auto& e : fs::recursive_directory_iterator(argv[2])) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext == ".dds" || ext == ".ktx2" || ext == ".png") paths.push_back(e.path().string());
    }
    for (const std::string& p : paths) {
        nue_tex_import_item it; NUE_STRUCT_INIT(&it);
        std::string lower = fs::path(p).filename().string();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        it.name = p.c_str(); it.path = p.c_str();
        // name heuristic for normal maps (the content type is not stored in DDS files); a game integration passes the real usage
        it.usage = (lower.find("normal") != std::string::npos || lower.find("_n.") != std::string::npos) ? NUE_TEX_USAGE_NORMAL_MAP : NUE_TEX_USAGE_COLOR;
        nue_tex_importer_add(imp, &it);
    }
    std::printf("cooking %zu textures into %s\n", paths.size(), argv[3]);
    nue_tex_importer_start(imp);
    nue_tex_importer_wait(imp, -1);
    nue_tex_import_progress p; NUE_STRUCT_INIT(&p);
    nue_tex_importer_get_progress(imp, &p);
    nue_tex_cache_stats st; NUE_STRUCT_INIT(&st);
    nue_tex_cache_scan(cx);
    nue_tex_cache_get_stats(cx, &st);
    std::printf("\ndone: %u textures (%u failed), %.1f s; cache now %llu files, %.1f MB\n", p.done, p.failed, p.elapsed_seconds, (unsigned long long)st.entries, st.bytes / 1048576.0);
    nue_tex_importer_destroy(imp);
    nue_tex_context_destroy(cx);
    return p.failed ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------- benchmark
static std::vector<uint8_t> bench_image(uint32_t n) {
    std::vector<uint8_t> px(size_t(n) * n * 4);
    uint32_t s = 99;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            const double u = double(x) / n, v = double(y) / n;
            s = s * 1664525u + 1013904223u;
            const double noise = (double((s >> 8) & 255) / 255.0 - 0.5) * 0.08;
            const double r = 0.5 + 0.4 * std::sin(u * 17 + std::cos(v * 9)) + noise, g = 0.5 + 0.4 * std::sin(v * 13 + u * 5) + noise, b = 0.5 + 0.4 * std::cos((u + v) * 11) + noise;
            const double cell = (std::fmod(std::floor(u * 8) + std::floor(v * 8), 2.0) < 1.0) ? 1.0 : 0.0;
            uint8_t* p = &px[(size_t(y) * n + x) * 4];
            auto q = [](double c) { return uint8_t(std::min(1.0, std::max(0.0, c)) * 255.0 + 0.5); };
            p[0] = q(r * (0.7 + 0.3 * cell)); p[1] = q(g); p[2] = q(b); p[3] = q(0.5 + 0.5 * std::sin(u * 7));
        }
    return px;
}

static int cmd_bench(int argc, char** argv) {
    const uint32_t n = arg_value(argc, argv, "--size") ? (uint32_t)std::atoi(arg_value(argc, argv, "--size")) : 1024;
    const double min_seconds = arg_value(argc, argv, "--seconds") ? std::atof(arg_value(argc, argv, "--seconds")) : 0.3;
    const uint32_t max_threads = arg_value(argc, argv, "--threads") ? (uint32_t)std::atoi(arg_value(argc, argv, "--threads")) : std::max(1u, std::thread::hardware_concurrency());
    const auto px = bench_image(n);
    std::printf("NUE texture benchmark: %ux%u synthetic image, %u hardware threads\ncodecs: %s\n\n", n, n, std::thread::hardware_concurrency(), nue_tex_codec_info());
    std::vector<uint32_t> thread_list = {1};
    for (uint32_t t = 2; t <= max_threads; t *= 2) thread_list.push_back(t);
    if (thread_list.back() != max_threads) thread_list.push_back(max_threads);
    struct Row { nue_tex_format f; nue_tex_quality q; };
    const std::vector<Row> rows = {
        {NUE_TEX_BC1_UNORM, NUE_TEX_QUALITY_NORMAL}, {NUE_TEX_BC3_UNORM, NUE_TEX_QUALITY_NORMAL}, {NUE_TEX_BC4_UNORM, NUE_TEX_QUALITY_NORMAL}, {NUE_TEX_BC5_UNORM, NUE_TEX_QUALITY_NORMAL},
        {NUE_TEX_BC7_UNORM, NUE_TEX_QUALITY_FASTEST}, {NUE_TEX_BC7_UNORM, NUE_TEX_QUALITY_NORMAL},
        {NUE_TEX_ETC1_RGB8, NUE_TEX_QUALITY_NORMAL}, {NUE_TEX_ETC2_RGB8, NUE_TEX_QUALITY_FAST}, {NUE_TEX_ETC2_RGB8, NUE_TEX_QUALITY_NORMAL}, {NUE_TEX_ETC2_RGBA8, NUE_TEX_QUALITY_NORMAL},
        {NUE_TEX_EAC_R11_UNORM, NUE_TEX_QUALITY_NORMAL}, {NUE_TEX_EAC_RG11_UNORM, NUE_TEX_QUALITY_NORMAL},
        {NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_QUALITY_FASTEST}, {NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_QUALITY_FAST}, {NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_QUALITY_NORMAL},
        {NUE_TEX_ASTC_UNORM(4), NUE_TEX_QUALITY_FAST}, {NUE_TEX_ASTC_UNORM(7), NUE_TEX_QUALITY_FAST}, {NUE_TEX_PVRTC1_4BPP, NUE_TEX_QUALITY_NORMAL},
    };
    std::printf("%-18s %-8s %8s", "encode", "quality", "psnr");
    for (uint32_t t : thread_list) std::printf(" %7ut", t);
    std::printf("   (MPixel/s)\n");
    static const char* qn[] = {"fastest", "fast", "normal", "high", "best"};
    const double mpix = double(n) * n / 1e6;
    std::vector<nue_tex_executor> execs;
    for (uint32_t t : thread_list) {
        nue_tex_executor ex = nullptr;
        if (t > 1) { nue_tex_executor_desc ed; NUE_STRUCT_INIT(&ed); ed.threads = t - 1; nue_tex_executor_create(&ed, &ex); }      // the caller works too
        execs.push_back(ex);
    }
    std::vector<std::vector<uint8_t>> encoded;
    for (const Row& r : rows) {
        std::vector<uint8_t> dst(nue_tex_level_size(r.f, n, n, 1)), dec(size_t(n) * n * 4);
        nue_tex_encode_params ep; nue_tex_encode_params_default(&ep); ep.quality = r.q;
        std::printf("%-18s %-8s", nue_tex_format_name(r.f), qn[r.q]);
        double psnr = 0;
        std::string cells;
        for (size_t ti = 0; ti < thread_list.size(); ++ti) {
            ep.executor = execs[ti];
            int iters = 0;
            const auto t0 = std::chrono::steady_clock::now();
            double el = 0;
            do {
                if (nue_tex_encode_level(r.f, n, n, NUE_TEX_RGBA8_UNORM, px.data(), size_t(n) * 4, dst.data(), dst.size(), &ep) < 0) break;
                ++iters;
                el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            } while (el < min_seconds && iters < 20);
            char cell[32];
            std::snprintf(cell, sizeof cell, " %8.1f", iters ? mpix * iters / el : 0.0);
            cells += cell;
            if (ti == 0 && iters && nue_tex_decode_level(r.f, n, n, dst.data(), dst.size(), NUE_TEX_RGBA8_UNORM, dec.data(), dec.size(), nullptr) >= 0) {
                double se = 0; size_t cnt = 0;
                const int ch = (r.f == NUE_TEX_BC4_UNORM || r.f == NUE_TEX_EAC_R11_UNORM) ? 1 : (r.f == NUE_TEX_BC5_UNORM || r.f == NUE_TEX_EAC_RG11_UNORM) ? 2 : 3;
                for (size_t i = 0; i < size_t(n) * n; ++i) for (int k = 0; k < ch; ++k) { const double d = double(px[i * 4 + k]) - dec[i * 4 + k]; se += d * d; ++cnt; }
                psnr = se > 0 ? 10.0 * std::log10(255.0 * 255.0 / (se / double(cnt))) : 99.0;
            }
        }
        std::printf(" %7.1f%s\n", psnr, cells.c_str());
    }
    std::printf("\n%-18s %8s %8s   (MPixel/s, one thread)\n", "decode", "", "");
    for (const Row& r : rows) {
        if (r.q != NUE_TEX_QUALITY_NORMAL && r.q != NUE_TEX_QUALITY_FAST) continue;
        std::vector<uint8_t> enc(nue_tex_level_size(r.f, n, n, 1)), dec(size_t(n) * n * 4);
        nue_tex_encode_params ep; nue_tex_encode_params_default(&ep); ep.quality = NUE_TEX_QUALITY_FASTEST;
        if (nue_tex_encode_level(r.f, n, n, NUE_TEX_RGBA8_UNORM, px.data(), size_t(n) * 4, enc.data(), enc.size(), &ep) < 0) continue;
        int iters = 0;
        const auto t0 = std::chrono::steady_clock::now();
        double el = 0;
        do { nue_tex_decode_level(r.f, n, n, enc.data(), enc.size(), NUE_TEX_RGBA8_UNORM, dec.data(), dec.size(), nullptr); ++iters; el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); } while (el < min_seconds / 2 && iters < 50);
        std::printf("%-18s %8.1f\n", nue_tex_format_name(r.f), mpix * iters / el);
    }
    for (auto e : execs) nue_tex_executor_destroy(e);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: nue_texconv info|convert|compare|plan|scan|bench|formats|classes|caps ...  (see the comment at the top of tools/nue_texconv/main.cpp)\n");
        return 2;
    }
    const std::string cmd = argv[1];
    int rc = 2;
    if (cmd == "info") rc = cmd_info(argc, argv);
    else if (cmd == "convert") rc = cmd_convert(argc, argv);
    else if (cmd == "compare") rc = cmd_compare(argc, argv);
    else if (cmd == "plan") rc = cmd_plan(argc, argv);
    else if (cmd == "scan") rc = cmd_scan(argc, argv);
    else if (cmd == "bench") rc = cmd_bench(argc, argv);
    else if (cmd == "cook") rc = cmd_cook(argc, argv);
    else if (cmd == "formats") {
        nue_tex_format list[NUE_TEX_FORMAT_COUNT];
        const uint32_t n = nue_tex_format_list(list, NUE_TEX_FORMAT_COUNT);
        for (uint32_t i = 0; i < n; ++i) {
            const nue_tex_format_info* f = nue_tex_get_format_info(list[i]);
            std::printf("%-26s id %3d  %2ux%-2u block %2u B  %6.3f bpp  %s%s%s\n", f->name, (int)f->format, f->block_width, f->block_height, f->block_bytes, f->bits_per_pixel,
                        (f->flags & NUE_TEX_FINFO_HAS_DECODER) ? "dec " : "    ", (f->flags & NUE_TEX_FINFO_HAS_ENCODER) ? "enc " : "    ", (f->flags & NUE_TEX_FINFO_COMPRESSED) ? "compressed" : "");
        }
        rc = 0;
    } else if (cmd == "classes") {
        for (int i = 0; i < NUE_TEX_DEVICE_COUNT; ++i) std::printf("%s\n", nue_tex_device_class_name(nue_tex_device_class(i)));
        rc = 0;
    } else if (cmd == "caps" && argc >= 3) {
        nue_tex_caps caps;
        nue_tex_caps_for_class(nue_tex_device_class_from_name(argv[2]), &caps);
        size_t need = 0;
        nue_tex_caps_to_string(&caps, nullptr, 0, &need);
        std::vector<char> text(need);
        nue_tex_caps_to_string(&caps, text.data(), text.size(), &need);
        std::fputs(text.data(), stdout);
        rc = 0;
    }
    if (rc == 2) std::fprintf(stderr, "bad arguments for '%s'\n", cmd.c_str());
    return rc;
}
