// The SDL3 backend run against SDL's dummy video and audio drivers (headless: no window is shown, no sound is played, no device is touched).
#include "test_env.h"
#include "nue_test.h"

#if defined(NUE_HAS_SDL3)

#include <atomic>
#include <chrono>
#include <thread>

struct SdlEngine {
    std::string root;
    bool ok = false;
    SdlEngine() {
        root = nue_test::unique_temp_dir("sdl");
        std::string user = root + "/user";
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.app_name = "NUE sdl test"; d.app_id = "nue-sdl-test";
        d.backend = NUE_BACKEND_SDL3;
        d.user_data_path = user.c_str();
        d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD;
        d.log_level = NUE_LOG_WARN;
        ok = NUE_SUCCEEDED(nue_init(&d));
        if (!ok) std::printf("  nue_init(SDL3) failed: %s\n", nue_get_last_error_message());
    }
    ~SdlEngine() { nue_shutdown(); std::error_code ec; std::filesystem::remove_all(root, ec); }
};

static void pump_for(int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) { nue_pump_events(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
}

TEST_CASE(sdl3_backend_starts_with_dummy_drivers) {
    SdlEngine e;
    CHECK(e.ok);
    CHECK(nue_has_capability(NUE_CAP_WINDOWS));
    CHECK(nue_has_capability(NUE_CAP_GAMEPAD));
    CHECK(nue_has_capability(NUE_CAP_AUDIO_OUTPUT));
    nue_build_info bi;
    NUE_STRUCT_INIT(&bi);
    nue_get_build_info(&bi);
    CHECK(std::strstr(bi.backends, "sdl3") != nullptr);
    CHECK(std::strlen(bi.sdl_version) > 0);
}

TEST_CASE(sdl3_displays_and_windows) {
    SdlEngine e;
    uint32_t n = 0;
    CHECK_EQ(nue_display_list(nullptr, 0, &n), NUE_OK);
    CHECK(n >= 1);
    nue_display_info di;
    NUE_STRUCT_INIT(&di);
    CHECK_EQ(nue_display_get_info(nue_display_primary(), &di), NUE_OK);
    CHECK(di.bounds.w > 0 && di.bounds.h > 0);
    CHECK(di.current_mode.refresh_rate_hz >= 0);   // SDL's dummy driver reports 0 (unknown); real drivers report the rate
    uint32_t modes = 0;
    CHECK_EQ(nue_display_list_modes(di.id, nullptr, 0, &modes), NUE_OK);

    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.title = "sdl dummy";
    wd.width = 640; wd.height = 360;
    wd.flags = NUE_WINDOW_RESIZABLE;
    nue_window w = nullptr;
    CHECK_EQ(nue_window_create(&wd, &w), NUE_OK);
    int32_t pw = 0, ph = 0;
    nue_window_get_size(w, &pw, &ph);
    CHECK_EQ(pw, 640); CHECK_EQ(ph, 360);
    char title[64]; size_t need;
    nue_window_get_title(w, title, sizeof(title), &need);
    CHECK_STR(title, "sdl dummy");
    nue_window_set_title(w, "renamed");
    nue_window_get_title(w, title, sizeof(title), &need);
    CHECK_STR(title, "renamed");
    CHECK_EQ(nue_window_set_size(w, 800, 450), NUE_OK);
    pump_for(50);
    nue_window_get_size(w, &pw, &ph);
    CHECK_EQ(pw, 800); CHECK_EQ(ph, 450);
    nue_window_get_pixel_size(w, &pw, &ph);
    CHECK(pw >= 800);
    nue_rect_i safe;
    CHECK_EQ(nue_window_get_safe_area(w, &safe), NUE_OK);
    CHECK(safe.w > 0);
    // cursor and relative mode calls must not fail on a headless driver
    CHECK(NUE_SUCCEEDED(nue_cursor_set_visible(NUE_FALSE)));
    CHECK(NUE_SUCCEEDED(nue_cursor_set_visible(NUE_TRUE)));
    CHECK(NUE_SUCCEEDED(nue_window_set_cursor_clip(w, nullptr, NUE_TRUE)));
    CHECK(NUE_SUCCEEDED(nue_window_set_cursor_clip(w, nullptr, NUE_FALSE)));
    nue_window_destroy(w);
    CHECK_EQ(nue_window_get_size(w, &pw, &ph), NUE_ERR_INVALID_HANDLE);
}

TEST_CASE(sdl3_window_events_reach_the_queue_and_keep_the_snapshot_current) {
    SdlEngine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.width = 320; wd.height = 200;
    wd.flags = NUE_WINDOW_RESIZABLE;
    nue_window w;
    nue_window_create(&wd, &w);
    pump_for(30);
    nue_flush_events(0, 0xFFFFFF);
    nue_window_set_size(w, 400, 300);
    pump_for(60);
    bool resized = false;
    nue_event ev;
    while (nue_poll_event(&ev)) {
        if (ev.type == NUE_EVENT_WINDOW_RESIZED || ev.type == NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED) { resized = true; CHECK_EQ(ev.window_id, nue_window_get_id(w)); }
    }
    CHECK(resized);
    int32_t pw, ph;
    nue_window_get_pixel_size(w, &pw, &ph);
    CHECK_EQ(pw, 400);
    nue_window_destroy(w);
}

TEST_CASE(sdl3_attached_window_kinds_that_the_platform_lacks_are_refused) {
    SdlEngine e;
    nue_native_window nw;
    NUE_STRUCT_INIT(&nw);
    nw.kind = NUE_NATIVE_ANDROID_WINDOW;   // never attachable through SDL on desktops
    nw.window = 1;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    wd.attach = &nw;
    nue_window w = nullptr;
    CHECK_EQ(nue_window_create(&wd, &w), NUE_ERR_UNSUPPORTED);
    CHECK(w == nullptr);
}

TEST_CASE(sdl3_audio_devices_and_a_callback_stream_run_on_the_dummy_driver) {
    SdlEngine e;
    uint32_t n = 0;
    CHECK_EQ(nue_audio_list_devices(NUE_FALSE, nullptr, 0, &n), NUE_OK);
    CHECK(n >= 1);
    static std::atomic<uint64_t> frames;
    frames = 0;
    nue_audio_stream_desc sd;
    NUE_STRUCT_INIT(&sd);
    sd.spec = {NUE_AUDIO_S16, 2, 48000};
    sd.userdata = nullptr;
    sd.callback = [](void*, void* out, uint32_t n) { frames += n; std::memset(out, 0, size_t(n) * 4); };
    sd.latency_ms = 20;
    nue_audio_stream s = nullptr;
    CHECK_EQ(nue_audio_stream_create(&sd, &s), NUE_OK);
    for (int i = 0; i < 100 && frames.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(frames.load() > 0);   // the dummy driver consumes in real time
    nue_audio_latency lat;
    NUE_STRUCT_INIT(&lat);
    CHECK_EQ(nue_audio_stream_get_latency(s, &lat), NUE_OK);
    CHECK(lat.device_buffer_frames > 0);
    nue_audio_stream_stop(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    uint64_t stopped_at = frames.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(frames.load(), stopped_at);   // a stopped stream does not call back
    nue_audio_stream_destroy(s);
}

TEST_CASE(sdl3_mixer_renders_through_a_real_sdl_stream) {
    SdlEngine e;
    nue_mixer_desc md;
    NUE_STRUCT_INIT(&md);
    md.sample_rate = 48000; md.channels = 2;
    nue_mixer mx = nullptr;
    CHECK_EQ(nue_mixer_create(&md, &mx), NUE_OK);
    std::vector<int16_t> pcm(48000, 8000);
    nue_buffer_desc bd;
    NUE_STRUCT_INIT(&bd);
    bd.spec = {NUE_AUDIO_S16, 1, 48000}; bd.data = pcm.data(); bd.frame_count = 48000; bd.copy_data = NUE_TRUE;
    nue_buffer_id b;
    nue_buffer_create(mx, &bd, &b);
    nue_voice_desc vd;
    NUE_STRUCT_INIT(&vd);
    vd.kind = NUE_VOICE_STATIC; vd.buffer = b; vd.flags = NUE_VOICE_START_PLAYING;
    nue_voice_id v;
    nue_voice_create(mx, &vd, &v);
    nue_mixer_stats ms;
    NUE_STRUCT_INIT(&ms);
    for (int i = 0; i < 100 && ms.blocks_mixed < 4; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); nue_mixer_get_stats(mx, &ms); }
    CHECK(ms.blocks_mixed >= 4);
    nue_mixer_destroy(mx);
}

TEST_CASE(sdl3_gamepad_api_without_devices) {
    SdlEngine e;
    uint32_t n = 99;
    CHECK_EQ(nue_gamepad_list(nullptr, 0, &n), NUE_OK);
    CHECK_EQ(n, 0u);
    nue_gamepad_state gs;
    NUE_STRUCT_INIT(&gs);
    CHECK_EQ(nue_gamepad_get_state(12345, &gs), NUE_ERR_NOT_FOUND);
    CHECK_EQ(nue_gamepad_rumble(12345, 1, 1, 10), NUE_ERR_NOT_FOUND);
    CHECK_EQ(nue_gamepad_add_mapping("not a mapping"), NUE_ERR_BACKEND);
    CHECK_EQ(nue_gamepad_add_mapping("03000000000000000000000000000000,Test Pad,a:b0,b:b1,x:b2,y:b3,"), NUE_OK);
    pump_for(20);
}

TEST_CASE(sdl3_keyboard_and_text_input_state) {
    SdlEngine e;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    nue_window w;
    nue_window_create(&wd, &w);
    CHECK_EQ(nue_text_input_start(w), NUE_OK);
    CHECK(nue_text_input_is_active(w));
    CHECK_EQ(nue_text_input_stop(w), NUE_OK);
    CHECK(!nue_text_input_is_active(w));
    CHECK(!nue_key_is_down(NUE_SC_SPACE));
    // events injected through the public queue behave like the real ones
    nue_event ke;
    std::memset(&ke, 0, sizeof(ke));
    ke.type = NUE_EVENT_KEY_DOWN;
    ke.payload.key.scancode = NUE_SC_SPACE;
    nue_push_event(&ke);
    CHECK(nue_key_is_down(NUE_SC_SPACE));
    nue_window_destroy(w);
}

TEST_MAIN_BEGIN
    RUN(sdl3_backend_starts_with_dummy_drivers);
    RUN(sdl3_displays_and_windows);
    RUN(sdl3_window_events_reach_the_queue_and_keep_the_snapshot_current);
    RUN(sdl3_attached_window_kinds_that_the_platform_lacks_are_refused);
    RUN(sdl3_audio_devices_and_a_callback_stream_run_on_the_dummy_driver);
    RUN(sdl3_mixer_renders_through_a_real_sdl_stream);
    RUN(sdl3_gamepad_api_without_devices);
    RUN(sdl3_keyboard_and_text_input_state);
TEST_MAIN_END

#else

int main() { std::printf("SDL3 backend not compiled in: nothing to test\n"); return 0; }

#endif
