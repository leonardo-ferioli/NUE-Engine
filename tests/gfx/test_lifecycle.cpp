// nue_gfx without a GPU: renderer names and selection, PREPARE mode, adopted native windows, the suspend / device-lost / reset state machine,
// listeners, shader cache directory, GPU memory budget with eviction hooks. Runs on the null backend, with and without -DNUE_WITH_GFX=ON
// (with it the NOOP renderer goes through bgfx's Noop backend, without it the context is bookkeeping only).
#include "nue_test.h"
#include "test_env.h"

#include <algorithm>
#include <filesystem>

namespace {

struct Seen {
    int resized = 0, suspended = 0, resumed = 0, lost = 0, reset = 0, destroying = 0, vsync = 0;
    int32_t w = 0, h = 0;
    int32_t generation = 0;
};

void NUE_CALL listener(void* ud, nue_gfx, nue_gfx_notification what, int32_t a, int32_t b) {
    Seen* s = static_cast<Seen*>(ud);
    switch (what) {
        case NUE_GFX_NOTIFY_RESIZED: ++s->resized; s->w = a; s->h = b; break;
        case NUE_GFX_NOTIFY_SUSPENDED: ++s->suspended; break;
        case NUE_GFX_NOTIFY_RESUMED: ++s->resumed; break;
        case NUE_GFX_NOTIFY_DEVICE_LOST: ++s->lost; break;
        case NUE_GFX_NOTIFY_DEVICE_RESET: ++s->reset; s->generation = a; break;
        case NUE_GFX_NOTIFY_DESTROYING: ++s->destroying; break;
        case NUE_GFX_NOTIFY_VSYNC_CHANGED: ++s->vsync; break;
    }
}

nue_window make_window(int w = 800, int h = 600) {
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.width = w; wd.height = h;
    nue_window win = nullptr;
    nue_window_create(&wd, &win);
    return win;
}

nue_gfx make_prepare(nue_window w, nue_gfx_flags extra = 0) {
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.window = w;
    gd.flags = NUE_GFX_PREPARE_ONLY | extra;
    nue_gfx g = nullptr;
    nue_gfx_create(&gd, &g);
    return g;
}

}  // namespace

TEST_CASE(renderer_names_and_list) {
    nue_test::Engine e;
    CHECK_STR(nue_renderer_name(NUE_RENDERER_D3D11), "Direct3D 11");
    CHECK_STR(nue_renderer_name(NUE_RENDERER_GLES), "OpenGL ES");
    nue_renderer list[8];
    uint32_t n = 0;
    CHECK_EQ(nue_gfx_list_renderers(list, 8, &n), NUE_OK);
    CHECK(n >= 1 && n <= 8);
    CHECK_EQ(list[n - 1], NUE_RENDERER_NOOP);          // NOOP is always last: tests, servers
    uint32_t total = 0;
    CHECK_EQ(nue_gfx_list_renderers(nullptr, 0, &total), NUE_OK);
    CHECK_EQ(total, n);
}

TEST_CASE(config_overrides_the_renderer_choice) {
    nue_test::Engine e;
    CHECK_EQ(nue_config_set_string("gfx.renderer", "noop"), NUE_OK);
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);                              // renderer AUTO, no window: only legal because the config picks NOOP
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    CHECK_EQ(nue_gfx_get_renderer(g), NUE_RENDERER_NOOP);
    nue_gfx_destroy(g);
    // a list: unknown names are skipped with a warning, the first usable one wins
    CHECK_EQ(nue_config_set_string("gfx.renderer", "bogus, NoOp ,vulkan"), NUE_OK);
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    CHECK_EQ(nue_gfx_get_renderer(g), NUE_RENDERER_NOOP);
    nue_gfx_destroy(g);
    // an explicit renderer in the descriptor wins over the config
    nue_config_set_string("gfx.renderer", "vulkan");
    gd.renderer = NUE_RENDERER_NOOP;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    CHECK_EQ(nue_gfx_get_renderer(g), NUE_RENDERER_NOOP);
    nue_gfx_destroy(g);
    // flags are overridable too
    nue_config_set_string("gfx.renderer", "noop");
    nue_config_set_bool("gfx.single_threaded", NUE_TRUE);
    gd.renderer = NUE_RENDERER_AUTO;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    CHECK_EQ(nue_gfx_get_native(g, &nat), NUE_OK);
    CHECK(nat.single_threaded);
    nue_gfx_destroy(g);
}

TEST_CASE(arguments_are_validated) {
    nue_test::Engine e;
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(nullptr, &g), NUE_ERR_INVALID_ARGUMENT);
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.renderer = NUE_RENDERER_D3D11;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_ERR_INVALID_ARGUMENT);          // a real renderer needs a surface
    nue_native_window nw;
    NUE_STRUCT_INIT(&nw);
    nw.kind = NUE_NATIVE_WIN32_HWND; nw.window = 0x1234;
    gd.native_window = &nw;
    gd.flags = NUE_GFX_PREPARE_ONLY;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_ERR_INVALID_ARGUMENT);          // an adopted window needs its size
    CHECK_EQ(nue_gfx_resize(nullptr, 1, 1), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_gfx_get_state(nullptr), NUE_GFX_STATE_ORPHANED);
}

TEST_CASE(adopted_native_window_is_lent_never_owned) {
    nue_test::Engine e;
    nue_native_window nw;
    NUE_STRUCT_INIT(&nw);
    nw.kind = NUE_NATIVE_WIN32_HWND; nw.window = 0x00ABCDEF;               // a window the game created (not a real handle: PREPARE mode never touches it)
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.native_window = &nw;
    gd.width = 640; gd.height = 480;
    gd.flags = NUE_GFX_PREPARE_ONLY;
    gd.renderer = NUE_RENDERER_D3D11;
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    CHECK_EQ(nue_gfx_get_native(g, &nat), NUE_OK);
    CHECK_EQ(nat.window.kind, NUE_NATIVE_WIN32_HWND);
    CHECK_EQ(nat.platform_nwh, uint64_t(0x00ABCDEF));
    CHECK_EQ(nat.platform_ndt, uint64_t(0));
    CHECK_EQ(nat.pixel_width, 640);
    CHECK_EQ(nue_gfx_resize(g, 0, 0), NUE_ERR_INVALID_ARGUMENT);         // nothing to follow
    CHECK_EQ(nue_gfx_resize(g, 1024, 768), NUE_OK);
    CHECK_EQ(nue_gfx_resize(g, 1024, 768), NUE_OK_NOOP);
    nue_gfx_destroy(g);                                                   // the handle is not ours: nothing to destroy, nothing crashes

    // X11 and Wayland map display and window the way bgfx PlatformData wants them
    nw.kind = NUE_NATIVE_X11_WINDOW; nw.window = 77; nw.display = 0x1000;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    CHECK_EQ(nat.platform_ndt, uint64_t(0x1000));
    CHECK_EQ(nat.platform_nwh, uint64_t(77));
    nue_gfx_destroy(g);
    nw.kind = NUE_NATIVE_WAYLAND_SURFACE; nw.window = 0x2000; nw.display = 0x3000; nw.extra = 0;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    CHECK_EQ(nat.platform_ndt, uint64_t(0x3000));
    CHECK_EQ(nat.platform_nwh, uint64_t(0x2000));
    nue_gfx_destroy(g);
    nw.kind = NUE_NATIVE_UIKIT_WINDOW; nw.window = 0x5000; nw.display = 0; nw.extra = 0x6000;             // extra = CAMetalLayer*
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    CHECK_EQ(nat.platform_nwh, uint64_t(0x6000));
    nue_gfx_destroy(g);
}

TEST_CASE(suspend_resume_state_and_listeners) {
    nue_test::Engine e;
    nue_window w = make_window();
    nue_gfx g = make_prepare(w);
    Seen seen;
    nue_gfx_add_listener(g, listener, &seen);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_OK);
    CHECK_EQ(nue_gfx_suspend(g), NUE_OK);
    CHECK_EQ(nue_gfx_suspend(g), NUE_OK_NOOP);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_SUSPENDED);
    CHECK_EQ(seen.suspended, 1);
    CHECK_EQ(nue_gfx_resume(g), NUE_OK);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_OK);
    CHECK_EQ(seen.resumed, 1);
    CHECK_EQ(nue_gfx_set_vsync(g, NUE_TRUE), NUE_OK);
    CHECK_EQ(nue_gfx_set_vsync(g, NUE_TRUE), NUE_OK_NOOP);
    CHECK_EQ(seen.vsync, 1);
    // surface events drive the same machine
    nue_event ev;
    std::memset(&ev, 0, sizeof ev);
    ev.type = NUE_EVENT_SURFACE_LOST;
    ev.window_id = nue_window_get_id(w);
    CHECK_EQ(nue_gfx_handle_event(g, &ev), NUE_OK);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_SUSPENDED);
    ev.type = NUE_EVENT_SURFACE_CREATED;
    CHECK_EQ(nue_gfx_handle_event(g, &ev), NUE_OK);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_OK);
    // a listener that is removed hears nothing more
    CHECK_EQ(nue_gfx_remove_listener(g, listener, &seen), NUE_OK);
    CHECK_EQ(nue_gfx_remove_listener(g, listener, &seen), NUE_ERR_NOT_FOUND);
    nue_gfx_suspend(g);
    CHECK_EQ(seen.suspended, 2);
    nue_gfx_destroy(g);
    nue_window_destroy(w);
}

TEST_CASE(device_lost_and_reset_follow_testcooperativelevel) {
    nue_test::Engine e;
    nue_window w = make_window();
    nue_gfx g = make_prepare(w);
    Seen seen;
    nue_gfx_add_listener(g, listener, &seen);
    CHECK_EQ(nue_gfx_reset_device(g, NUE_FALSE), NUE_OK_NOOP);            // nothing to reset: D3D_OK
    CHECK_EQ(nue_gfx_report_device_lost(g), NUE_OK);
    CHECK_EQ(nue_gfx_report_device_lost(g), NUE_OK);                       // repeated reports notify once
    CHECK_EQ(seen.lost, 1);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_NEEDS_RESET);            // D3DERR_DEVICENOTRESET
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    const uint32_t gen = nat.generation;
    CHECK_EQ(nue_gfx_reset_device(g, NUE_FALSE), NUE_OK);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_OK);
    CHECK_EQ(seen.reset, 1);
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    CHECK_EQ(nat.generation, gen + 1);
    CHECK_EQ(seen.generation, int32_t(gen + 1));
    // lost while suspended: still D3DERR_DEVICELOST, the reset waits for the surface
    nue_gfx_suspend(g);
    nue_gfx_report_device_lost(g);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_DEVICE_LOST);
    CHECK_EQ(nue_gfx_reset_device(g, NUE_FALSE), NUE_ERR_DEVICE_LOST);
    nue_gfx_resume(g);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_NEEDS_RESET);
    CHECK_EQ(nue_gfx_reset_device(g, NUE_FALSE), NUE_OK);
    CHECK_EQ(nue_gfx_reset_device(g, NUE_TRUE), NUE_OK);                   // force: recreate although nothing was lost
    nue_gfx_destroy(g);
    nue_window_destroy(w);
}

TEST_CASE(window_destroyed_orphans_the_context) {
    nue_test::Engine e;
    nue_window w = make_window();
    nue_gfx g = make_prepare(w);
    Seen seen;
    nue_gfx_add_listener(g, listener, &seen);
    nue_window_destroy(w);
    CHECK_EQ(seen.destroying, 1);
    CHECK_EQ(nue_gfx_get_state(g), NUE_GFX_STATE_ORPHANED);
    CHECK_EQ(nue_gfx_resize(g, 10, 10), NUE_ERR_BAD_STATE);
    CHECK_EQ(nue_gfx_reset_device(g, NUE_TRUE), NUE_ERR_BAD_STATE);
    nue_gfx_destroy(g);
    CHECK_EQ(seen.destroying, 1);
}

TEST_CASE(resize_follows_the_window_until_a_size_is_given) {
    nue_test::Engine e;
    nue_window w = make_window(800, 600);
    nue_gfx g = make_prepare(w);
    Seen seen;
    nue_gfx_add_listener(g, listener, &seen);
    nue_gfx_set_auto_events(g, NUE_TRUE);
    nue_null_resize_window(w, 1280, 720);
    CHECK_EQ(seen.resized, 1);
    CHECK_EQ(seen.w, 1280);
    CHECK_EQ(nue_gfx_resize(g, 640, 360), NUE_OK);
    nue_null_resize_window(w, 1000, 600);
    CHECK_EQ(seen.w, 640);                                                // an explicit size stops following
    CHECK_EQ(nue_gfx_resize(g, 0, 0), NUE_OK);                            // 0,0 follows again
    CHECK_EQ(seen.w, 1000);
    CHECK_EQ(nue_gfx_resize(g, 5, 0), NUE_ERR_INVALID_ARGUMENT);
    nue_gfx_destroy(g);
    nue_window_destroy(w);
}

TEST_CASE(shader_cache_directory_is_resolved_and_created) {
    nue_test::Engine e;
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.renderer = NUE_RENDERER_NOOP;
    char path[512];
    size_t need = 0;
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    CHECK_EQ(nue_gfx_get_shader_cache_dir(g, path, sizeof path, &need), NUE_ERR_NOT_FOUND);   // none requested
    nue_gfx_destroy(g);
    gd.shader_cache_dir = "cache:shaders/d3d11";
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    CHECK_EQ(nue_gfx_get_shader_cache_dir(g, path, sizeof path, &need), NUE_OK);
    CHECK(need > 1);
    CHECK(std::filesystem::is_directory(path));
    CHECK(std::string(path).find("shaders/d3d11") != std::string::npos);
    nue_gfx_destroy(g);
}

namespace {
struct Pool {
    nue_gfx gfx = nullptr;
    int64_t held = 0;
    int calls = 0;
    uint32_t last_level = 0;
};
uint64_t NUE_CALL evict_cb(void* ud, nue_gfx g, uint64_t needed, uint32_t level) {
    Pool* p = static_cast<Pool*>(ud);
    ++p->calls;
    p->last_level = level;
    const uint64_t give = needed ? std::min<uint64_t>(uint64_t(p->held), needed) : uint64_t(p->held);
    p->held -= int64_t(give);
    nue_gfx_memory_report(g, -int64_t(give));       // the hook accounts what it frees
    return give;
}
}  // namespace

TEST_CASE(gpu_memory_goes_through_the_texture_budget_with_eviction) {
    nue_test::Engine e;
    nue_config_set_int("budget.texture.limit_mb", 1);          // 1 MiB: the policy is configuration, not a constant
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.renderer = NUE_RENDERER_NOOP;
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    Pool pool;
    pool.gfx = g;
    CHECK_EQ(nue_gfx_add_evictor(g, evict_cb, &pool), NUE_OK);
    CHECK_EQ(nue_gfx_memory_request(g, 700 * 1024), NUE_OK);
    pool.held += 700 * 1024;
    nue_budget_id id = 0;
    CHECK_EQ(nue_budget_find("texture", &id), NUE_OK);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.used_bytes, uint64_t(700 * 1024));
    CHECK_EQ(st.limit_bytes, uint64_t(1024 * 1024));
    CHECK_EQ(pool.calls, 0);
    CHECK_EQ(nue_gfx_memory_request(g, 600 * 1024), NUE_OK);   // does not fit: the evictor runs, then it does
    pool.held += 600 * 1024;
    CHECK(pool.calls >= 1);
    CHECK_EQ(nue_gfx_memory_request(g, 5 * 1024 * 1024), NUE_ERR_OVERFLOW);   // bigger than the whole budget
    nue_gfx_memory_report(g, -pool.held);                      // the client destroyed its resources
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.used_bytes, uint64_t(0));
    // memory pressure reaches the hook with the level
    pool.held = 300 * 1024;
    nue_gfx_memory_report(g, pool.held);
    pool.calls = 0;
    nue_budget_trim(NUE_PRESSURE_HIGH);
    CHECK(pool.calls >= 1);
    CHECK_EQ(pool.last_level, uint32_t(NUE_PRESSURE_HIGH));
    CHECK_EQ(nue_gfx_remove_evictor(g, evict_cb, &pool), NUE_OK);
    CHECK_EQ(nue_gfx_remove_evictor(g, evict_cb, &pool), NUE_ERR_NOT_FOUND);
    nue_gfx_destroy(g);
}

TEST_CASE(noop_context_presents_and_honours_suspension) {
    nue_test::Engine e;
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.renderer = NUE_RENDERER_NOOP;
    gd.width = 320; gd.height = 200;
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    uint32_t frame = 0;
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_OK);
    CHECK_EQ(frame, 1u);
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_OK);
    CHECK_EQ(frame, 2u);
    nue_gfx_suspend(g);
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_OK_NOOP);                    // nothing is presented while the surface is gone
    CHECK_EQ(frame, 2u);
    nue_gfx_resume(g);
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_OK);
    CHECK_EQ(frame, 3u);
    nue_gfx_report_device_lost(g);
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_ERR_DEVICE_LOST);            // D3DERR_DEVICENOTRESET until reset
    CHECK_EQ(nue_gfx_reset_device(g, NUE_FALSE), NUE_OK);
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_OK);
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    nue_gfx_get_native(g, &nat);
    CHECK_EQ(nat.pixel_width, 320);
    CHECK_EQ(nat.renderer, NUE_RENDERER_NOOP);
#if defined(NUE_HAS_GFX)
    CHECK(nat.bgfx_initialized);                                          // bgfx's Noop backend is really running
    nue_gfx_caps caps;
    NUE_STRUCT_INIT(&caps);
    CHECK_EQ(nue_gfx_get_caps(g, &caps), NUE_OK);
    CHECK(caps.max_texture_size > 0);
#else
    CHECK(!nat.bgfx_initialized);
#endif
    nue_gfx_destroy(g);
}

TEST_CASE(texture_caps_come_from_the_probe_without_a_device) {
    nue_test::Engine e;
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.renderer = NUE_RENDERER_NOOP;
    nue_gfx g = nullptr;
    nue_gfx_create(&gd, &g);
    nue_tex_caps tc;
    NUE_STRUCT_INIT(&tc);
    CHECK(NUE_SUCCEEDED(nue_gfx_get_tex_caps(g, &tc)));
    CHECK(tc.max_size_2d > 0);
    CHECK(tc.format_flags[NUE_TEX_RGBA8_UNORM] & NUE_TEX_FCAP_SAMPLE);
    nue_gfx_destroy(g);
}

TEST_MAIN_BEGIN
    RUN(renderer_names_and_list);
    RUN(config_overrides_the_renderer_choice);
    RUN(arguments_are_validated);
    RUN(adopted_native_window_is_lent_never_owned);
    RUN(suspend_resume_state_and_listeners);
    RUN(device_lost_and_reset_follow_testcooperativelevel);
    RUN(window_destroyed_orphans_the_context);
    RUN(resize_follows_the_window_until_a_size_is_given);
    RUN(shader_cache_directory_is_resolved_and_created);
    RUN(gpu_memory_goes_through_the_texture_budget_with_eviction);
    RUN(noop_context_presents_and_honours_suspension);
    RUN(texture_caps_come_from_the_probe_without_a_device);
TEST_MAIN_END
