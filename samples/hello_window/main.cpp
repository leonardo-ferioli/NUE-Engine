// hello_window: opens a window with the engine-owned loop (nue_app_run) through the portable entry point.
// Closes on the window close button or Escape. Runs on the real SDL3 backend: it DOES open a window, so it is never run by the tests.
#include "nue/entry.h"
#include "nuepp/nue.hpp"

#include <cstdio>

namespace {
nuepp::Window g_window;

nue_result on_init(void*, int, char**) {
    nue_window_desc d = nuepp::init<nue_window_desc>();
    d.title = "NUE hello_window";
    d.width = 1280; d.height = 720;
    d.flags = NUE_WINDOW_RESIZABLE | NUE_WINDOW_HIGH_DPI;
    nuepp::Result r = g_window.create(d);
    if (!r) { std::fprintf(stderr, "window: %s\n", r.describe().c_str()); return r.code(); }
    nue_size_i px = g_window.pixel_size();
    NUE_LOGI("sample", "window %dx%d pixels, scale %.2f, refresh %.1f Hz", px.w, px.h, double(g_window.content_scale()), double(g_window.refresh_rate()));
    return NUE_OK;
}

nue_app_result on_event(void*, const nue_event* e) {
    if (e->type == NUE_EVENT_QUIT || e->type == NUE_EVENT_WINDOW_CLOSE_REQUESTED) return NUE_APP_QUIT_OK;
    if (e->type == NUE_EVENT_KEY_DOWN && e->payload.key.scancode == NUE_SC_ESCAPE) return NUE_APP_QUIT_OK;
    if (e->type == NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED) NUE_LOGI("sample", "resized to %dx%d", e->payload.window.x, e->payload.window.y);
    return NUE_APP_CONTINUE;
}

nue_app_result on_frame(void*) {
    nue_frame_mark();
    nue_frame_limiter_wait(60.0);   // a real renderer would present with vsync instead
    return NUE_APP_CONTINUE;
}

void on_quit(void*, nue_result) { g_window.reset(); }
}  // namespace

int NUE_CALL nue_main(int argc, char** argv) {
    nue_init_desc init = nuepp::init<nue_init_desc>();
    init.app_name = "NUE hello_window"; init.app_id = "hello-window"; init.org_name = "NUE";
    init.log_level = NUE_LOG_INFO;
    nue_app_callbacks cb = nuepp::init<nue_app_callbacks>();
    cb.on_init = on_init; cb.on_event = on_event; cb.on_frame = on_frame; cb.on_quit = on_quit;
    return nue_app_run(&init, &cb, argc, argv);
}
