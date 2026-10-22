// The C++ wrapper (nuepp): RAII ownership, error handling without exceptions, thin forwarding. Compiled with exceptions disabled where the compiler allows it.
#include "nuepp/nue.hpp"
#include "nue/null_backend.h"
#include "test_env.h"
#include "nue_test.h"

#include <type_traits>

static nue_init_desc test_desc(const std::string& root, const std::string& data) {
    nue_init_desc d = nuepp::init<nue_init_desc>();
    d.api_version = NUE_API_VERSION;
    d.app_name = "wrapper test"; d.app_id = "nue-wrapper-test";
    d.backend = NUE_BACKEND_NULL;
    d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_DETERMINISTIC;
    d.user_data_path = root.c_str();
    d.game_data_path = data.c_str();
    d.log_level = NUE_LOG_WARN;
    return d;
}

struct Env {
    std::string root, data, user;
    nuepp::Context ctx;
    Env() {
        root = nue_test::unique_temp_dir("wrap");
        data = root + "/data";
        user = root + "/user";
        std::filesystem::create_directories(data);
        nue_init_desc d = test_desc(user, data);
        ctx.init(d);
    }
    ~Env() { ctx.reset(); std::error_code ec; std::filesystem::remove_all(root, ec); }
};

TEST_CASE(wrapper_types_are_move_only_raii) {
    static_assert(!std::is_copy_constructible<nuepp::Window>::value, "windows are unique");
    static_assert(std::is_nothrow_move_constructible<nuepp::Window>::value, "windows move");
    static_assert(!std::is_copy_constructible<nuepp::File>::value, "files are unique");
    static_assert(!std::is_copy_constructible<nuepp::Mixer>::value, "mixers are unique");
    static_assert(!std::is_copy_constructible<nuepp::Context>::value, "the context is unique");
    CHECK(!nuepp::Context::initialized());
    {
        Env e;
        CHECK(e.ctx.ok());
        CHECK(nuepp::Context::initialized());
        CHECK(nuepp::Context::version().find("0.1") == 0);
    }
    CHECK(!nuepp::Context::initialized());   // destroyed context shut the engine down
}

TEST_CASE(results_and_expected_without_exceptions) {
    Env e;
    nuepp::Result ok;
    CHECK(ok.ok());
    nuepp::Result bad(NUE_ERR_NOT_FOUND);
    CHECK(!bad);
    CHECK_STR(bad.name(), "NUE_ERR_NOT_FOUND");
    auto missing = nuepp::vfs::load("data:nope.bin");
    CHECK(!missing.ok());
    CHECK_EQ(missing.result().code(), NUE_ERR_NOT_FOUND);
    CHECK(missing.value().empty());   // failed Expected: default value, no throw
    CHECK(std::strlen(nuepp::Result::last_error()) > 0);
    nuepp::Context bad_ctx;
    nue_init_desc d = nuepp::init<nue_init_desc>();
    d.api_version = NUE_MAKE_VERSION(99, 0, 0);
    CHECK(bad_ctx.init(d).code() == NUE_ERR_VERSION_MISMATCH);
    CHECK(!bad_ctx.ok());
}

TEST_CASE(files_and_vfs_helpers) {
    Env e;
    const char payload[] = "hello wrapper";
    CHECK(nuepp::vfs::save("user:wrap/hello.txt", payload, sizeof(payload) - 1).ok());
    CHECK(nuepp::vfs::exists("user:WRAP/HELLO.TXT"));
    auto loaded = nuepp::vfs::load("user:wrap/hello.txt");
    CHECK(loaded.ok());
    CHECK_EQ(loaded.value().size(), sizeof(payload) - 1);
    {
        nuepp::File f("user:wrap/hello.txt");
        CHECK(f.ok());
        CHECK_EQ(f.size(), sizeof(payload) - 1);
        char buf[8] = {};
        CHECK_EQ(f.read(buf, 5), 5u);
        CHECK_STR(buf, "hello");
        CHECK_EQ(f.tell(), 5u);
        CHECK(f.seek(-7, NUE_SEEK_END).ok());
        CHECK_EQ(f.read(buf, 7), 7u);
        std::vector<uint8_t> all = f.read_all();
        CHECK_EQ(all.size(), sizeof(payload) - 1);
        nuepp::File moved(std::move(f));
        CHECK(!f.ok());
        CHECK(moved.ok());
    }
    nuepp::File missing("user:wrap/none.txt");
    CHECK(!missing.ok());
    CHECK_EQ(missing.result().code(), NUE_ERR_NOT_FOUND);
    nue_test::write_file(e.data + "/B.txt", "b");
    nue_test::write_file(e.data + "/a.txt", "a");
    std::vector<std::string> names = nuepp::vfs::list("data:");
    CHECK_EQ(names.size(), 2u);
    CHECK(!nuepp::vfs::resolve("data:b.txt").empty());
}

TEST_CASE(config_helpers_and_threads) {
    Env e;
    nuepp::config::set("wrap.flag", true);
    nuepp::config::set("wrap.count", 7);
    nuepp::config::set("wrap.ratio", 0.25);
    nuepp::config::set("wrap.name", "nfsu2");
    CHECK(nuepp::config::get("wrap.flag", false));
    CHECK_EQ(nuepp::config::get("wrap.count", 0), 7);
    CHECK_NEAR(nuepp::config::get("wrap.ratio", 0.0), 0.25, 1e-12);
    CHECK(nuepp::config::get("wrap.name", "") == "nfsu2");
    CHECK(nuepp::config::get("wrap.absent", "fallback") == "fallback");

    nuepp::Mutex m;
    int shared = 0;
    nuepp::Semaphore sem;
    {
        nuepp::Thread t([&] { for (int i = 0; i < 1000; ++i) { nuepp::LockGuard g(m); ++shared; } sem.post(); }, "wrapper-thread");
        CHECK(t.joinable());
        CHECK(sem.wait(5000000000ll));
    }   // destructor joins
    CHECK_EQ(shared, 1000);
    nuepp::Thread empty;
    CHECK(!empty.joinable());
}

TEST_CASE(timer_window_gfx_and_events) {
    Env e;
    int ticks = 0;
    auto cb = [&](uint64_t) { ++ticks; };
    {
        nuepp::Timer t(&cb, 5000000);
        nue_null_advance_time(23000000);
        CHECK_EQ(ticks, 4);
    }
    nuepp::Window w(nuepp::init<nue_window_desc>());
    CHECK(w.ok());
    CHECK(w.has_focus());
    CHECK(w.set_title("wrapped").ok());
    CHECK(w.title() == "wrapped");
    nue_size_i s = w.pixel_size();
    CHECK_EQ(s.w, 1280);
    nuepp::Gfx gfx(w);
    CHECK(gfx.ok());
    CHECK(gfx.native().pixel_width == 1280);
    CHECK(gfx.set_auto_events(true).ok());
    nue_null_resize_window(w.handle(), 1000, 600);
    CHECK_EQ(gfx.native().pixel_width, 1000);   // followed the window through the auto-events watcher
    int count = nuepp::events::drain([](const nue_event&) { return true; });
    CHECK(count > 0);
    CHECK(!nuepp::events::poll().has_value());
    nuepp::events::post_user(5, 6, 7);
    auto ev = nuepp::events::poll();
    CHECK(ev.has_value());
    CHECK_EQ(ev->payload.user.code, 5);
    w.reset();
    CHECK(!w.ok());
    nuepp::Window attached = nuepp::Window::attach(nue_native_window{sizeof(nue_native_window), NUE_NATIVE_WIN32_HWND, 0x1234, 0, 0});
    CHECK(attached.ok());
    CHECK_EQ(attached.native().window, 0x1234ull);
}

TEST_CASE(audio_stream_and_mixer_wrappers) {
    Env e;
    nuepp::AudioStream stream(nue_audio_spec{NUE_AUDIO_S16, 2, 48000});
    CHECK(stream.ok());
    int16_t frames[200] = {};
    CHECK_EQ(stream.push(frames, 100), 100u);
    CHECK_EQ(stream.queued_frames(), 100u);
    CHECK(stream.flush().ok());
    CHECK_EQ(stream.queued_frames(), 0u);

    nuepp::Mixer mixer = nuepp::Mixer::manual(48000, 2, 64);
    CHECK(mixer.ok());
    std::vector<int16_t> pcm(256, 16384);
    auto buffer = mixer.make_buffer({NUE_AUDIO_S16, 1, 48000}, pcm.data(), 256);
    CHECK(buffer.ok());
    auto voice = mixer.play(buffer.value(), false, 1.0f, -1.0f);
    CHECK(voice.ok());
    std::vector<float> out(2 * 64);
    CHECK(mixer.render(out.data(), 64).ok());
    CHECK_NEAR(out[0], 0.5, 1e-3);
    CHECK_EQ(mixer.stats().active_voices, 1u);
    auto bus = mixer.make_bus();
    CHECK(bus.ok());
    auto bad = mixer.play(0xDEAD0001u);
    CHECK(!bad.ok());
    CHECK_EQ(bad.result().code(), NUE_ERR_INVALID_HANDLE);
    nuepp::Mixer moved(std::move(mixer));
    CHECK(!mixer.ok());
    CHECK(moved.ok());
}

TEST_CASE(budget_and_trace_helpers) {
    Env e;
    nuepp::Budget b("wrap.pool", NUE_BUDGET_GAME);
    CHECK(b.id() != 0);
    CHECK(b.request(1024).ok());
    CHECK_EQ(b.state().used_bytes, 1024u);
    b.report(-1024);
    CHECK_EQ(b.state().used_bytes, 0u);
    {
        nuepp::TraceZone zone("wrapper-zone");   // tracing is off: nothing happens, nothing breaks
    }
    CHECK(true);
}

TEST_MAIN_BEGIN
    RUN(wrapper_types_are_move_only_raii);
    RUN(results_and_expected_without_exceptions);
    RUN(files_and_vfs_helpers);
    RUN(config_helpers_and_threads);
    RUN(timer_window_gfx_and_events);
    RUN(audio_stream_and_mixer_wrappers);
    RUN(budget_and_trace_helpers);
TEST_MAIN_END
