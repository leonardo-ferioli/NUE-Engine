// API hooks added for the platform sessions: lifecycle/suspend/audio events, refresh-rate cooperation and frame limiter, glyph override, GPU caps
// probe, platform class flags, power TDP fields, on-screen keyboard.
#include "test_env.h"
#include "nue_test.h"

#include <atomic>
#include <cstring>

using nue_test::Engine;

TEST_CASE(suspend_and_audio_interruption_events) {
    Engine e;
    nue_null_system_suspend(NUE_TRUE);
    nue_null_system_suspend(NUE_FALSE);
    nue_null_audio_interruption(NUE_TRUE);
    nue_null_audio_interruption(NUE_FALSE);
    nue_event ev[8];
    int n = nue_test::drain(ev, 8);
    CHECK_EQ(n, 4);
    CHECK_EQ(ev[0].type, (uint32_t)NUE_EVENT_SYSTEM_SUSPENDING);
    CHECK_EQ(ev[1].type, (uint32_t)NUE_EVENT_SYSTEM_RESUMED);
    CHECK_EQ(ev[2].type, (uint32_t)NUE_EVENT_AUDIO_INTERRUPTED);
    CHECK_EQ(ev[3].type, (uint32_t)NUE_EVENT_AUDIO_RESUMED);
}

TEST_CASE(refresh_request_and_frame_limit_are_stored_even_without_platform_support) {
    Engine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    nue_window w;
    nue_window_create(&wd, &w);
    GENERIC_EQ(nue_window_request_refresh_rate(w, 45.0f), NUE_OK_NOOP);
    GENERIC_EQ(nue_window_set_frame_limit(w, 40.0f), NUE_OK_NOOP);
    CHECK_NEAR(nue_window_get_frame_limit(w), 40.0, 1e-6);
    nue_window_set_frame_limit(w, 0.0f);
    CHECK_NEAR(nue_window_get_frame_limit(w), 0.0, 1e-6);
    nue_window_destroy(w);
    CHECK_EQ(nue_window_set_frame_limit(w, 30.0f), NUE_ERR_INVALID_HANDLE);
}

TEST_CASE(frame_limiter_paces_on_the_virtual_clock) {
    Engine e;
    nue_frame_limiter_reset();
    int64_t t0 = nue_time_ns();
    for (int i = 0; i < 30; ++i) {
        nue_frame_limiter_wait(60.0);
        nue_null_advance_time(2000000);   // 2 ms of "work" per frame
    }
    // 30 frames at 60 fps: the first is free, 29 intervals of 16.67 ms (the work time is part of the interval)
    double elapsed_ms = double(nue_time_ns() - t0) / 1e6;
    CHECK(elapsed_ms > 29 * 16.6 && elapsed_ms < 30 * 16.8 + 3);
    // a long stall does not make the limiter burst to catch up
    nue_null_advance_time(2000000000ll);
    int64_t waited = nue_frame_limiter_wait(60.0);
    CHECK_EQ(waited, 0);
    int64_t t1 = nue_time_ns();
    nue_frame_limiter_wait(60.0);
    CHECK(nue_time_ns() - t1 >= 16000000);
    CHECK_EQ(nue_frame_limiter_wait(0.0), 0);   // fps <= 0: no limit
}

TEST_CASE(glyph_override_wins_over_the_device) {
    Engine e;
    nue_gamepad_id pad;
    nue_null_add_gamepad("Steam Virtual Gamepad", NUE_PAD_TYPE_XBOX360, &pad);
    nue_null_set_gamepad_button(pad, NUE_PAD_SOUTH, NUE_TRUE);
    nue_last_input li;
    NUE_STRUCT_INIT(&li);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.glyphs, NUE_GLYPHS_XBOX);
    CHECK_EQ(nue_input_set_glyph_override(NUE_GLYPHS_STEAM), NUE_OK);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.glyphs, NUE_GLYPHS_STEAM);
    nue_gamepad_info gi;
    NUE_STRUCT_INIT(&gi);
    nue_gamepad_get_info(pad, &gi);
    CHECK_EQ(gi.glyphs, NUE_GLYPHS_STEAM);
    nue_null_set_gamepad_button(pad, NUE_PAD_EAST, NUE_TRUE);
    nue_input_get_last_device(&li);
    CHECK_EQ(li.glyphs, NUE_GLYPHS_STEAM);
    CHECK_EQ(nue_input_set_glyph_override(-1), NUE_OK);
    nue_gamepad_get_info(pad, &gi);
    CHECK_EQ(gi.glyphs, NUE_GLYPHS_XBOX);
    CHECK_EQ(nue_input_set_glyph_override(99), NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(gpu_probe_without_a_platform_layer_is_an_honest_assumption) {
    Engine e;
    nue_gpu_caps c;
    NUE_STRUCT_INIT(&c);
    nue_result r = nue_gpu_probe(&c);
    GENERIC(CHECK_EQ(r, NUE_OK_PARTIAL));   // partial = assumptions only (a real platform probe answers NUE_OK)
    GENERIC(CHECK(!c.probed));
    CHECK(c.format[NUE_FMT_RGBA8] & NUE_FMTSUP_RENDER_TARGET);
    CHECK_STR(nue_format_family_name(NUE_FMT_BC3), "BC3");
    CHECK_STR(nue_format_family_name(NUE_FMT_FAMILY_COUNT), "UNKNOWN");
#if defined(NUE_PLATFORM_WINDOWS) || defined(NUE_PLATFORM_LINUX) || defined(NUE_PLATFORM_MACOS)
    CHECK(c.format[NUE_FMT_BC1] & NUE_FMTSUP_SAMPLE);
    CHECK(!(c.format[NUE_FMT_ASTC_LDR] & NUE_FMTSUP_SAMPLE));   // not assumed on desktops
#endif
    nue_gfx_desc gd;
    NUE_STRUCT_INIT(&gd);
    gd.renderer = NUE_RENDERER_NOOP;
    nue_gfx g;
    nue_gfx_create(&gd, &g);
    nue_gpu_caps lc;
    NUE_STRUCT_INIT(&lc);
    CHECK_EQ(nue_gfx_get_gpu_caps(g, &lc), NUE_OK_PARTIAL);   // no bgfx behind it yet
    GENERIC(CHECK_EQ(lc.renderer, NUE_RENDERER_AUTO));
    nue_gfx_destroy(g);
}

TEST_CASE(platform_class_flags_default_to_none_on_the_generic_layer) {
    Engine e;
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    nue_system_get_info(&si);
    GENERIC(CHECK_EQ(si.platform_flags, 0u));
    GENERIC(CHECK_STR(si.platform_variant, ""));
    GENERIC_EQ(nue_system_get_platform_flags(), 0u);
}

TEST_CASE(power_info_new_fields_report_unknown) {
    Engine e;
    nue_power_info pi;
    NUE_STRUCT_INIT(&pi);
    nue_power_get_info(&pi);
    CHECK_EQ(pi.tdp_watts_current, -1);
    CHECK_EQ(pi.tdp_watts_min, -1);
    GENERIC(CHECK(!pi.suspend_supported));
    // an old caller (smaller struct) still works and sees nothing beyond its size
    struct OldPowerInfo { uint32_t struct_size; } small = {sizeof(uint32_t) * 2};
    uint8_t buf[sizeof(nue_power_info)];
    std::memset(buf, 0xEE, sizeof(buf));
    std::memcpy(buf, &small, sizeof(small));
    CHECK_EQ(nue_power_get_info(reinterpret_cast<nue_power_info*>(buf)), NUE_OK);
    CHECK_EQ(buf[sizeof(buf) - 1], 0xEE);   // untouched beyond struct_size
    nue_power_request r;
    NUE_STRUCT_INIT(&r);
    r.tdp_watts = 12;
    nue_power_applied applied;
    nue_result pr = nue_power_apply(&r, &applied);
    GENERIC(CHECK_EQ(pr, NUE_OK_NOOP));
    GENERIC(CHECK_EQ(applied & NUE_POWER_APPLIED_TDP, 0u));
}

TEST_CASE(screen_keyboard_request) {
    Engine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    nue_window w;
    nue_window_create(&wd, &w);
    GENERIC_EQ(nue_text_input_show_screen_keyboard(w, NUE_TRUE), NUE_ERR_UNSUPPORTED);   // null backend/generic layer: no on-screen keyboard
    GENERIC_EQ(nue_text_input_show_screen_keyboard(w, NUE_FALSE), NUE_OK_NOOP);          // hiding is always fine
    CHECK_EQ(nue_text_input_show_screen_keyboard(nullptr, NUE_TRUE), NUE_ERR_INVALID_HANDLE);
    nue_window_destroy(w);
}

TEST_CASE(init_order_loads_settings_before_the_backend_starts) {
    // settings written by a previous run are visible to config readers from the first platform call on
    std::string root = nue_test::unique_temp_dir("order");
    nue_test::write_file(root + "/user/settings/nue.ini", "[ios]\naudio_mix = respect_silent\n");
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    std::string user = root + "/user";
    d.api_version = NUE_API_VERSION; d.backend = NUE_BACKEND_NULL; d.user_data_path = user.c_str();
    d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_DETERMINISTIC;
    d.log_level = NUE_LOG_ERROR;
    CHECK_EQ(nue_init(&d), NUE_OK);
    char buf[32]; size_t need;
    nue_config_get_string("ios.audio_mix", "", buf, sizeof(buf), &need);
    CHECK_STR(buf, "respect_silent");
    nue_shutdown();
    std::filesystem::remove_all(root);
}

TEST_CASE(virtual_timers_fire_exactly_on_the_virtual_clock) {
    Engine e;
    static uint64_t ticks, last_tick;
    static int one_shots;
    ticks = last_tick = 0; one_shots = 0;
    nue_timer_desc d;
    NUE_STRUCT_INIT(&d);
    d.period_ns = 10000000;   // 10 ms
    d.callback = [](void*, nue_timer, uint64_t tick) { ++ticks; last_tick = tick; };
    nue_timer t;
    CHECK_EQ(nue_timer_create(&d, &t), NUE_OK);
    nue_null_advance_time(35000000);
    CHECK_EQ(ticks, 3u);
    CHECK_EQ(last_tick, 3u);
    nue_timer_set_period(t, 5000000);
    nue_null_advance_time(20000000);
    CHECK(ticks >= 6);
    nue_timer_cancel(t);
    uint64_t before = ticks;
    nue_null_advance_time(100000000);
    CHECK_EQ(ticks, before);
    nue_timer_destroy(t);

    nue_timer_desc os;
    NUE_STRUCT_INIT(&os);
    os.period_ns = 4000000; os.one_shot = NUE_TRUE;
    os.callback = [](void*, nue_timer, uint64_t) { ++one_shots; };
    nue_timer t2;
    nue_timer_create(&os, &t2);
    nue_null_advance_time(50000000);
    CHECK_EQ(one_shots, 1);
    nue_timer_destroy(t2);
    nue_timer_desc bad;
    NUE_STRUCT_INIT(&bad);
    CHECK_EQ(nue_timer_create(&bad, &t2), NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(real_timers_run_on_their_own_thread) {
    Engine e(0);   // not deterministic: real clock and threads
    static std::atomic<int> ticks;
    ticks = 0;
    nue_timer_desc d;
    NUE_STRUCT_INIT(&d);
    d.period_ns = 5000000;
    d.callback = [](void*, nue_timer, uint64_t) { ++ticks; };
    d.name = "test-timer";
    nue_timer t;
    CHECK_EQ(nue_timer_create(&d, &t), NUE_OK);
    for (int i = 0; i < 200 && ticks.load() < 4; ++i) nue_sleep_ns(5000000, NUE_SLEEP_COARSE);
    CHECK(ticks.load() >= 4);
    nue_timer_destroy(t);   // waits for a callback in flight
    int after = ticks.load();
    nue_sleep_ns(30000000, NUE_SLEEP_COARSE);
    CHECK_EQ(ticks.load(), after);
}

TEST_MAIN_BEGIN
    RUN(suspend_and_audio_interruption_events);
    RUN(refresh_request_and_frame_limit_are_stored_even_without_platform_support);
    RUN(frame_limiter_paces_on_the_virtual_clock);
    RUN(glyph_override_wins_over_the_device);
    RUN(gpu_probe_without_a_platform_layer_is_an_honest_assumption);
    RUN(platform_class_flags_default_to_none_on_the_generic_layer);
    RUN(power_info_new_fields_report_unknown);
    RUN(screen_keyboard_request);
    RUN(init_order_loads_settings_before_the_backend_starts);
    RUN(virtual_timers_fire_exactly_on_the_virtual_clock);
    RUN(real_timers_run_on_their_own_thread);
TEST_MAIN_END
