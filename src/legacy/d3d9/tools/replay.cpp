// nue_replay: replays a recorded Direct3D 9 trace through the layer.
//   nue_replay <trace folder> --null [--frames N] [--twice]    headless on the null Gpu: statistics and the deterministic call hash
//   nue_replay <trace folder> --bgfx d3d11|vulkan|opengl [--frames N] [--out dir] [--window]    (added with the bgfx adapter)
#include "nue/d3d9_layer.hpp"
#include "nue/d3d9_trace.hpp"
#include "nue/null_gpu.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef NUE_HAVE_BGFX
#include "nue/bgfx_gpu.hpp"
int run_bgfx(int argc, char** argv);
#endif

using namespace nue;

static void log_line(const char* message) { std::fprintf(stderr, "[layer] %s\n", message); }

struct NullRun { uint64_t hash = 0; d3d9::LayerStats layer; d3d9::ReplayStats replay; double seconds = 0; bool ok = false; std::string error; std::string report; uint64_t draws = 0, passes = 0, programs = 0; uint64_t constant_bytes = 0; };

static NullRun run_null(const std::string& folder, uint64_t frames) {
    NullRun out;
    d3d9::TraceReader reader;
    if (!reader.open(folder, out.error)) return out;
    NullGpu gpu;
    d3d9::LayerOptions options;
    options.log = log_line;
    auto layer = d3d9::create_layer(gpu, options);
    const auto t0 = std::chrono::steady_clock::now();
    out.ok = d3d9::replay(reader, *layer, out.replay, out.error, frames);
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    out.hash = gpu.call_hash;
    out.layer = layer->stats();
    out.report = layer->report();
    out.draws = gpu.draws; out.passes = gpu.passes; out.programs = gpu.programs_created; out.constant_bytes = gpu.constant_bytes;
    return out;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Crash report for the field: where (module + offset, to be looked up with llvm-symbolizer and the .pdb) and what.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    const void* address = info->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module)) GetModuleFileNameA(module, name, MAX_PATH);
    std::fprintf(stderr, "crash: exception %08lx at %p = %s+0x%llx\n", info->ExceptionRecord->ExceptionCode, address, name, (unsigned long long)(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    if (info->ExceptionRecord->NumberParameters >= 2) std::fprintf(stderr, "  %s of address %p\n", info->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : info->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "execute", reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    if (argc < 3) { std::fprintf(stderr, "usage: nue_replay <trace folder> --null [--frames N] [--twice]\n"); return 2; }
    std::string folder = argv[1];
    uint64_t frames = 0;
    bool twice = false, null_mode = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--null")) null_mode = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--twice")) twice = true;
    }
#ifdef NUE_HAVE_BGFX
    if (!null_mode) return run_bgfx(argc, argv);
#endif
    if (!null_mode) { std::fprintf(stderr, "this build has no bgfx adapter: use --null\n"); return 2; }
    NullRun a = run_null(folder, frames);
    if (!a.ok) { std::fprintf(stderr, "replay failed: %s\n", a.error.c_str()); return 1; }
    const auto& s = a.layer;
    std::printf("replayed %llu records, %llu frames in %.2f s (%.1f ms/frame)\n", (unsigned long long)a.replay.records, (unsigned long long)a.replay.frames, a.seconds,
                a.replay.frames ? 1000.0 * a.seconds / double(a.replay.frames) : 0.0);
    std::printf("unsupported records %llu, bad objects %llu\n", (unsigned long long)a.replay.unsupported, (unsigned long long)a.replay.bad_objects);
    std::printf("draws %llu (draw_up %llu, fixed-function %llu, skipped %llu), gpu draws %llu, passes %llu, clears %llu (partial %llu), programs %llu\n",
                (unsigned long long)s.draws, (unsigned long long)s.draw_up, (unsigned long long)s.draws_fixed_function, (unsigned long long)s.draws_skipped,
                (unsigned long long)a.draws, (unsigned long long)a.passes, (unsigned long long)s.clears, (unsigned long long)s.partial_clears, (unsigned long long)a.programs);
    std::printf("static buffer uploads %llu (%.1f MB), transient copies %llu (%.1f MB, %llu dedupe hits), index rewrites %llu, fan conversions %llu\n",
                (unsigned long long)s.static_buffer_uploads, double(s.static_buffer_bytes) / 1048576.0, (unsigned long long)s.transient_copies, double(s.transient_bytes) / 1048576.0,
                (unsigned long long)s.transient_dedupe_hits, (unsigned long long)s.index_rewrites, (unsigned long long)s.fan_conversions);
    std::printf("texture uploads %llu (%.1f MB, %.1f MB converted), shader translations %llu (%llu failures), constants %.1f MB, stretch_rect %llu, bad handles %llu\n",
                (unsigned long long)s.texture_uploads, double(s.texture_upload_bytes) / 1048576.0, double(s.texture_converted_bytes) / 1048576.0,
                (unsigned long long)s.shader_translations, (unsigned long long)s.shader_failures, double(s.constants_bytes) / 1048576.0, (unsigned long long)s.stretch_rects, (unsigned long long)s.bad_handles);
    std::printf("primitives:");
    for (int i = 1; i <= 6; ++i) std::printf(" %d:%llu", i, (unsigned long long)s.draw_by_primitive[i]);
    std::printf("\ncall hash %016llx\nunsupported:\n%s", (unsigned long long)a.hash, a.report.c_str());
    if (twice) {
        NullRun b = run_null(folder, frames);
        const bool same = b.ok && b.hash == a.hash;
        std::printf("second run hash %016llx: %s\n", (unsigned long long)b.hash, same ? "IDENTICAL (deterministic)" : "DIFFERENT");
        if (!same) return 1;
    }
    return 0;
}
