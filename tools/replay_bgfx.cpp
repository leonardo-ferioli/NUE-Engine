// nue_replay --bgfx: replays a trace through the layer and the bgfx adapter into a window, saves the frames that have a reference image and compares them.
//   nue_replay <trace> --bgfx d3d11|vulkan|opengl [--frames N] [--out dir] [--window] [--keep-going]
// Metrics per reference image: mean absolute error over RGB (0..255), and the 99.9th percentile of the per-pixel maximum channel difference outside a
// one-pixel edge mask (pixels near strong gradients of the reference image are excluded: geometry edges differ by rasteriser rules).
#include "nue/bgfx_gpu.hpp"
#include "nue/d3d9_layer.hpp"
#include "nue/d3d9_trace.hpp"
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

struct Metrics { double mae = 0, p999 = 0, max_diff = 0; double masked_fraction = 0; };

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
    double sum = 0;
    std::vector<uint32_t> histogram(256, 0);
    uint64_t counted = 0, masked = 0;
    if (diff_image) diff_image->assign(size_t(W) * H * 4, 255);
    for (size_t i = 0; i < size_t(W) * H; ++i) {
        const uint8_t* r = &ref.bgra[i * 4]; const uint8_t* o = ours + i * 4;
        int mx = 0, total = 0;
        for (int k = 0; k < 3; ++k) { const int d = std::abs(int(r[k]) - int(o[k])); mx = std::max(mx, d); total += d; }
        sum += total / 3.0;
        m.max_diff = std::max<double>(m.max_diff, mx);
        if (diff_image) { uint8_t v = uint8_t(std::min(255, mx * 4)); (*diff_image)[i * 4] = (*diff_image)[i * 4 + 1] = (*diff_image)[i * 4 + 2] = v; if (mask[i]) (*diff_image)[i * 4 + 2] = std::max<uint8_t>(v, 40); }
        if (mask[i]) { ++masked; continue; }
        ++histogram[mx]; ++counted;
    }
    m.mae = sum / double(size_t(W) * H);
    m.masked_fraction = double(masked) / double(size_t(W) * H);
    uint64_t acc = 0;
    const uint64_t target = uint64_t(std::ceil(double(counted) * 0.999));
    for (int v = 0; v < 256; ++v) { acc += histogram[v]; if (acc >= target) { m.p999 = v; break; } }
    return m;
}

#ifdef _WIN32
LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_CLOSE) { std::exit(0); } return DefWindowProcA(h, m, w, l); }
HWND make_window(uint32_t w, uint32_t h, bool show) {
    WNDCLASSA wc{}; wc.lpfnWndProc = wnd_proc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nue_replay"; wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r{0, 0, LONG(w), LONG(h)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("nue_replay", "nue_replay", WS_OVERLAPPEDWINDOW, 100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, show ? SW_SHOW : SW_SHOWNOACTIVATE);
    if (!show) ShowWindow(hwnd, SW_HIDE);
    return hwnd;
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
static int run_native(const std::string& folder, const std::string& out_dir, uint64_t frames, bool show, unsigned long long draw_first, unsigned long long draw_last) {
    std::filesystem::create_directories(out_dir);
    HWND hwnd = make_window(1280, 720, show);
    std::string error;
    auto native = d3d9::create_native_d3d9(hwnd, error);
    if (!native) { std::fprintf(stderr, "native Direct3D 9 failed: %s\n", error.c_str()); return 1; }
    native->set_draw_range(draw_first, draw_last);
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

int run_bgfx(int argc, char** argv) {
    std::string folder = argv[1], out_dir = "nue_out", renderer_name = "d3d11";
    uint64_t frames = 0;
    bool show = false, native_mode = false;
    unsigned long long draw_first = 0, draw_last = ~0ull;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--bgfx") && i + 1 < argc) renderer_name = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--window")) show = true;
        else if (!std::strcmp(argv[i], "--native")) native_mode = true;
        else if (!std::strcmp(argv[i], "--draws") && i + 1 < argc) { std::sscanf(argv[++i], "%llu-%llu", &draw_first, &draw_last); }
    }
#ifdef _WIN32
    if (native_mode) return run_native(folder, out_dir, frames, show, draw_first, draw_last);
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
    lo.debug_draw_first = draw_first; lo.debug_draw_last = draw_last;
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
        std::printf("frame %u: MAE %.3f/255, p99.9 outside edges %.0f/255 (max %.0f, %.1f%% of pixels masked as edges)\n", frame, m.mae, m.p999, m.max_diff, 100.0 * m.masked_fraction);
    };
    hooks.presented = [&](uint64_t) { pump(); };
    d3d9::ReplayStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = d3d9::replay(reader, *layer, stats, error, frames, &hooks);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) std::fprintf(stderr, "replay failed: %s\n", error.c_str());
    const auto& s = layer->stats();
    const auto& g = gpu->stats();
    std::printf("replayed %llu frames, %llu draws in %.1f s; programs %llu (translations %llu, failures %llu); shader cross-compile %.1f ms, compile %.1f ms in total\n",
                (unsigned long long)stats.frames, (unsigned long long)s.draws, seconds, (unsigned long long)g.programs, (unsigned long long)s.shader_translations, (unsigned long long)s.shader_failures, g.cross_ms, g.compile_ms);
    if (!results.empty()) std::printf("images compared: %zu, mean of MAE %.3f, worst p99.9 %.0f\n", results.size(), sum_mae / double(results.size()), worst_p999);
    std::printf("unsupported:\n%s", layer->report().c_str());
    layer.reset();
    gpu.reset();
    return ok ? 0 : 1;
}
