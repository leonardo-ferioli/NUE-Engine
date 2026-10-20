// nue_replay --bgfx: replays a trace through the layer and the bgfx adapter into a window, saves the frames that have a reference image and compares them.
//   nue_replay <trace> --bgfx d3d11|vulkan|opengl [--frames N] [--out dir] [--window] [--keep-going]
// Metrics per reference image: mean absolute error over RGB (0..255), and the 99.9th percentile of the per-pixel maximum channel difference outside a
// one-pixel edge mask (pixels near strong gradients of the reference image are excluded: geometry edges differ by rasteriser rules).
#include "nue/bgfx_gpu.hpp"
#include "nue/d3d9_layer.hpp"
#include "nue/d3d9_trace.hpp"
#include "nue/null_gpu.hpp"
#ifdef _WIN32
#include "native_d3d9.hpp"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

using namespace nue;

namespace {

struct Image { uint32_t width = 0, height = 0; std::vector<uint8_t> bgra; };   // top-down rows

bool read_bmp(const std::filesystem::path& path, Image& img) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
    const uint32_t off = u32(10);
    const int32_t w = int32_t(u32(18)), h = int32_t(u32(22));
    const uint16_t bpp = uint16_t(d[28] | d[29] << 8);
    if ((bpp != 24 && bpp != 32) || w <= 0) return false;
    const uint32_t H = uint32_t(std::abs(h));
    img.width = uint32_t(w); img.height = H;
    img.bgra.assign(size_t(w) * H * 4, 255);
    const size_t row = (size_t(w) * bpp / 8 + 3) & ~size_t(3);
    for (uint32_t y = 0; y < H; ++y) {
        const size_t src = off + row * (h < 0 ? y : H - 1 - y);
        if (src + size_t(w) * bpp / 8 > d.size()) return false;
        for (int32_t x = 0; x < w; ++x) {
            const uint8_t* p = &d[src + size_t(x) * bpp / 8];
            uint8_t* q = &img.bgra[(size_t(y) * w + x) * 4];
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 255;
        }
    }
    return true;
}

void write_bmp(const std::filesystem::path& path, uint32_t w, uint32_t h, const uint8_t* bgra) {
    std::ofstream f(path, std::ios::binary);
    uint8_t header[54] = {'B', 'M'};
    const uint32_t size = 54 + w * h * 4, off = 54, dib = 40, planes_bpp = 1 | (32 << 16);
    std::memcpy(header + 2, &size, 4); std::memcpy(header + 10, &off, 4); std::memcpy(header + 14, &dib, 4);
    std::memcpy(header + 18, &w, 4); const int32_t nh = -int32_t(h); std::memcpy(header + 22, &nh, 4); std::memcpy(header + 26, &planes_bpp, 4);
    f.write(reinterpret_cast<const char*>(header), 54);
    f.write(reinterpret_cast<const char*>(bgra), size_t(w) * h * 4);
}

struct Metrics { double mae = 0, p999 = 0, max_diff = 0; double masked_fraction = 0; double edge_mae = 0; double crop_mae = 0, crop_edge_mae = 0, crop_p999 = 0; bool has_crop = false; };

Metrics compare(const Image& ref, const uint8_t* ours, std::vector<uint8_t>* diff_image) {
    const uint32_t W = ref.width, H = ref.height;
    std::vector<uint8_t> edge(size_t(W) * H, 0);
    auto pix = [&](uint32_t x, uint32_t y) { return &ref.bgra[(size_t(y) * W + x) * 4]; };
    for (uint32_t y = 0; y + 1 < H; ++y) for (uint32_t x = 0; x + 1 < W; ++x) {
        const uint8_t* a = pix(x, y); const uint8_t* b = pix(x + 1, y); const uint8_t* c = pix(x, y + 1);
        int g = 0;
        for (int k = 0; k < 3; ++k) g = std::max({g, std::abs(int(a[k]) - int(b[k])), std::abs(int(a[k]) - int(c[k]))});
        if (g > 32) edge[size_t(y) * W + x] = 1;
    }
    std::vector<uint8_t> mask(edge.size(), 0);
    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) if (edge[size_t(y) * W + x])
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int64_t xx = int64_t(x) + dx, yy = int64_t(y) + dy;
            if (xx >= 0 && yy >= 0 && xx < W && yy < H) mask[size_t(yy) * W + xx] = 1;
        }
    Metrics m;
    // NUE_CROP=x,y,w,h: the same metrics inside a region (e.g. a car), plus the error on the edge band that the standard p99.9 leaves out
    int crop[4] = {0, 0, 0, 0};
    if (const char* c = std::getenv("NUE_CROP")) if (std::sscanf(c, "%d,%d,%d,%d", &crop[0], &crop[1], &crop[2], &crop[3]) == 4 && crop[2] > 0 && crop[3] > 0) m.has_crop = true;
    double edge_sum = 0, crop_sum = 0, crop_edge_sum = 0;
    uint64_t edge_n = 0, crop_n = 0, crop_edge_n = 0;
    std::vector<uint32_t> crop_histogram(256, 0);
    uint64_t crop_counted = 0;
    double sum = 0;
    std::vector<uint32_t> histogram(256, 0);
    uint64_t counted = 0, masked = 0;
    if (diff_image) diff_image->assign(size_t(W) * H * 4, 255);
    for (size_t i = 0; i < size_t(W) * H; ++i) {
        const uint8_t* r = &ref.bgra[i * 4]; const uint8_t* o = ours + i * 4;
        int mx = 0, total = 0;
        for (int k = 0; k < 3; ++k) { const int d = std::abs(int(r[k]) - int(o[k])); mx = std::max(mx, d); total += d; }
        sum += total / 3.0;
        if (mask[i]) { edge_sum += total / 3.0; ++edge_n; }
        if (m.has_crop) {
            const int x = int(i % W), y = int(i / W);
            if (x >= crop[0] && y >= crop[1] && x < crop[0] + crop[2] && y < crop[1] + crop[3]) {
                crop_sum += total / 3.0; ++crop_n;
                if (mask[i]) { crop_edge_sum += total / 3.0; ++crop_edge_n; } else { ++crop_histogram[mx]; ++crop_counted; }
            }
        }
        m.max_diff = std::max<double>(m.max_diff, mx);
        if (diff_image) { uint8_t v = uint8_t(std::min(255, mx * 4)); (*diff_image)[i * 4] = (*diff_image)[i * 4 + 1] = (*diff_image)[i * 4 + 2] = v; if (mask[i]) (*diff_image)[i * 4 + 2] = std::max<uint8_t>(v, 40); }
        if (mask[i]) { ++masked; continue; }
        ++histogram[mx]; ++counted;
    }
    m.mae = sum / double(size_t(W) * H);
    m.edge_mae = edge_n ? edge_sum / double(edge_n) : 0;
    if (m.has_crop && crop_n) {
        m.crop_mae = crop_sum / double(crop_n);
        m.crop_edge_mae = crop_edge_n ? crop_edge_sum / double(crop_edge_n) : 0;
        uint64_t a = 0;
        const uint64_t t = uint64_t(std::ceil(double(crop_counted) * 0.999));
        for (int v = 0; v < 256; ++v) { a += crop_histogram[v]; if (a >= t) { m.crop_p999 = v; break; } }
    }
    m.masked_fraction = double(masked) / double(size_t(W) * H);
    uint64_t acc = 0;
    const uint64_t target = uint64_t(std::ceil(double(counted) * 0.999));
    for (int v = 0; v < 256; ++v) { acc += histogram[v]; if (acc >= target) { m.p999 = v; break; } }
    return m;
}

#ifdef _WIN32
LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_CLOSE) { std::exit(0); } return DefWindowProcA(h, m, w, l); }
HWND make_window(uint32_t w, uint32_t h, bool) {   // never shown: no window may appear on the user's desktop (the swap chain works on a hidden window)
    WNDCLASSA wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nue_replay"; wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    return CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "nue_replay", "nue_replay", WS_POPUP, -32000, -32000, int(w), int(h), nullptr, nullptr, wc.hInstance, nullptr);
}
void pump() { MSG msg; while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); } }
#else
void* make_window(uint32_t, uint32_t, bool) { return nullptr; }
void pump() {}
#endif

void log_line(const char* m) { std::fprintf(stderr, "[layer] %s\n", m); }

}  // namespace

#ifdef _WIN32
// Replays the trace on the real Direct3D 9 runtime and compares the captures with the recorded reference images (fidelity of the trace itself).
static int run_native(const std::string& folder, const std::string& out_dir, uint64_t frames, bool show, unsigned long long draw_first, unsigned long long draw_last, unsigned long long draw_also) {
    std::filesystem::create_directories(out_dir);
    HWND hwnd = make_window(1280, 720, show);
    std::string error;
    auto native = d3d9::create_native_d3d9(hwnd, error);
    if (!native) { std::fprintf(stderr, "native Direct3D 9 failed: %s\n", error.c_str()); return 1; }
    native->set_draw_range(draw_first, draw_last, draw_also);
    d3d9::TraceReader reader;
    if (!reader.open(folder, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    d3d9::ReplayHooks hooks;
    double sum = 0; int count = 0;
    hooks.reference_image = [&](uint32_t frame, uint32_t, uint32_t, uint32_t) {
        char name[64]; std::snprintf(name, sizeof name, "frame_%06u.bmp", frame);
        Image ref;
        std::vector<uint8_t> bgra; uint32_t w = 0, h = 0;
        if (!native->capture(bgra, w, h)) { std::printf("frame %u: capture failed\n", frame); return; }
        char ours[96]; std::snprintf(ours, sizeof ours, "native_%06u.bmp", frame);
        write_bmp(std::filesystem::path(out_dir) / ours, w, h, bgra.data());
        if (!read_bmp(std::filesystem::path(folder) / name, ref) || ref.width != w || ref.height != h) { std::printf("frame %u: reference not comparable\n", frame); return; }
        const Metrics m = compare(ref, bgra.data(), nullptr);
        sum += m.mae; ++count;
        std::printf("frame %u (native replay vs recorded reference): MAE %.3f/255, p99.9 outside edges %.0f/255, max %.0f\n", frame, m.mae, m.p999, m.max_diff);
    };
    hooks.presented = [&](uint64_t) { pump(); };
    d3d9::ReplayStats stats;
    const bool ok = d3d9::replay(reader, native->device(), stats, error, frames, &hooks);
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", error.c_str());
    std::printf("native replay: %llu frames, %llu records, %llu unsupported records, %llu bad objects; mean MAE %.3f over %d images\n", (unsigned long long)stats.frames, (unsigned long long)stats.records,
                (unsigned long long)stats.unsupported, (unsigned long long)stats.bad_objects, count ? sum / count : 0.0, count);
    return ok ? 0 : 1;
}
#endif


#ifdef _WIN32
#include <dbghelp.h>
#include <timeapi.h>
#include <atomic>
#include <thread>
// A sampling profiler: another thread suspends the replaying thread every millisecond and records where it was; symbols come from the PDB.
struct Sampler {
    // Each sample keeps where the thread was and, when that is outside this program (a Windows or driver function), the first return address on its stack that
    // points into this program: "who called it". The stack is scanned, not unwound, so the caller is a good guess, not a proof.
    struct Sample { uint64_t ip, caller; };
    std::atomic<bool> stop{false};
    std::thread worker;
    std::vector<Sample> samples;   // preallocated: the sampler must not allocate while the sampled thread is suspended (it could hold the heap lock)
    uint32_t total = 0;
    uint64_t code_begin = 0, code_end = 0;
    void start() {
        samples.reserve(1u << 20);
        timeBeginPeriod(1);   // 1 ms sleeps: a sample per millisecond
        const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        code_begin = uint64_t(uintptr_t(base)); code_end = code_begin + nt->OptionalHeader.SizeOfImage;
        HANDLE target = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
        worker = std::thread([this, target] {
            while (!stop) {
                Sleep(1);
                if (SuspendThread(target) == DWORD(-1)) continue;
                CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(target, &ctx) && samples.size() < samples.capacity()) {
#ifdef _WIN64
                    const uint64_t ip = ctx.Rip, sp = ctx.Rsp;
#else
                    const uint64_t ip = ctx.Eip, sp = ctx.Esp;
#endif
                    uint64_t caller = 0;
                    if (ip < code_begin || ip >= code_end) {
                        const auto* stack = reinterpret_cast<const uintptr_t*>(uintptr_t(sp));
                        for (int i = 0; i < 512 && !caller; ++i) {   // the thread is suspended: its stack does not move under us
                            const uint64_t v = stack[i];
                            if (v > code_begin + 0x1000 && v < code_end) caller = v;
                        }
                    }
                    samples.push_back({ip, caller});
                    ++total;
                }
                ResumeThread(target);
            }
            CloseHandle(target);
        });
    }
    void report(unsigned top) {
        stop = true; worker.join();
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir = exe; dir = dir.substr(0, dir.find_last_of(char(92)));
        SymInitialize(process, dir.c_str(), TRUE);   // the modules loaded now, symbols from the PDBs next to them (no symbol server)
        SymRefreshModuleList(process);
        std::map<uint64_t, std::string> names;
        auto name_of = [&](uint64_t address) -> const std::string& {
            auto it = names.find(address);
            if (it != names.end()) return it->second;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            return names[address] = SymFromAddr(process, address, &disp, sym) ? std::string(sym->Name) : std::string("(unknown)");
        };
        std::map<std::string, uint32_t> by_function, by_caller;
        for (const Sample& s : samples) {
            const std::string& f = name_of(s.ip);
            ++by_function[f];
            if (s.caller) ++by_caller[f + "  <-  " + name_of(s.caller)];
        }
        auto print = [&](const std::map<std::string, uint32_t>& m, unsigned count) {
            std::vector<std::pair<uint32_t, std::string>> ordered;
            for (auto& [name, n] : m) ordered.push_back({n, name});
            std::sort(ordered.rbegin(), ordered.rend());
            for (unsigned i = 0; i < ordered.size() && i < count; ++i) std::printf("  %5.1f%%  %s\n", 100.0 * ordered[i].first / std::max(total, 1u), ordered[i].second.c_str());
        };
        std::printf("samples: %u\n", total);
        print(by_function, top);
        std::printf("functions outside this program, by the caller found on the stack:\n");
        print(by_caller, 25);
    }
};
#endif

// ---- benchmark: replays one recording window many times from memory and reports the time per frame --------------------------------------
// nue_replay <trace> --bench [--bgfx d3d11|vulkan|null] [--window N] [--loops L] [--mt]
// The records of window N are read once (blobs cached in memory); the window is replayed L + 1 times (the first loop is untimed warm-up: shader
// translation, buffers, textures). Frame time = from one Present to the next, trace decoding included (it is small: the records are in memory).
static int run_bench(int argc, char** argv) {
    std::string folder = argv[1], mode = "d3d11";
    unsigned window_index = 0, loops = 4;
    bool multithreaded = false;
    std::string cache_dir;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bgfx") && i + 1 < argc) mode = argv[++i];
        else if (!std::strcmp(argv[i], "--window") && i + 1 < argc) window_index = unsigned(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--loops") && i + 1 < argc) loops = unsigned(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--mt")) multithreaded = true;
        else if (!std::strcmp(argv[i], "--shader-cache") && i + 1 < argc) cache_dir = argv[++i];
    }
    std::string error;
    std::unique_ptr<gpu::Gpu> gpu_owner;
    gpu::BgfxGpu* bgfx_gpu = nullptr;
    NullGpu* null_gpu = nullptr;
#ifdef _WIN32
    std::unique_ptr<d3d9::NativeCapture> com_device;
    if (mode == "com") {   // the drop-in dll through its COM objects (NUE_D3D9_DLL=<path>): the same calls the game makes, minus the launcher
        com_device = d3d9::create_native_d3d9(make_window(1280, 720, false), error);
        if (!com_device) { std::fprintf(stderr, "COM device failed: %s\n", error.c_str()); return 1; }
        std::printf("device: %s through COM\n", std::getenv("NUE_D3D9_DLL") ? std::getenv("NUE_D3D9_DLL") : "the system d3d9.dll");
    } else
#endif
    if (mode == "null" || mode == "devnull") { auto g = std::make_unique<NullGpu>(); null_gpu = g.get(); gpu_owner = std::move(g); }
    else {
        gpu::BgfxOptions options;
        options.renderer = mode == "vulkan" ? gpu::Renderer::vulkan : mode == "opengl" ? gpu::Renderer::opengl : mode == "noop" ? gpu::Renderer::noop : gpu::Renderer::d3d11;
        options.window_width = 1280; options.window_height = 720;
        options.single_threaded = !multithreaded;
        options.profile_draws = !std::getenv("NUE_BENCH_LIGHT");   // NUE_BENCH_LIGHT=1: no per-draw clocks (what the dll does), for the sampler and for timing the real cost
        options.shader_cache_dir = cache_dir;
        options.window = make_window(1280, 720, false);
        auto g = gpu::create_bgfx_gpu(options, error);
        if (!g) { std::fprintf(stderr, "bgfx adapter failed: %s\n", error.c_str()); return 1; }
        bgfx_gpu = g.get();
        gpu_owner = std::move(g);
        std::printf("renderer: %s (%s)\n", bgfx_gpu->stats().renderer_name, multithreaded ? "render thread" : "single thread");
    }
    d3d9::LayerOptions lo;
    lo.log = log_line;
    lo.profile = !std::getenv("NUE_BENCH_LIGHT");
    std::unique_ptr<d3d9::Layer> layer_owner = gpu_owner ? d3d9::create_layer(*gpu_owner, lo) : nullptr;
    NullDevice bare_device;   // --bgfx devnull: only the replay harness, no layer
    const bool bare = mode == "devnull";
    d3d9::Layer* layer = layer_owner.get();
#ifdef _WIN32
    d3d9::Device& device = com_device ? com_device->device() : bare ? static_cast<d3d9::Device&>(bare_device) : static_cast<d3d9::Device&>(*layer);
#else
    d3d9::Device& device = bare ? static_cast<d3d9::Device&>(bare_device) : static_cast<d3d9::Device&>(*layer);
#endif
    d3d9::TraceReader reader;
    if (!reader.open(folder, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    d3d9::ReplayStats stats;
    d3d9::ReplaySession session(reader, device, stats);
    std::vector<d3d9::Record> window;
    d3d9::Record record;
    bool collecting = false;
    const auto load_start = std::chrono::steady_clock::now();
    while (reader.next(record)) {
        if (!collecting && record.op == d3d9::op_window_begin && record.words.size() >= 1 && record.words[0] == window_index) { collecting = true; reader.cache_blobs(true); }
        if (collecting) {
            window.push_back(record);
            if (record.op == d3d9::op_window_end) break;
            continue;
        }
        if (!session.step(record)) { std::fprintf(stderr, "malformed record\n"); return 1; }
    }
    std::printf("trace up to window %u replayed in %.1f s; window has %zu records\n", window_index, std::chrono::duration<double>(std::chrono::steady_clock::now() - load_start).count(), window.size());
    std::vector<double> frame_ms;
    auto last = std::chrono::steady_clock::now();
    d3d9::ReplayHooks hooks;
    bool timing = false;
    hooks.presented = [&](uint64_t) {
        static const double pace_ms = std::getenv("NUE_BENCH_PACE_MS") ? std::atof(std::getenv("NUE_BENCH_PACE_MS")) : 0.0;   // pretend the game is the bottleneck: one frame every pace_ms
        if (pace_ms > 0) { const auto until = last + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double, std::milli>(pace_ms)); while (std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::microseconds(200)); }
        const auto now = std::chrono::steady_clock::now();
        if (timing) frame_ms.push_back(std::chrono::duration<double, std::milli>(now - last).count());
        last = now;
        pump();
    };
    session.set_hooks(&hooks);
#ifdef _WIN32
    Sampler sampler;
    const bool sample = [&] { for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--sample")) return true; return false; }();
#endif
    for (unsigned loop = 0; loop <= loops; ++loop) {
        timing = loop > 0;
        if (loop == 1) { if (layer) layer->reset_profile(); if (bgfx_gpu) bgfx_gpu->reset_profile(); }
#ifdef _WIN32
        if (loop == 1 && sample) sampler.start();
#endif
        last = std::chrono::steady_clock::now();
        for (const d3d9::Record& r : window) if (!session.step(r)) { std::fprintf(stderr, "malformed record in the window\n"); return 1; }
    }
#ifdef _WIN32
    if (sample) sampler.report(45);
#endif
    if (frame_ms.empty()) { std::printf("no frames measured\n"); return 1; }
    if (std::getenv("NUE_BENCH_FRAMES")) for (size_t i = 0; i < frame_ms.size(); ++i) std::printf("frame %zu: %.2f ms\n", i % 120, frame_ms[i]);
    std::vector<double> v = frame_ms;
    std::sort(v.begin(), v.end());
    double sum = 0; for (double x : v) sum += x;
    std::printf("%zu frames timed: mean %.2f ms, median %.2f ms, p95 %.2f ms, p99 %.2f ms, max %.2f ms  =>  %.0f fps mean, %.0f fps at p99\n", v.size(), sum / double(v.size()), v[v.size() / 2],
                v[size_t(double(v.size()) * 0.95)], v[size_t(double(v.size()) * 0.99)], v.back(), 1000.0 * double(v.size()) / sum, 1000.0 / v[size_t(double(v.size()) * 0.99)]);
    if (bare || !layer) return 0;
    const auto& s = layer->stats();
    std::printf("layer per frame (all loops): draws %.0f, transient copies %.0f, static uploads %.2f, constants %.1f KB\n", double(s.draws) / double(stats.frames), double(s.transient_copies) / double(stats.frames),
                double(s.static_buffer_uploads) / double(stats.frames), double(s.constants_bytes) / double(stats.frames) / 1024.0);
    std::printf("%s", layer->profile_report().c_str());
#ifdef _WIN32
    { PROCESS_MEMORY_COUNTERS pmc{}; if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) std::printf("process memory: peak working set %.0f MB, now %.0f MB (includes the cached window of the trace)\n", double(pmc.PeakWorkingSetSize) / 1048576.0, double(pmc.WorkingSetSize) / 1048576.0); }
#endif
    if (bgfx_gpu) {
        std::printf("%s", bgfx_gpu->profile_report().c_str());
        const auto& bs = bgfx_gpu->stats();
        std::printf("programs %llu, shader builds %llu, SPIR-V cross-compile %.0f ms, shader compiler %.0f ms (whole run: the cold cost of the first frames; %s)\n", (unsigned long long)bs.programs, (unsigned long long)bs.shader_builds, bs.cross_ms, bs.compile_ms, cache_dir.empty() ? "no disk cache" : "disk cache on");
    }
    layer_owner.reset();
    gpu_owner.reset();
    return 0;
}

int run_bgfx(int argc, char** argv) {
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--bench")) return run_bench(argc, argv);
    std::string folder = argv[1], out_dir = "nue_out", renderer_name = "d3d11";
    uint64_t frames = 0;
    bool show = false, native_mode = false;
    unsigned long long draw_first = 0, draw_last = ~0ull, draw_also = ~0ull;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bgfx") && i + 1 < argc) renderer_name = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--window")) show = true;
        else if (!std::strcmp(argv[i], "--native")) native_mode = true;
        else if (!std::strcmp(argv[i], "--also") && i + 1 < argc) draw_also = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--draws") && i + 1 < argc) { std::sscanf(argv[++i], "%llu-%llu", &draw_first, &draw_last); }
    }
#ifdef _WIN32
    if (native_mode) return run_native(folder, out_dir, frames, show, draw_first, draw_last, draw_also);
#endif
    std::filesystem::create_directories(out_dir);
    gpu::BgfxOptions options;
    options.renderer = renderer_name == "vulkan" ? gpu::Renderer::vulkan : renderer_name == "opengl" ? gpu::Renderer::opengl : renderer_name == "noop" ? gpu::Renderer::noop : gpu::Renderer::d3d11;
    options.window_width = 1280; options.window_height = 720;
    options.window = make_window(1280, 720, show);
    std::string error;
    auto gpu = gpu::create_bgfx_gpu(options, error);
    if (!gpu) { std::fprintf(stderr, "bgfx adapter failed: %s\n", error.c_str()); return 1; }
    std::printf("renderer: %s\n", gpu->stats().renderer_name);
    d3d9::LayerOptions lo;
    lo.log = log_line;
    lo.debug_record_draws = std::getenv("NUE_RECENT_DRAWS") != nullptr;
    lo.debug_log_passes = std::getenv("NUE_LOG_PASSES") != nullptr;
    lo.debug_draw_first = draw_first; lo.debug_draw_last = draw_last; lo.debug_draw_also = draw_also;
    auto layer = d3d9::create_layer(*gpu, lo);

    d3d9::TraceReader reader;
    if (!reader.open(folder, error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    struct Result { uint32_t frame; Metrics m; };
    std::vector<Result> results;
    double sum_mae = 0, worst_p999 = 0;
    d3d9::ReplayHooks hooks;
    hooks.reference_image = [&](uint32_t frame, uint32_t w, uint32_t h, uint32_t) {
        char name[64]; std::snprintf(name, sizeof name, "frame_%06u.bmp", frame);
        Image ref;
        if (!read_bmp(std::filesystem::path(folder) / name, ref)) { std::printf("frame %u: reference image %s not readable\n", frame, name); return; }
        layer->flush();
        if (std::getenv("NUE_RECENT_DRAWS") && std::atoi(std::getenv("NUE_RECENT_DRAWS")) == int(frame)) std::printf("recent draws before the capture of frame %u:\n%s", frame, layer->recent_draws().c_str());
        uint32_t rw = 0, rh = 0; std::vector<std::byte> bgra;
        if (gpu->read_back(gpu::backbuffer, rw, rh, bgra) != Status::ok || rw != ref.width || rh != ref.height) { std::printf("frame %u: read back failed (%ux%u vs %ux%u)\n", frame, rw, rh, ref.width, ref.height); return; }
        for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = std::byte{255};
        std::vector<uint8_t> diff;
        const Metrics m = compare(ref, reinterpret_cast<const uint8_t*>(bgra.data()), &diff);
        char ours[96], dname[96]; std::snprintf(ours, sizeof ours, "ours_%06u.bmp", frame); std::snprintf(dname, sizeof dname, "diff_%06u.bmp", frame);
        write_bmp(std::filesystem::path(out_dir) / ours, rw, rh, reinterpret_cast<const uint8_t*>(bgra.data()));
        write_bmp(std::filesystem::path(out_dir) / dname, rw, rh, diff.data());
        results.push_back({frame, m});
        sum_mae += m.mae; worst_p999 = std::max(worst_p999, m.p999);
        std::printf("frame %u: MAE %.3f/255, p99.9 outside edges %.0f/255 (max %.0f, %.1f%% of pixels masked as edges), error on the edge band %.2f", frame, m.mae, m.p999, m.max_diff, 100.0 * m.masked_fraction, m.edge_mae);
        if (m.has_crop) std::printf("; crop: MAE %.3f, edge-band MAE %.2f, p99.9 %.0f", m.crop_mae, m.crop_edge_mae, m.crop_p999);
        std::printf("\n");
    };
    std::vector<double> frame_ms;
    auto last_present = std::chrono::steady_clock::now();
    const bool log_frame_draws = std::getenv("NUE_FRAME_DRAWS") != nullptr;   // prints the number of draws made up to the end of every frame (to pick --draws ranges)
    hooks.presented = [&](uint64_t) { if (log_frame_draws) std::printf("present: %llu draws so far\n", (unsigned long long)layer->stats().draws); const auto now = std::chrono::steady_clock::now(); frame_ms.push_back(std::chrono::duration<double, std::milli>(now - last_present).count()); pump(); last_present = std::chrono::steady_clock::now(); };
    d3d9::ReplayStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = d3d9::replay(reader, *layer, stats, error, frames, &hooks);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", error.c_str());
    const auto& s = layer->stats();
    const auto& g = gpu->stats();
    std::printf("replayed %llu frames, %llu draws in %.1f s; programs %llu (translations %llu, failures %llu); shader cross-compile %.1f ms, compile %.1f ms in total\n",
                (unsigned long long)stats.frames, (unsigned long long)s.draws, seconds, (unsigned long long)g.programs, (unsigned long long)s.shader_translations, (unsigned long long)s.shader_failures, g.cross_ms, g.compile_ms);
    if (frame_ms.size() > 30) {   // steady state: skip the first frames of the run (resource creation) and the first frame of every window
        std::vector<double> v(frame_ms.begin() + 20, frame_ms.end());
        std::sort(v.begin(), v.end());
        double sum = 0; for (double x : v) sum += x;
        std::printf("frame time (replay + layer + bgfx, trace reading included): mean %.1f ms, median %.1f ms, p95 %.1f ms, p99 %.1f ms\n", sum / double(v.size()), v[v.size() / 2], v[size_t(double(v.size()) * 0.95)], v[size_t(double(v.size()) * 0.99)]);
    }
    if (!results.empty()) std::printf("images compared: %zu, mean of MAE %.3f, worst p99.9 %.0f\n", results.size(), sum_mae / double(results.size()), worst_p999);
    std::printf("unsupported:\n%s", layer->report().c_str());
    layer.reset();
    gpu.reset();
    return ok ? 0 : 1;
}
