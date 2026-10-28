// hello_gfx: the smallest real use of nue_gfx. A window (or none), nue_gfx in MANAGED mode (nue_gfx_create initialises bgfx on the best renderer of the platform),
// a cleared back buffer, a spinning colour triangle and a textured quad. It follows window resizes, pauses when the surface is lost (Android/iOS lifecycle) and
// recreates its GPU objects after a device reset - the same loop a game would have.
//
//   nue_sample_hello_gfx                       real window, platform default renderer
//   nue_sample_hello_gfx --renderer vulkan     d3d11 | d3d12 | vulkan | metal | opengl | gles | noop  (also config key gfx.renderer)
//   nue_sample_hello_gfx --noop --frames 60    NO window, no GPU, null backend: the CI smoke test (runs the same code through bgfx's Noop renderer)
//   nue_sample_hello_gfx --hidden --frames 30  real renderer but a hidden window (never shown on the desktop)
//
// Closes on the window close button, Escape or after --frames. It opens a window when run without --noop/--hidden, so it is never run by the tests except with --noop.
#include "nue/entry.h"
#include "nuepp/nue.hpp"

#include "draw.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct App {
    bool noop = false, hidden = false;
    int frames_wanted = 0;                 // 0 = until closed
    std::string renderer;
    int frames = 0;
    nue_window window = nullptr;
    nue_gfx gfx = nullptr;
    hello_gfx::Pipeline pipe;
    int width = 1280, height = 720;
    int64_t start_ns = 0;
    int resets = 0;
};
App g;
int g_argc = 0;
char** g_argv = nullptr;

const uint64_t kGpuBytes = 4 + 8 * 8 * 4;      // the two textures

void parse_args() {
    for (int i = 1; i < g_argc; ++i) {
        const std::string a = g_argv[i];
        if (a == "--noop") g.noop = true;
        else if (a == "--hidden") g.hidden = true;
        else if (a == "--frames" && i + 1 < g_argc) g.frames_wanted = std::atoi(g_argv[++i]);
        else if (a == "--renderer" && i + 1 < g_argc) g.renderer = g_argv[++i];
        else std::fprintf(stderr, "ignored argument: %s\n", a.c_str());
    }
}

bool create_gpu_objects() {
    // GPU memory goes through the engine's texture budget: ask before creating, report when destroyed
    if (nue_gfx_memory_request(g.gfx, kGpuBytes) != NUE_OK) return false;
    if (!g.pipe.init()) { nue_gfx_memory_report(g.gfx, -int64_t(kGpuBytes)); return false; }
    return true;
}

void destroy_gpu_objects() {
    g.pipe.shutdown();
    nue_gfx_memory_report(g.gfx, -int64_t(kGpuBytes));
}

void NUE_CALL on_gfx(void*, nue_gfx, nue_gfx_notification what, int32_t a, int32_t b) {
    if (what == NUE_GFX_NOTIFY_RESIZED) NUE_LOGI("sample", "back buffer follows the window: %dx%d", a, b);
    if (what == NUE_GFX_NOTIFY_SUSPENDED) NUE_LOGI("sample", "surface lost: rendering paused");
    if (what == NUE_GFX_NOTIFY_RESUMED) NUE_LOGI("sample", "surface back: rendering resumes");
    if (what == NUE_GFX_NOTIFY_DEVICE_LOST) NUE_LOGW("sample", "device lost: it is reset on the render thread");
}

nue_result on_init(void*, int, char**) {
    parse_args();
    if (!g.renderer.empty()) {
        if (g.renderer == "noop") g.noop = true;
        else nue_config_set_string("gfx.renderer", g.renderer.c_str());
    }
    if (!g.noop) {
        nue_window_desc wd = nuepp::init<nue_window_desc>();
        wd.title = "NUE hello_gfx";
        wd.width = g.width; wd.height = g.height;
        wd.flags = NUE_WINDOW_RESIZABLE | NUE_WINDOW_HIGH_DPI | (g.hidden ? NUE_WINDOW_HIDDEN : 0);
        nue_result r = nue_window_create(&wd, &g.window);
        if (NUE_FAILED(r)) { std::fprintf(stderr, "window: %s\n", nue_get_last_error_message()); return r; }
    }
    nue_gfx_desc gd = nuepp::init<nue_gfx_desc>();
    gd.window = g.window;
    gd.renderer = g.noop ? NUE_RENDERER_NOOP : NUE_RENDERER_AUTO;       // AUTO: config gfx.renderer, then the platform order
    gd.flags = NUE_GFX_VSYNC | NUE_GFX_MAXANISOTROPY;
    gd.width = g.noop ? g.width : 0; gd.height = g.noop ? g.height : 0;  // 0 = the window's pixel size, followed on resize
    gd.shader_cache_dir = "cache:shaders";
    nue_result r = nue_gfx_create(&gd, &g.gfx);
    if (NUE_FAILED(r)) { std::fprintf(stderr, "gfx: %s\n", nue_get_last_error_message()); return r; }
    nue_gfx_add_listener(g.gfx, on_gfx, nullptr);
    nue_gfx_set_auto_events(g.gfx, NUE_TRUE);                            // resize / surface / pause events drive the back buffer
    nue_gfx_caps caps = nuepp::init<nue_gfx_caps>();
    if (NUE_SUCCEEDED(nue_gfx_get_caps(g.gfx, &caps)))
        std::printf("hello_gfx: renderer %s on '%s', max texture %u\n", caps.renderer_name, caps.adapter_name, caps.max_texture_size);
    if (!create_gpu_objects()) return NUE_ERR_BACKEND;
    g.start_ns = nue_time_ns();
    return NUE_OK;
}

nue_app_result on_event(void*, const nue_event* e) {
    if (e->type == NUE_EVENT_QUIT || e->type == NUE_EVENT_WINDOW_CLOSE_REQUESTED) return NUE_APP_QUIT_OK;
    if (e->type == NUE_EVENT_KEY_DOWN && e->payload.key.scancode == NUE_SC_ESCAPE) return NUE_APP_QUIT_OK;
    return NUE_APP_CONTINUE;
}

nue_app_result on_frame(void*) {
    nue_frame_mark();
    // device lost / reset: the D3D9 TestCooperativeLevel pattern
    switch (nue_gfx_get_state(g.gfx)) {
        case NUE_GFX_STATE_NEEDS_RESET:
            destroy_gpu_objects();
            if (NUE_FAILED(nue_gfx_reset_device(g.gfx, NUE_FALSE)) || !create_gpu_objects()) return NUE_APP_QUIT_FAILURE;
            ++g.resets;
            break;
        case NUE_GFX_STATE_SUSPENDED: case NUE_GFX_STATE_DEVICE_LOST:
            nue_sleep_ns(16000000, NUE_SLEEP_COARSE);                                     // nothing to draw into
            return NUE_APP_CONTINUE;
        default: break;
    }
    nue_gfx_native nat = nuepp::init<nue_gfx_native>();
    nue_gfx_get_native(g.gfx, &nat);
    const int w = nat.pixel_width, h = nat.pixel_height;
    const float t = float(double(nue_time_ns() - g.start_ns) * 1e-9);

    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x203040ffu, 1.0f, 0);
    bgfx::setViewRect(0, 0, 0, uint16_t(w), uint16_t(h));
    hello_gfx::Pipeline::set_view_2d(0, w, h);
    bgfx::touch(0);

    const float cx = float(w) * 0.30f, cy = float(h) * 0.5f, r = float(h) * 0.25f;
    float tri[6];
    for (int i = 0; i < 3; ++i) { const float a = t + float(i) * 2.0943951f; tri[i * 2] = cx + std::cos(a) * r; tri[i * 2 + 1] = cy + std::sin(a) * r; }
    const uint32_t colours[3] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u};   // abgr: red, green, blue
    g.pipe.triangle(0, tri, colours);
    const float side = float(h) * 0.4f;
    g.pipe.quad(0, float(w) * 0.55f, cy - side * 0.5f, side, side, 0xffffffffu, g.pipe.checker);

    uint32_t frame = 0;
    const nue_result pr = nue_gfx_present(g.gfx, &frame);
    if (pr == NUE_ERR_DEVICE_LOST) return NUE_APP_CONTINUE;              // reset at the top of the next frame
    if (NUE_FAILED(pr)) return NUE_APP_QUIT_FAILURE;
    ++g.frames;
    if (g.frames_wanted && g.frames >= g.frames_wanted) return NUE_APP_QUIT_OK;
    return NUE_APP_CONTINUE;
}

void on_quit(void*, nue_result) {
    if (g.gfx) {
        destroy_gpu_objects();
        nue_gfx_destroy(g.gfx);
        g.gfx = nullptr;
    }
    if (g.window) { nue_window_destroy(g.window); g.window = nullptr; }
    std::printf("hello_gfx: %d frames, %d device resets\n", g.frames, g.resets);
}

}  // namespace

int NUE_CALL nue_main(int argc, char** argv) {
    g_argc = argc; g_argv = argv;
    bool noop = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--noop") == 0) noop = true;
    nue_init_desc init = nuepp::init<nue_init_desc>();
    init.app_name = "NUE hello_gfx"; init.app_id = "hello-gfx"; init.org_name = "NUE";
    init.log_level = NUE_LOG_INFO;
    if (noop) { init.backend = NUE_BACKEND_NULL; init.flags |= NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE; }   // CI: no window, no GPU
    nue_app_callbacks cb = nuepp::init<nue_app_callbacks>();
    cb.on_init = on_init; cb.on_event = on_event; cb.on_frame = on_frame; cb.on_quit = on_quit;
    return nue_app_run(&init, &cb, argc, argv);
}
