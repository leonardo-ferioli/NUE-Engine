// Optional: run the planner and the converters over a directory of real textures (DDS / KTX2 / PNG).
//   nue_texture_test_real_textures <directory> [device_class]      or     NUE_TEX_REAL_DIR=<directory>
// Without a directory it reports "skipped" and passes: game data is never stored in the repository.
#include "tex_common.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : std::getenv("NUE_TEX_REAL_DIR");
    if (!dir || !*dir) { std::printf("skipped: no texture directory given (argument or NUE_TEX_REAL_DIR)\n"); return 0; }
    const char* cls = argc > 2 ? argv[2] : "android_astc";
    nue_tex_caps caps; nue_tex_caps_for_class(nue_tex_device_class_from_name(cls), &caps);
    nue_tex_context_desc cd; NUE_STRUCT_INIT(&cd); cd.caps = &caps;
    nue_tex_context cx = nullptr;
    if (nue_tex_context_create(&cd, &cx) < 0) return 1;
    uint32_t n = 0, bad = 0, lossy = 0;
    uint64_t src_bytes = 0, dst_bytes = 0;
    double worst_psnr = 99, sum_psnr = 0, secs = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || n >= 400) continue;
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (ext != ".dds" && ext != ".ktx2" && ext != ".png") continue;
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
                if (nue_tex_compare_images(im, out, &m) >= 0) { worst_psnr = std::min(worst_psnr, m.psnr_db); sum_psnr += m.psnr_db; }
            }
        }
        nue_tex_image_destroy(im); nue_tex_image_destroy(out);
    }
    std::printf("%u textures (%u failed), %u converted for %s: %.1f MB -> %.1f MB, %.1f s, mean psnr %.1f dB, worst %.1f dB\n", n, bad, lossy, cls, src_bytes / 1048576.0, dst_bytes / 1048576.0,
                secs, lossy ? sum_psnr / lossy : 0.0, worst_psnr);
    nue_tex_context_destroy(cx);
    return bad ? 1 : 0;
}
