// OPTIONAL local test (-DNUE_GFX_LOCAL_TESTS=ON, Windows, needs a GPU; never run in CI): the real Direct3D 11 renderer on a HIDDEN window.
// The window is created with NUE_WINDOW_HIDDEN, is never shown and never fullscreen; frames are read back with nue_gfx_request_capture and checked pixel by pixel.
// Covers: renderer choice, GPU caps from the live renderer, clear + draw + present, capture, resize, suspend/resume, device reset and the texture budget.
#include "nue_test.h"
#include "test_env.h"

#include "../../../samples/hello_gfx/draw.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace {

struct Capture {
    bool got = false;
    int w = 0, h = 0, pitch = 0;
    std::vector<uint8_t> px;
};

void NUE_CALL on_capture(void* ud, int32_t w, int32_t h, int32_t pitch, const void* bgra) {
    Capture* c = static_cast<Capture*>(ud);
    c->w = w; c->h = h; c->pitch = pitch;
    c->px.assign(static_cast<const uint8_t*>(bgra), static_cast<const uint8_t*>(bgra) + size_t(pitch) * h);
    c->got = true;
}

struct Rgb { int r, g, b; };
Rgb at(const Capture& c, int x, int y) {
    const uint8_t* p = c.px.data() + size_t(y) * c.pitch + size_t(x) * 4;   // BGRA
    return {p[2], p[1], p[0]};
}
bool near(Rgb a, Rgb b, int tol = 3) { return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol; }

// One frame: clear, a red quad in the middle, present. `want_capture` asks for the frame back.
void frame(nue_gfx g, hello_gfx::Pipeline& pipe, Capture* cap) {
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x4080c0ffu, 1.0f, 0);   // RGBA: (0x40, 0x80, 0xc0)
    bgfx::setViewRect(0, 0, 0, uint16_t(nat.pixel_width), uint16_t(nat.pixel_height));
    hello_gfx::Pipeline::set_view_2d(0, nat.pixel_width, nat.pixel_height);
    bgfx::touch(0);
    const float w = float(nat.pixel_width), h = float(nat.pixel_height);
    pipe.quad(0, w * 0.25f, h * 0.25f, w * 0.5f, h * 0.5f, 0xff0000ffu /* abgr: red */, pipe.white);
    if (cap) { cap->got = false; nue_gfx_request_capture(g, on_capture, cap); }
    uint32_t n = 0;
    nue_gfx_present(g, &n);
}

// Frames until the capture callback has fired (bgfx delivers it a couple of frames later).
bool wait_capture(nue_gfx g, hello_gfx::Pipeline& pipe, Capture& cap) {
    for (int i = 0; i < 10 && !cap.got; ++i) frame(g, pipe, nullptr);
    return cap.got;
}

}  // namespace

TEST_CASE(d3d11_on_a_hidden_window) {
    const std::string root = nue_test::unique_temp_dir("gfx_d3d11");
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    d.api_version = NUE_API_VERSION;
    d.app_name = "NUE gfx local test"; d.app_id = "nue-gfx-local"; d.org_name = "NUE";
    d.backend = NUE_BACKEND_SDL3;
    d.flags = NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_LOG_FILE;                 // NOT headless: the window needs a real HWND
    d.game_data_path = root.c_str();
    const std::string user = root + "/user";
    d.user_data_path = user.c_str();
    d.log_level = NUE_LOG_INFO;
    CHECK(NUE_SUCCEEDED(nue_init(&d)));

    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.title = "nue gfx hidden test";
    wd.width = 320; wd.height = 240;
    wd.flags = NUE_WINDOW_HIDDEN | NUE_WINDOW_RESIZABLE;                           // hidden: nothing appears on the desktop
    nue_window w = nullptr;
    CHECK(NUE_SUCCEEDED(nue_window_create(&wd, &w)));
    CHECK(!nue_window_is_visible(w));
    CHECK_EQ(nue_window_get_mode(w), NUE_WINDOW_MODE_WINDOWED);

    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.window = w;
    gd.renderer = NUE_RENDERER_D3D11;                                             // strict: no fallback
    gd.flags = NUE_GFX_CAPTURE | NUE_GFX_DEBUG;
    gd.shader_cache_dir = "cache:shaders";
    nue_gfx g = nullptr;
    nue_result r = nue_gfx_create(&gd, &g);
    if (NUE_FAILED(r)) { std::printf("  nue_gfx_create failed: %s\n", nue_get_last_error_message()); }
    CHECK(NUE_SUCCEEDED(r));
    if (!g) { nue_window_destroy(w); nue_shutdown(); return; }
    CHECK_EQ(nue_gfx_get_renderer(g), NUE_RENDERER_D3D11);
    CHECK(!nue_window_is_visible(w));

    // live capabilities
    nue_gfx_caps caps;
    NUE_STRUCT_INIT(&caps);
    CHECK(NUE_SUCCEEDED(nue_gfx_get_caps(g, &caps)));
    std::printf("  renderer '%s', adapter '%s', vendor 0x%04x, max texture %u\n", caps.renderer_name, caps.adapter_name, caps.vendor_id, caps.max_texture_size);
    CHECK(std::string(caps.renderer_name).find("Direct3D 11") != std::string::npos);
    CHECK(caps.max_texture_size >= 4096);
    nue_gpu_caps gc;
    NUE_STRUCT_INIT(&gc);
    CHECK_EQ(nue_gfx_get_gpu_caps(g, &gc), NUE_OK);
    CHECK(gc.probed);
    CHECK_EQ(gc.renderer, NUE_RENDERER_D3D11);
    CHECK(gc.format[NUE_FMT_RGBA8] & NUE_FMTSUP_RENDER_TARGET);
    CHECK(gc.format[NUE_FMT_BC1] & NUE_FMTSUP_SAMPLE);                             // every desktop GPU samples BCn
    CHECK(!(gc.format[NUE_FMT_BC1] & NUE_FMTSUP_EMULATED));
    std::printf("  BC1 %s, BC7 %s, ASTC %s, ETC2 %s, max texture %u, msaa %u\n", (gc.format[NUE_FMT_BC1] & NUE_FMTSUP_SAMPLE) ? "yes" : "no",
                (gc.format[NUE_FMT_BC7] & NUE_FMTSUP_SAMPLE) ? "yes" : "no", (gc.format[NUE_FMT_ASTC_LDR] & NUE_FMTSUP_SAMPLE) ? "yes" : "no",
                (gc.format[NUE_FMT_ETC2_RGB] & NUE_FMTSUP_SAMPLE) ? "yes" : "no", gc.max_texture_2d, gc.max_msaa_samples);
    nue_tex_caps tc;
    NUE_STRUCT_INIT(&tc);
    CHECK(NUE_SUCCEEDED(nue_gfx_get_tex_caps(g, &tc)));
    CHECK(nue_tex_caps_can_use(&tc, NUE_TEX_BC1_UNORM));

    // memory: the texture budget sees the resources
    CHECK_EQ(nue_gfx_memory_request(g, 4 + 8 * 8 * 4), NUE_OK);
    hello_gfx::Pipeline pipe;
    CHECK(pipe.init());

    // clear + draw + capture
    Capture cap;
    frame(g, pipe, nullptr);
    frame(g, pipe, &cap);
    CHECK(wait_capture(g, pipe, cap));
    CHECK_EQ(cap.w, 320);
    CHECK_EQ(cap.h, 240);
    if (cap.got && cap.w == 320 && cap.h == 240) {
        CHECK(near(at(cap, 4, 4), {0x40, 0x80, 0xc0}));                            // clear colour in the corner
        CHECK(near(at(cap, 160, 120), {0xff, 0x00, 0x00}));                        // red quad in the middle
        CHECK(near(at(cap, 316, 236), {0x40, 0x80, 0xc0}));
    }

    // resize: the swap chain follows (render thread = this thread: applied at once)
    CHECK_EQ(nue_gfx_resize(g, 200, 150), NUE_OK);
    frame(g, pipe, &cap);
    CHECK(wait_capture(g, pipe, cap));
    CHECK_EQ(cap.w, 200);
    CHECK_EQ(cap.h, 150);
    if (cap.got && cap.w == 200 && cap.h == 150) CHECK(near(at(cap, 100, 75), {0xff, 0x00, 0x00}));

    // suspend / resume: nothing is presented while suspended
    CHECK_EQ(nue_gfx_suspend(g), NUE_OK);
    uint32_t n = 0;
    CHECK_EQ(nue_gfx_present(g, &n), NUE_OK_NOOP);
    CHECK_EQ(nue_gfx_resume(g), NUE_OK);
    frame(g, pipe, &cap);
    CHECK(wait_capture(g, pipe, cap));

    // device reset: GPU objects are gone, the generation moves, drawing works again after recreating them
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    const uint32_t gen = nat.generation;
    pipe.shutdown();
    CHECK_EQ(nue_gfx_reset_device(g, NUE_TRUE), NUE_OK);
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    CHECK_EQ(nat.generation, gen + 1);
    CHECK(nat.bgfx_initialized);
    CHECK(pipe.init());
    frame(g, pipe, &cap);
    CHECK(wait_capture(g, pipe, cap));
    if (cap.got) CHECK(near(at(cap, cap.w / 2, cap.h / 2), {0xff, 0x00, 0x00}));

    // the shader / pipeline cache directory exists and nothing leaked into the budget when everything is released
    char dir[512];
    size_t need = 0;
    CHECK_EQ(nue_gfx_get_shader_cache_dir(g, dir, sizeof dir, &need), NUE_OK);
    CHECK(std::filesystem::is_directory(dir));
    pipe.shutdown();
    nue_gfx_memory_report(g, -int64_t(4 + 8 * 8 * 4));
    nue_budget_id id = 0;
    CHECK_EQ(nue_budget_find("texture", &id), NUE_OK);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.used_bytes, uint64_t(0));

    CHECK(!nue_window_is_visible(w));                                              // it never appeared
    nue_gfx_destroy(g);
    nue_window_destroy(w);
    nue_shutdown();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_MAIN_BEGIN
    RUN(d3d11_on_a_hidden_window);
TEST_MAIN_END
