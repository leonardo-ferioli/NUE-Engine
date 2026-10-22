// Event queue, watchers, lifecycle, windows/displays, input state, last-used device, power events, gfx lifecycle (all on the null backend).
#include "test_env.h"
#include "nue_test.h"

#include <cstring>
#include <vector>

using nue_test::Engine;

static std::vector<uint32_t> types_of(int max = 64) {
    std::vector<nue_event> ev(static_cast<size_t>(max));
    int n = nue_test::drain(ev.data(), max);
    std::vector<uint32_t> t;
    for (int i = 0; i < n; ++i) t.push_back(ev[size_t(i)].type);
    return t;
}

TEST_CASE(queue_order_flush_disable_peek) {
    Engine e;
    nue_push_user_event(1, 10, 0);
    nue_push_user_event(2, 20, 0);
    nue_push_user_event(3, 30, 0);
    nue_event peek[2];
    CHECK_EQ(nue_peek_events(peek, 2), 2);
    CHECK_EQ(peek[0].payload.user.code, 1);
    nue_event ev;
    CHECK(nue_poll_event(&ev));
    CHECK_EQ(ev.payload.user.code, 1);
    CHECK(ev.timestamp_ns != 0);
    nue_set_event_enabled(NUE_EVENT_USER, NUE_FALSE);
    nue_push_user_event(4, 0, 0);
    nue_set_event_enabled(NUE_EVENT_USER, NUE_TRUE);
    CHECK(nue_poll_event(&ev)); CHECK_EQ(ev.payload.user.code, 2);
    CHECK(nue_poll_event(&ev)); CHECK_EQ(ev.payload.user.code, 3);
    CHECK(!nue_poll_event(&ev));   // 4 was never queued
    nue_push_user_event(5, 0, 0);
    nue_flush_events(NUE_EVENT_USER, NUE_EVENT_USER + 0xFFFF);
    CHECK(!nue_poll_event(&ev));
}

TEST_CASE(queue_capacity_drops_oldest_motion_first) {
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    std::string dir = nue_test::unique_temp_dir("cap");
    d.api_version = NUE_API_VERSION; d.backend = NUE_BACKEND_NULL; d.event_queue_capacity = 4; d.user_data_path = dir.c_str();
    d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD | NUE_INIT_DETERMINISTIC;
    d.log_level = NUE_LOG_ERROR;
    CHECK_EQ(nue_init(&d), NUE_OK);
    nue_push_user_event(1, 0, 0);
    nue_event m;
    std::memset(&m, 0, sizeof(m));
    m.type = NUE_EVENT_MOUSE_MOTION;
    nue_push_event(&m); nue_push_event(&m); nue_push_event(&m);   // queue full: 1 user + 3 motion
    nue_push_user_event(2, 0, 0);                                  // evicts the oldest motion
    nue_push_user_event(3, 0, 0);                                  // evicts another
    std::vector<uint32_t> t = types_of();
    CHECK_EQ(t.size(), 4u);
    int users = 0;
    for (uint32_t x : t) users += x == NUE_EVENT_USER;
    CHECK_EQ(users, 3);   // no user event was lost
    nue_shutdown();
    std::filesystem::remove_all(dir);
}

TEST_CASE(watcher_sees_first_and_can_swallow) {
    Engine e;
    static int seen, swallowed;
    seen = swallowed = 0;
    auto w = [](void*, const nue_event* ev) -> nue_bool {
        ++seen;
        if (ev->type == NUE_EVENT_USER && ev->payload.user.code == 99) { ++swallowed; return NUE_FALSE; }
        return NUE_TRUE;
    };
    CHECK_EQ(nue_event_add_watcher(w, nullptr), NUE_OK);
    nue_push_user_event(1, 0, 0);
    nue_push_user_event(99, 0, 0);
    CHECK_EQ(seen, 2);
    CHECK_EQ(swallowed, 1);
    nue_event ev;
    CHECK(nue_poll_event(&ev)); CHECK_EQ(ev.payload.user.code, 1);
    CHECK(!nue_poll_event(&ev));
    CHECK_EQ(nue_event_remove_watcher(w, nullptr), NUE_OK);
    CHECK_EQ(nue_event_remove_watcher(w, nullptr), NUE_ERR_NOT_FOUND);
}

TEST_CASE(wait_event_times_out_on_virtual_clock) {
    Engine e;
    nue_event ev;
    int64_t t0 = nue_time_ns();
    CHECK(!nue_wait_event(&ev, 50));
    CHECK(nue_time_ns() - t0 >= 50000000);
    nue_push_user_event(5, 0, 0);
    CHECK(nue_wait_event(&ev, 1000));
}

TEST_CASE(lifecycle_sequence_and_state) {
    Engine e;
    CHECK_EQ(nue_app_get_state(), NUE_APP_STATE_FOREGROUND);
    static int watcher_pausing;
    watcher_pausing = 0;
    nue_event_add_watcher([](void*, const nue_event* ev) -> nue_bool { watcher_pausing += ev->type == NUE_EVENT_APP_PAUSING; return NUE_TRUE; }, nullptr);
    nue_null_set_app_state(NUE_APP_STATE_BACKGROUND);
    CHECK_EQ(nue_app_get_state(), NUE_APP_STATE_BACKGROUND);
    CHECK_EQ(watcher_pausing, 1);   // delivered synchronously
    nue_null_set_app_state(NUE_APP_STATE_FOREGROUND);
    std::vector<uint32_t> t = types_of();
    CHECK_EQ(t.size(), 4u);
    CHECK_EQ(t[0], (uint32_t)NUE_EVENT_APP_PAUSING);
    CHECK_EQ(t[1], (uint32_t)NUE_EVENT_APP_PAUSED);
    CHECK_EQ(t[2], (uint32_t)NUE_EVENT_APP_RESUMING);
    CHECK_EQ(t[3], (uint32_t)NUE_EVENT_APP_RESUMED);
    nue_app_request_quit();
    CHECK(nue_app_quit_requested());
    CHECK(types_of().at(0) == NUE_EVENT_QUIT);
}

TEST_CASE(engine_loop_runs_callbacks_until_quit) {
    struct Ctx { int init = 0, frames = 0, quits = 0, events = 0; } c;
    nue_app_callbacks cb;
    NUE_STRUCT_INIT(&cb);
    cb.userdata = &c;
    cb.on_init = [](void* u, int, char**) -> nue_result { ++static_cast<Ctx*>(u)->init; return NUE_OK; };
    cb.on_event = [](void* u, const nue_event* ev) -> nue_app_result { ++static_cast<Ctx*>(u)->events; return ev->type == NUE_EVENT_QUIT ? NUE_APP_QUIT_OK : NUE_APP_CONTINUE; };
    cb.on_frame = [](void* u) -> nue_app_result {
        Ctx* c = static_cast<Ctx*>(u);
        if (++c->frames == 3) nue_app_request_quit();
        return NUE_APP_CONTINUE;
    };
    cb.on_quit = [](void* u, nue_result) { ++static_cast<Ctx*>(u)->quits; };
    std::string dir = nue_test::unique_temp_dir("loop");
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    d.api_version = NUE_API_VERSION; d.backend = NUE_BACKEND_NULL; d.user_data_path = dir.c_str();
    d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD | NUE_INIT_DETERMINISTIC;
    d.log_level = NUE_LOG_ERROR;
    int32_t code = nue_app_run(&d, &cb, 0, nullptr);
    CHECK_EQ(code, 0);
    CHECK_EQ(c.init, 1);
    CHECK_EQ(c.frames, 3);
    CHECK_EQ(c.quits, 1);
    CHECK(c.events >= 1);
    CHECK(!nue_is_initialized());
    std::filesystem::remove_all(dir);
}

TEST_CASE(displays_modes_and_windows) {
    Engine e;
    uint32_t n = 0;
    nue_display_id ids[4];
    CHECK_EQ(nue_display_list(ids, 4, &n), NUE_OK);
    CHECK_EQ(n, 1u);
    nue_display_info di;
    NUE_STRUCT_INIT(&di);
    CHECK_EQ(nue_display_get_info(nue_display_primary(), &di), NUE_OK);
    CHECK_EQ(di.bounds.w, 1920);
    uint32_t modes = 0;
    CHECK_EQ(nue_display_list_modes(di.id, nullptr, 0, &modes), NUE_OK);
    CHECK(modes > 3);
    nue_display_mode best;
    NUE_STRUCT_INIT(&best);
    CHECK_EQ(nue_display_find_mode(di.id, 1280, 700, 0, &best), NUE_OK);
    CHECK_EQ(best.width, 1280);

    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.title = "hello";
    wd.width = 800; wd.height = 600;
    wd.flags = NUE_WINDOW_RESIZABLE;
    nue_window w = nullptr;
    CHECK_EQ(nue_window_create(&wd, &w), NUE_OK);
    CHECK(w != nullptr);
    int32_t pw, ph;
    nue_window_get_pixel_size(w, &pw, &ph);
    CHECK_EQ(pw, 800);
    CHECK(nue_window_has_focus(w));
    char title[32]; size_t need;
    nue_window_get_title(w, title, sizeof(title), &need);
    CHECK_STR(title, "hello");
    nue_window_set_title(w, "renamed");
    nue_window_get_title(w, title, sizeof(title), &need);
    CHECK_STR(title, "renamed");

    nue_event ev[16];
    int count = nue_test::drain(ev, 16);
    bool saw_shown = false, saw_focus = false, saw_surface = false;
    for (int i = 0; i < count; ++i) { saw_shown |= ev[i].type == NUE_EVENT_WINDOW_SHOWN; saw_focus |= ev[i].type == NUE_EVENT_WINDOW_FOCUS_GAINED; saw_surface |= ev[i].type == NUE_EVENT_SURFACE_CREATED; }
    CHECK(saw_shown && saw_focus && saw_surface);

    // high DPI display: pixel size follows the scale, an event announces it
    nue_null_set_display(1920, 1080, 60.0f, 2.0f);
    nue_window_desc hd = wd;
    hd.flags |= NUE_WINDOW_HIGH_DPI;
    nue_window hw = nullptr;
    CHECK_EQ(nue_window_create(&hd, &hw), NUE_OK);
    nue_window_get_pixel_size(hw, &pw, &ph);
    CHECK_EQ(pw, 1600);
    CHECK_NEAR(nue_window_get_pixel_density(hw), 2.0, 1e-6);

    // resize announces logical and pixel size
    nue_flush_events(0, 0xFFFFFF);
    nue_null_resize_window(w, 1024, 768);
    std::vector<uint32_t> t = types_of();
    CHECK(t.size() == 2);
    nue_window_get_size(w, &pw, &ph);
    CHECK_EQ(pw, 1024);

    // modes
    CHECK_EQ(nue_window_set_mode(w, NUE_WINDOW_MODE_BORDERLESS, nullptr), NUE_OK);
    nue_window_get_size(w, &pw, &ph);
    CHECK_EQ(pw, 1920);
    CHECK_EQ(nue_window_get_mode(w), NUE_WINDOW_MODE_BORDERLESS);

    CHECK(nue_window_from_id(nue_window_get_id(w)) == w);
    nue_window_destroy(w);
    CHECK_EQ(nue_window_get_size(w, &pw, &ph), NUE_ERR_INVALID_HANDLE);   // stale handle is detected, not dereferenced
    nue_window_destroy(hw);
}

TEST_CASE(attach_native_window_is_not_resizable_by_us) {
    Engine e;
    nue_native_window nw;
    NUE_STRUCT_INIT(&nw);
    nw.kind = NUE_NATIVE_WIN32_HWND;
    nw.window = 0xABCD;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.attach = &nw;
    wd.width = 640; wd.height = 480;
    nue_window w;
    CHECK_EQ(nue_window_create(&wd, &w), NUE_OK);
    nue_native_window got;
    NUE_STRUCT_INIT(&got);
    CHECK_EQ(nue_window_get_native(w, &got), NUE_OK);
    CHECK_EQ(got.window, 0xABCDull);
    CHECK_EQ(nue_window_set_size(w, 100, 100), NUE_ERR_UNSUPPORTED);
    nue_window_destroy(w);
}

TEST_CASE(keyboard_mouse_state_and_focus_loss) {
    Engine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    nue_window w;
    nue_window_create(&wd, &w);
    nue_null_press_key(NUE_SC_W, NUE_TRUE);
    nue_null_press_key(NUE_SC_LSHIFT, NUE_TRUE);
    CHECK(nue_key_is_down(NUE_SC_W));
    CHECK((nue_key_get_mods() & NUE_KMOD_SHIFT) != 0);
    CHECK(nue_key_get_state_array()[NUE_SC_W] == 1);
    nue_null_press_key(NUE_SC_W, NUE_FALSE);
    CHECK(!nue_key_is_down(NUE_SC_W));
    nue_null_move_mouse(100, 50);
    nue_null_move_mouse(110, 55);
    nue_null_press_mouse(NUE_MOUSE_LEFT, NUE_TRUE);
    float x, y, dx, dy;
    uint32_t buttons = nue_mouse_get_state(&x, &y);
    CHECK_NEAR(x, 110, 1e-6);
    CHECK((buttons & NUE_MOUSE_BUTTON_MASK(NUE_MOUSE_LEFT)) != 0);
    nue_mouse_get_relative_state(&dx, &dy);
    CHECK_NEAR(dx, 110, 1e-6);   // accumulated since start
    nue_mouse_get_relative_state(&dx, &dy);
    CHECK_NEAR(dx, 0, 1e-6);     // consumed
    // focus lost must release held keys
    nue_null_press_key(NUE_SC_A, NUE_TRUE);
    nue_window_desc other = wd;
    nue_window w2;
    nue_window_create(&other, &w2);   // takes focus from w
    nue_window_raise(w);
    CHECK(nue_window_get_focused() == w);
    nue_window_hide(w);
    CHECK(!nue_key_is_down(NUE_SC_A));
    nue_window_destroy(w2);
    nue_window_destroy(w);
}

TEST_CASE(scancode_and_keycode_mapping) {
    CHECK_EQ(nue_key_from_scancode(NUE_SC_A), 'a');
    CHECK_EQ(nue_key_from_scancode(NUE_SC_0), '0');
    CHECK_EQ(nue_key_from_scancode(NUE_SC_F5), NUE_SC_F5 | NUE_KEYCODE_SCANCODE_MASK);
    CHECK_EQ(nue_key_to_scancode('d'), (uint32_t)NUE_SC_D);
    CHECK_EQ(nue_key_to_scancode(NUE_SC_UP | NUE_KEYCODE_SCANCODE_MASK), (uint32_t)NUE_SC_UP);
    CHECK_STR(nue_scancode_name(NUE_SC_LSHIFT), "Left Shift");
    CHECK_STR(nue_scancode_name(NUE_SC_F12), "F12");
}

TEST_CASE(last_used_device_and_glyphs) {
    Engine e;
    nue_last_input li;
    NUE_STRUCT_INIT(&li);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.kind, NUE_INPUT_DEVICE_NONE);
    nue_null_press_key(NUE_SC_SPACE, NUE_TRUE);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.kind, NUE_INPUT_DEVICE_KEYBOARD_MOUSE);
    nue_flush_events(0, 0xFFFFFF);

    nue_gamepad_id pad = 0;
    CHECK_EQ(nue_null_add_gamepad("DualSense", NUE_PAD_TYPE_PS5, &pad), NUE_OK);
    // stick drift below the threshold does not steal the glyphs
    nue_null_set_gamepad_axis(pad, NUE_PAD_AXIS_LEFTX, 3000);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.kind, NUE_INPUT_DEVICE_KEYBOARD_MOUSE);
    nue_null_set_gamepad_button(pad, NUE_PAD_SOUTH, NUE_TRUE);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.kind, NUE_INPUT_DEVICE_GAMEPAD);
    CHECK_EQ(li.glyphs, NUE_GLYPHS_PLAYSTATION);
    CHECK_EQ(li.device_id, pad);
    CHECK_STR(nue_gamepad_button_name(NUE_PAD_SOUTH, li.glyphs), "Cross");
    std::vector<uint32_t> t = types_of();
    int changed = 0;
    for (uint32_t x : t) changed += x == NUE_EVENT_INPUT_DEVICE_CHANGED;
    CHECK_EQ(changed, 1);

    // an Xbox pad then touch
    nue_gamepad_id xb;
    nue_null_add_gamepad("Xbox", NUE_PAD_TYPE_XBOXONE, &xb);
    nue_null_set_gamepad_axis(xb, NUE_PAD_AXIS_RIGHT_TRIGGER, 30000);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.glyphs, NUE_GLYPHS_XBOX);
    nue_null_touch(1, 0.5f, 0.5f, 0);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.kind, NUE_INPUT_DEVICE_TOUCH);
    nue_finger f[4];
    uint32_t cnt = 0;
    nue_touch_get_fingers(f, 4, &cnt);
    CHECK_EQ(cnt, 1u);
    nue_null_touch(1, 0.5f, 0.5f, 2);
    nue_touch_get_fingers(f, 4, &cnt);
    CHECK_EQ(cnt, 0u);

    // unplugging the pad in use falls back to keyboard glyphs
    nue_null_set_gamepad_button(xb, NUE_PAD_SOUTH, NUE_TRUE);
    nue_null_remove_gamepad(xb);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.kind, NUE_INPUT_DEVICE_KEYBOARD_MOUSE);

    nue_gamepad_state gs;
    NUE_STRUCT_INIT(&gs);
    CHECK_EQ(nue_gamepad_get_state(pad, &gs), NUE_OK);
    CHECK((gs.buttons & 1u) != 0);
    CHECK_EQ(gs.axes[NUE_PAD_AXIS_LEFTX], 3000);
    CHECK_EQ(nue_gamepad_get_state(xb, &gs), NUE_ERR_NOT_FOUND);
    CHECK_EQ(nue_gamepad_rumble(pad, 1000, 2000, 100), NUE_OK);
    nue_gamepad_info info;
    NUE_STRUCT_INIT(&info);
    CHECK_EQ(nue_gamepad_get_info(pad, &info), NUE_OK);
    CHECK_STR(info.name, "DualSense");
}

TEST_CASE(deadzone_math) {
    float x, y;
    nue_gamepad_apply_deadzone(1000, 1000, 0.2f, &x, &y);
    CHECK_NEAR(x, 0, 1e-6);
    nue_gamepad_apply_deadzone(32767, 0, 0.2f, &x, &y);
    CHECK_NEAR(x, 1.0, 1e-4);
    nue_gamepad_apply_deadzone(16384, 0, 0.5f, &x, &y);
    CHECK(x < 0.01f);           // 0.5 deflection sits right at the deadzone edge
    nue_gamepad_apply_deadzone(-32768, 0, 0.0f, &x, &y);
    CHECK_NEAR(x, -1.0, 1e-4);  // clamped, not -1.00003
}

TEST_CASE(power_and_thermal_events) {
    Engine e;
    nue_power_info pi;
    NUE_STRUCT_INIT(&pi);
    nue_power_get_info(&pi);
    GENERIC_EQ(pi.thermal, NUE_THERMAL_UNKNOWN);   // a real platform layer reports the machine's own thermal state from the start
    nue_null_set_thermal_state(NUE_THERMAL_NOMINAL);   // first sample: baseline
    nue_flush_events(0, 0xFFFFFF);
    nue_null_set_thermal_state(NUE_THERMAL_SERIOUS);
    CHECK_EQ(nue_power_get_thermal_state(), NUE_THERMAL_SERIOUS);
    nue_event ev;
    CHECK(nue_poll_event(&ev));
    CHECK_EQ(ev.type, (uint32_t)NUE_EVENT_THERMAL_CHANGED);
    CHECK_EQ(ev.payload.power.thermal, (uint32_t)NUE_THERMAL_SERIOUS);
    nue_null_set_power(NUE_POWER_ON_BATTERY, 42);
    nue_power_get_info(&pi);
    CHECK_EQ(pi.battery_percent, 42);
    CHECK(!pi.on_ac_power);
}

TEST_CASE(power_request_bookkeeping_without_platform_layer) {
    Engine e;
    nue_power_request r;
    NUE_STRUCT_INIT(&r);
    nue_power_applied applied = 0xFF;
    CHECK_EQ(nue_power_apply(&r, &applied), NUE_OK);   // nothing asked, nothing to honour
    CHECK_EQ(applied, 0u);
    r.profile = NUE_PERF_HIGH;
    r.timer_resolution_us = 1000;
    GENERIC_EQ(nue_power_apply(&r, &applied), NUE_OK_NOOP);   // the generic layer honours nothing and says so
    nue_power_request back;
    NUE_STRUCT_INIT(&back);
    nue_power_get_request(&back);
    CHECK_EQ(back.timer_resolution_us, 1000u);
    GENERIC_EQ(nue_thread_set_priority(nullptr, NUE_THREAD_PRIORITY_HIGH), NUE_ERR_UNSUPPORTED);
    GENERIC(CHECK(!nue_has_capability(NUE_CAP_GAME_MODE)));
    nue_perf_hint h;
    nue_result hr = nue_perf_hint_create(nullptr, 0, 16666666, &h);
    GENERIC(CHECK_EQ(hr, NUE_OK_NOOP));
    CHECK(NUE_SUCCEEDED(hr));
    nue_result rr = nue_perf_hint_report(h, 10000000);
    GENERIC(CHECK_EQ(rr, NUE_OK_NOOP));
    nue_perf_hint_destroy(h);
}

TEST_CASE(gfx_prepare_mode_follows_window_and_notifies) {
    Engine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.width = 800; wd.height = 600;
    nue_window w;
    nue_window_create(&wd, &w);
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.window = w;
    gd.flags = NUE_GFX_PREPARE_ONLY | NUE_GFX_VSYNC;
    nue_gfx g;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    nue_gfx_native nat;
    NUE_STRUCT_INIT(&nat);
    CHECK_EQ(nue_gfx_get_native(g, &nat), NUE_OK);
    CHECK_EQ(nat.pixel_width, 800);
    CHECK(nat.window.window != 0);
    CHECK(nat.renderer != NUE_RENDERER_AUTO);

    static int resized, suspended, resumed, destroying;
    static int32_t last_w;
    resized = suspended = resumed = destroying = 0;
    nue_gfx_add_listener(g, [](void*, nue_gfx, nue_gfx_notification what, int32_t a, int32_t) {
        if (what == NUE_GFX_NOTIFY_RESIZED) { ++resized; last_w = a; }
        if (what == NUE_GFX_NOTIFY_SUSPENDED) ++suspended;
        if (what == NUE_GFX_NOTIFY_RESUMED) ++resumed;
        if (what == NUE_GFX_NOTIFY_DESTROYING) ++destroying;
    }, nullptr);
    nue_gfx_set_auto_events(g, NUE_TRUE);
    nue_null_resize_window(w, 1280, 720);
    CHECK_EQ(resized, 1);
    CHECK_EQ(last_w, 1280);
    nue_null_set_app_state(NUE_APP_STATE_BACKGROUND);
    nue_null_set_app_state(NUE_APP_STATE_FOREGROUND);
    CHECK_EQ(suspended, 1);
    CHECK_EQ(resumed, 1);
    CHECK_EQ(nue_gfx_present(g, nullptr), NUE_ERR_BAD_STATE);   // prepare-only: the bgfx owner presents
    CHECK_EQ(nue_gfx_resize(g, 640, 360), NUE_OK);              // explicit size stops following the window
    nue_null_resize_window(w, 1000, 600);
    CHECK_EQ(last_w, 640);
    nue_window_destroy(w);                                      // gfx is told and invalidated instead of dangling
    CHECK_EQ(destroying, 1);
    CHECK_EQ(nue_gfx_resize(g, 100, 100), NUE_ERR_BAD_STATE);
    nue_gfx_destroy(g);
    CHECK_EQ(destroying, 1);
}

TEST_CASE(gfx_managed_mode_is_honestly_unsupported_for_now) {
    Engine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    nue_window w;
    nue_window_create(&wd, &w);
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.window = w;
    nue_gfx g = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_ERR_UNSUPPORTED);
    gd.renderer = NUE_RENDERER_NOOP;
    gd.window = nullptr;
    CHECK_EQ(nue_gfx_create(&gd, &g), NUE_OK);
    uint32_t frame = 0;
    CHECK_EQ(nue_gfx_present(g, &frame), NUE_OK);
    CHECK_EQ(frame, 1u);
    nue_gfx_destroy(g);
    nue_window_destroy(w);
}

TEST_CASE(leaks_are_closed_at_shutdown) {
    {
        Engine e;
        nue_window_desc wd;
        NUE_STRUCT_INIT(&wd);
        nue_window w;
        nue_window_create(&wd, &w);
        nue_gfx_desc gd;
        NUE_STRUCT_INIT(&gd);
        gd.renderer = NUE_RENDERER_NOOP;
        nue_gfx g;
        nue_gfx_create(&gd, &g);
    }   // ~Engine -> nue_shutdown closes both
    CHECK(!nue_is_initialized());
}

TEST_MAIN_BEGIN
    RUN(queue_order_flush_disable_peek);
    RUN(queue_capacity_drops_oldest_motion_first);
    RUN(watcher_sees_first_and_can_swallow);
    RUN(wait_event_times_out_on_virtual_clock);
    RUN(lifecycle_sequence_and_state);
    RUN(engine_loop_runs_callbacks_until_quit);
    RUN(displays_modes_and_windows);
    RUN(attach_native_window_is_not_resizable_by_us);
    RUN(keyboard_mouse_state_and_focus_loss);
    RUN(scancode_and_keycode_mapping);
    RUN(last_used_device_and_glyphs);
    RUN(deadzone_math);
    RUN(power_and_thermal_events);
    RUN(power_request_bookkeeping_without_platform_layer);
    RUN(gfx_prepare_mode_follows_window_and_notifies);
    RUN(gfx_managed_mode_is_honestly_unsupported_for_now);
    RUN(leaks_are_closed_at_shutdown);
TEST_MAIN_END
