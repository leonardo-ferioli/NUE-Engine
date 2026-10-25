// Quality regression on the real game textures. OPT-IN: set NUE_TEX_REAL_DIR to a folder of extracted DDS/KTX2/PNG textures (never committed). Without it the test
// reports "skipped" and passes. For every device class below each texture is planned and converted exactly as the cook does, the result is compared with the decoded source, and the
// per source -> target format averages are checked against the baseline measured on the NFSU2 dump (2026-10-22, 6119 textures). A codec change that makes any format worse than the
// margins fails; to accept a deliberate change regenerate the table with NUE_TEX_REAL_PRINT_BASELINE=1 and review the diff.
//   NUE_TEX_REAL_DIR=<folder>  NUE_TEX_REAL_STRIDE=<n> (use every n-th texture, default 1)  NUE_TEX_REAL_PRINT_BASELINE=1
#include "tex_common.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

namespace fs = std::filesystem;

struct Baseline { const char* key; uint32_t count; double psnr, ssim, delta_e; uint32_t below30; };

// class|source -> target, textures in the full dump, mean PSNR (dB), mean SSIM, mean CIEDE2000, textures below 30 dB
static const Baseline kBaseline[] = {
    {"android_astc|BC1_UNORM -> ASTC_6x5_UNORM", 506, 39.2, 0.992, 1.19, 15},
    {"android_astc|BC1_UNORM -> ETC2_RGB8", 4026, 36.7, 0.984, 2.60, 127},
    {"android_astc|BC2_UNORM -> ASTC_4x4_UNORM", 1195, 57.9, 0.999, 0.24, 0},
    {"android_astc|BC2_UNORM -> ETC2_RGB8", 256, 36.2, 0.934, 4.06, 19},
    {"android_etc2|BC1_UNORM -> ETC2_RGB8", 4026, 36.7, 0.984, 2.60, 127},
    {"android_etc2|BC1_UNORM -> ETC2_RGBA8", 506, 36.4, 0.888, 3.81, 22},
    {"android_etc2|BC2_UNORM -> ETC2_RGB8", 256, 36.2, 0.934, 4.06, 19},
    {"android_etc2|BC2_UNORM -> ETC2_RGBA8", 1195, 44.2, 0.965, 1.28, 32},
};

struct Agg { uint32_t n = 0, below30 = 0; double psnr = 0, ssim = 0, de = 0, secs = 0; };

int main() {
    const char* dir = std::getenv("NUE_TEX_REAL_DIR");
    if (!dir || !*dir) { std::printf("skipped: NUE_TEX_REAL_DIR is not set (real-texture quality regression is opt-in)\n"); return 0; }
    const int stride = std::getenv("NUE_TEX_REAL_STRIDE") ? std::max(1, std::atoi(std::getenv("NUE_TEX_REAL_STRIDE"))) : 1;
    const bool print = std::getenv("NUE_TEX_REAL_PRINT_BASELINE") != nullptr;
    std::vector<std::string> files;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".dds") continue;
        files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    std::printf("%zu textures found, using every %d-th\n", files.size(), stride);
    int failures = 0;
    nue_tex_policy pol; nue_tex_policy_default(&pol);
    const bool policy_override = std::getenv("NUE_TEX_REAL_QUALITY") != nullptr;      // experiments: force the encoder effort (0 fastest .. 4 best); the baseline check then does not apply
    if (policy_override) pol.quality = uint32_t(std::atoi(std::getenv("NUE_TEX_REAL_QUALITY"))) + 1;
    const bool experiment = policy_override || std::getenv("NUE_TEX_REAL_SRGB") != nullptr;
    for (const char* cls : {"android_astc", "android_etc2"}) {
        nue_tex_caps caps; nue_tex_caps_for_class(nue_tex_device_class_from_name(cls), &caps);
        nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps; cd.flags = NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE;
        nue_tex_context cx = nullptr;
        if (nue_tex_context_create(&cd, &cx) < 0) return 1;
        std::map<std::string, Agg> agg;
        uint32_t bad = 0;
        for (size_t i = 0; i < files.size(); i += size_t(stride)) {
            nue_tex_image im = nullptr;
            if (nue_tex_image_load_file(files[i].c_str(), NUE_TEX_FILE_AUTO, 0, &im) < 0) { ++bad; continue; }
            nue_tex_convert_request rq; NUE_STRUCT_INIT(&rq); rq.source = im;
            rq.policy = policy_override ? &pol : nullptr;
            nue_tex_image out = nullptr; nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
            const auto t0 = std::chrono::steady_clock::now();
            const nue_result r = nue_tex_context_convert(cx, &rq, &out, &plan, nullptr);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (r < 0) { ++bad; nue_tex_image_destroy(im); continue; }
            if (plan.action != NUE_TEX_ACTION_UPLOAD_AS_IS) {
                nue_tex_image_desc a, b; NUE_STRUCT_INIT(&a); NUE_STRUCT_INIT(&b);
                nue_tex_image_get_desc(im, &a); nue_tex_image_get_desc(out, &b);
                nue_tex_metrics m; NUE_STRUCT_INIT(&m);
                if (a.width == b.width && a.height == b.height && nue_tex_compare_images(im, out, &m) >= 0) {
                    Agg& g = agg[std::string(cls) + "|" + nue_tex_format_name(a.format) + " -> " + nue_tex_format_name(b.format)];
                    ++g.n; g.psnr += m.psnr_db; g.ssim += m.ssim; g.de += m.delta_e_mean; g.secs += secs;
                    if (m.psnr_db < 30.0) ++g.below30;
                }
            }
            nue_tex_image_destroy(im); nue_tex_image_destroy(out);
        }
        nue_tex_context_destroy(cx);
        if (bad) { std::printf("  FAIL %s: %u textures failed to load or convert\n", cls, bad); ++failures; }
        for (const auto& kv : agg) {
            const Agg& g = kv.second;
            const double psnr = g.psnr / g.n, ssim = g.ssim / g.n, de = g.de / g.n;
            if (experiment) { std::printf("  %-52s %5u  psnr %.2f  ssim %.4f  dE %.3f  below30 %u  (%.1f s)\n", kv.first.c_str(), g.n, psnr, ssim, de, g.below30, g.secs); continue; }
            if (print) { std::printf("    {\"%s\", %u, %.1f, %.3f, %.2f, %u},\n", kv.first.c_str(), g.n, psnr, ssim, de, g.below30); continue; }
            const Baseline* base = nullptr;
            for (const Baseline& b : kBaseline) if (kv.first == b.key) base = &b;
            std::printf("  %-52s %5u  psnr %.1f  ssim %.3f  dE %.2f  below30 %u  (%.2f s)", kv.first.c_str(), g.n, psnr, ssim, de, g.below30, g.secs);
            if (!base) { std::printf("  [not in the baseline]\n"); continue; }
            const bool sample = g.n >= 40;
            bool ok = true;
            if (sample) {
                ok &= psnr >= base->psnr - 1.0;
                ok &= ssim >= base->ssim - 0.01;
                ok &= de <= base->delta_e * 1.15 + 0.25;
                ok &= double(g.below30) / g.n <= double(base->below30) / base->count + 0.03;
            }
            std::printf("  %s\n", ok ? "ok" : "REGRESSION");
            if (!ok) {
                std::printf("      baseline: psnr %.1f ssim %.3f dE %.2f below30 %u/%u\n", base->psnr, base->ssim, base->delta_e, base->below30, base->count);
                ++failures;
            }
        }
    }
    return failures ? 1 : 0;
}
