// init/shutdown, hashing, config store, logging, threads and sync, time, allocator.
#include "test_env.h"
#include "nue_test.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

TEST_CASE(init_refcount_and_version_check) {
    CHECK(!nue_is_initialized());
    nue_init_desc bad;
    NUE_STRUCT_INIT(&bad);
    bad.api_version = NUE_MAKE_VERSION(NUE_VERSION_MAJOR + 1, 0, 0);
    bad.backend = NUE_BACKEND_NULL;
    CHECK_EQ(nue_init(&bad), NUE_ERR_VERSION_MISMATCH);
    CHECK(!nue_is_initialized());
    CHECK(std::strlen(nue_get_last_error_message()) > 0);

    nue_test::Engine e;
    CHECK(e.ok);
    CHECK(nue_is_initialized());
    nue_init_desc again;
    NUE_STRUCT_INIT(&again);
    CHECK_EQ(nue_init(&again), NUE_OK);   // second reference
    nue_shutdown();
    CHECK(nue_is_initialized());          // still held by the first
    nue_build_info bi;
    NUE_STRUCT_INIT(&bi);
    CHECK_EQ(nue_get_build_info(&bi), NUE_OK);
    CHECK_EQ(bi.pointer_size, sizeof(void*));
    CHECK(std::strlen(bi.platform_layer) > 0);
    GENERIC(CHECK_STR(bi.platform_layer, "generic"));
}

TEST_CASE(shutdown_without_init_is_harmless) {
    nue_shutdown();
    CHECK(!nue_is_initialized());
    nue_window w = nullptr;
    nue_window_desc wd;
    NUE_STRUCT_INIT(&wd);
    CHECK_EQ(nue_window_create(&wd, &w), NUE_ERR_NOT_INITIALIZED);
}

TEST_CASE(hash_known_vectors) {
    CHECK_EQ(nue_crc32(0, "123456789", 9), 0xCBF43926u);
    uint8_t d[32];
    char hex[80];
    nue_sha256("abc", 3, d);
    nue_hex_encode(d, 32, hex, sizeof(hex));
    CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    nue_sha256("", 0, d);
    nue_hex_encode(d, 32, hex, sizeof(hex));
    CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // streaming equals one-shot, across block boundaries
    std::string big(1000, 'x');
    nue_sha256_state st;
    nue_sha256_begin(&st);
    for (size_t i = 0; i < big.size(); i += 37) nue_sha256_update(&st, big.data() + i, std::min<size_t>(37, big.size() - i));
    uint8_t a[32], b[32];
    nue_sha256_end(&st, a);
    nue_sha256(big.data(), big.size(), b);
    CHECK(std::memcmp(a, b, 32) == 0);
    CHECK_EQ(nue_crc32(nue_crc32(0, "1234", 4), "56789", 5), 0xCBF43926u);
}

TEST_CASE(config_types_and_ini_roundtrip) {
    nue_test::Engine e;
    CHECK(!nue_config_has("video.vsync"));
    CHECK_EQ(nue_config_set_bool("video.vsync", NUE_TRUE), NUE_OK);
    CHECK_EQ(nue_config_set_int("video.width", 1920), NUE_OK);
    CHECK_EQ(nue_config_set_double("audio.volume", 0.75), NUE_OK);
    CHECK_EQ(nue_config_set_string("player.name", "Ana \"The Wheel\"\tR"), NUE_OK);
    CHECK_EQ(nue_config_set_string("player.numeric_looking", "123"), NUE_OK);
    CHECK(nue_config_get_bool("video.vsync", NUE_FALSE));
    CHECK_EQ(nue_config_get_int("video.width", 0), 1920);
    CHECK_NEAR(nue_config_get_double("audio.volume", 0), 0.75, 1e-12);
    CHECK_EQ(nue_config_get_int("missing.key", 42), 42);
    CHECK_EQ(nue_config_type_of("video.width"), NUE_CONFIG_INT);

    CHECK_EQ(nue_config_save(nullptr), NUE_OK);   // settings:nue.ini
    nue_config_remove("video.width");
    nue_config_set_string("player.name", "overwritten");
    CHECK_EQ(nue_config_load(nullptr), NUE_OK);
    CHECK_EQ(nue_config_get_int("video.width", 0), 1920);
    char buf[64];
    size_t need = 0;
    CHECK_EQ(nue_config_get_string("player.name", "", buf, sizeof(buf), &need), NUE_OK);
    CHECK_STR(buf, "Ana \"The Wheel\"\tR");
    nue_config_get_string("player.numeric_looking", "", buf, sizeof(buf), &need);
    CHECK_STR(buf, "123");
    CHECK_EQ(nue_config_type_of("player.numeric_looking"), NUE_CONFIG_STRING);   // quoted on save, stays a string
}

TEST_CASE(config_text_parsing_and_errors) {
    nue_test::Engine e;
    const char* text = "; comment\nglobal = 5\n[gfx]\nrenderer = vulkan ; trailing\nfullscreen = yes\nscale=1.5\n";
    CHECK_EQ(nue_config_load_text(text, std::strlen(text)), NUE_OK);
    CHECK_EQ(nue_config_get_int("global", 0), 5);
    CHECK(nue_config_get_bool("gfx.fullscreen", NUE_FALSE));
    CHECK_NEAR(nue_config_get_double("gfx.scale", 0), 1.5, 1e-12);
    char buf[32]; size_t need;
    nue_config_get_string("gfx.renderer", "", buf, sizeof(buf), &need);
    CHECK_STR(buf, "vulkan");
    CHECK_EQ(nue_config_get_string("gfx.renderer", "", buf, 4, &need), NUE_OK);   // truncation, size query
    CHECK_STR(buf, "vul");
    CHECK_EQ(need, 7u);
    const char* bad = "[open\nx=1\n";
    CHECK_EQ(nue_config_load_text(bad, std::strlen(bad)), NUE_ERR_CORRUPT);
}

TEST_CASE(config_declare_clamps_and_watch) {
    nue_test::Engine e;
    nue_config_decl d;
    NUE_STRUCT_INIT(&d);
    d.key = "audio.master";
    d.type = NUE_CONFIG_DOUBLE;
    d.default_double = 1.0;
    d.min_value = 0.0;
    d.max_value = 2.0;
    CHECK_EQ(nue_config_declare(&d), NUE_OK);
    CHECK_NEAR(nue_config_get_double("audio.master", 0), 1.0, 1e-12);
    nue_config_set_double("audio.master", 7.0);
    CHECK_NEAR(nue_config_get_double("audio.master", 0), 2.0, 1e-12);   // clamped on read

    static int hits;
    static std::string last;
    hits = 0;
    nue_config_watch_id w = 0;
    nue_config_watch("audio.", [](void*, const char* key) { ++hits; last = key; }, nullptr, &w);
    nue_config_set_double("audio.master", 0.5);
    nue_config_set_double("audio.master", 0.5);   // unchanged: no notification
    nue_config_set_int("video.x", 1);             // other prefix
    CHECK_EQ(hits, 1);
    CHECK_STR(last.c_str(), "audio.master");
    CHECK_EQ(nue_config_unwatch(w), NUE_OK);
    nue_config_set_double("audio.master", 0.6);
    CHECK_EQ(hits, 1);
}

TEST_CASE(log_levels_tags_ring_and_callback_sink) {
    nue_test::Engine e;
    static std::string captured;
    captured.clear();
    nue_log_sink_desc sd;
    NUE_STRUCT_INIT(&sd);
    sd.kind = NUE_LOG_SINK_CALLBACK;
    sd.min_level = NUE_LOG_TRACE;
    sd.callback = [](void*, const nue_log_record* r) { captured += std::string(r->tag) + ":" + r->message + "|"; };
    nue_log_sink_id id = 0;
    CHECK_EQ(nue_log_add_sink(&sd, &id), NUE_OK);
    nue_log_set_level(NUE_LOG_INFO);
    NUE_LOGD("t", "hidden");
    NUE_LOGI("t", "shown %d", 7);
    nue_log_set_tag_level("noisy", NUE_LOG_ERROR);
    NUE_LOGW("noisy", "suppressed");
    NUE_LOGE("noisy", "kept");
    CHECK_STR(captured.c_str(), "t:shown 7|noisy:kept|");
    char ring[4096];
    size_t need = 0;
    CHECK_EQ(nue_log_get_recent(ring, sizeof(ring), &need), NUE_OK);
    CHECK(std::strstr(ring, "shown 7") != nullptr);
    CHECK_EQ(nue_log_remove_sink(id), NUE_OK);
    NUE_LOGI("t", "after removal");
    CHECK(captured.find("after removal") == std::string::npos);
    std::string long_msg(3000, 'z');
    NUE_LOGI("t", "%s", long_msg.c_str());   // longer than the stack buffer
    nue_log_get_recent(nullptr, 0, &need);
    CHECK(need > 3000);
}

TEST_CASE(log_file_sink_rotates) {
    nue_test::Engine e(NUE_INIT_DETERMINISTIC, true);
    nue_log_set_level(NUE_LOG_INFO);
    nue_log_sink_desc sd;
    NUE_STRUCT_INIT(&sd);
    sd.kind = NUE_LOG_SINK_FILE;
    sd.path = "user:logs/rotate.log";
    sd.max_bytes = 2000;
    sd.keep_files = 2;
    CHECK_EQ(nue_log_add_sink(&sd, nullptr), NUE_OK);
    for (int i = 0; i < 100; ++i) NUE_LOGI("rot", "line number %d with some padding to make it longer", i);
    nue_log_flush();
    CHECK(nue_vfs_exists("user:logs/rotate.log"));
    CHECK(nue_vfs_exists("user:logs/rotate.log.1"));
    CHECK(nue_vfs_exists("user:logs/rotate.log.2"));
    CHECK(!nue_vfs_exists("user:logs/rotate.log.3"));
}

TEST_CASE(threads_mutex_cond_semaphore_tls) {
    nue_test::Engine e;
    static std::atomic<int> counter;
    counter = 0;
    nue_mutex m;
    CHECK_EQ(nue_mutex_create(NUE_FALSE, &m), NUE_OK);
    nue_thread threads[4];
    static nue_mutex g_m;
    g_m = m;
    static int shared;
    shared = 0;
    for (auto& t : threads) {
        nue_thread_desc td;
        NUE_STRUCT_INIT(&td);
        td.name = "worker";
        CHECK_EQ(nue_thread_create(&td, [](void*) -> int32_t {
            for (int i = 0; i < 1000; ++i) { nue_mutex_lock(g_m); ++shared; nue_mutex_unlock(g_m); }
            ++counter;
            return 7;
        }, nullptr, &t), NUE_OK);
    }
    for (auto& t : threads) { int32_t code = 0; CHECK_EQ(nue_thread_join(t, &code), NUE_OK); CHECK_EQ(code, 7); }
    CHECK_EQ(shared, 4000);
    CHECK_EQ(counter.load(), 4);
    CHECK(nue_mutex_trylock(m));
    nue_mutex_unlock(m);
    nue_mutex_destroy(m);

    // semaphore + timeout
    nue_semaphore s;
    nue_semaphore_create(0, &s);
    CHECK_EQ(nue_semaphore_wait(s, 0), NUE_ERR_TIMEOUT);
    nue_semaphore_post(s);
    CHECK_EQ(nue_semaphore_wait(s, 0), NUE_OK);
    nue_semaphore_destroy(s);

    // condition variable handshake
    static nue_mutex cm; static nue_cond cv; static bool ready;
    nue_mutex_create(NUE_FALSE, &cm); nue_cond_create(&cv); ready = false;
    nue_thread t;
    nue_thread_create(nullptr, [](void*) -> int32_t { nue_mutex_lock(cm); ready = true; nue_cond_signal(cv); nue_mutex_unlock(cm); return 0; }, nullptr, &t);
    nue_mutex_lock(cm);
    while (!ready) CHECK_EQ(nue_cond_wait(cv, cm, 5000000000ll), NUE_OK);
    nue_mutex_unlock(cm);
    nue_thread_join(t, nullptr);
    nue_cond_destroy(cv); nue_mutex_destroy(cm);

    // TLS values are per thread and destructors run at thread exit
    static std::atomic<int> destroyed; destroyed = 0;
    nue_tls_id tls = 0;
    CHECK_EQ(nue_tls_create([](void*) { ++destroyed; }, &tls), NUE_OK);
    static nue_tls_id g_tls; g_tls = tls;
    nue_tls_set(tls, (void*)0x1);
    nue_thread t2;
    nue_thread_create(nullptr, [](void*) -> int32_t { int32_t r = nue_tls_get(g_tls) == nullptr ? 1 : 0; nue_tls_set(g_tls, (void*)0x2); return r; }, nullptr, &t2);
    int32_t code = 0;
    nue_thread_join(t2, &code);
    CHECK_EQ(code, 1);                          // fresh thread saw an empty slot
    CHECK(nue_tls_get(tls) == (void*)0x1);      // ours untouched
    CHECK_EQ(destroyed.load(), 1);              // the other thread's value was destroyed at its exit
    nue_tls_destroy(tls);
}

TEST_CASE(deterministic_time_and_sleep) {
    nue_test::Engine e;
    int64_t t0 = nue_time_ns();
    nue_sleep_ns(5000000, NUE_SLEEP_PRECISE);       // virtual: returns at once, advances the clock
    CHECK_EQ(nue_time_ns() - t0, 5000000);
    CHECK_EQ(nue_null_advance_time(1000), NUE_OK);
    CHECK_EQ(nue_time_ns() - t0, 5001000);
    nue_sleep_until_ns(t0 + 9000000, NUE_SLEEP_COARSE);
    CHECK_EQ(nue_time_ns() - t0, 9000000);
}

TEST_CASE(real_clock_is_monotonic_and_precise_sleep_is_close) {
    // no engine: the OS clock
    int64_t a = nue_time_ns();
    nue_sleep_ns(3000000, NUE_SLEEP_PRECISE);
    int64_t b = nue_time_ns();
    CHECK(b - a >= 3000000);
    CHECK(b - a < 500000000);   // very generous: loaded CI machines can stall a thread for 100 ms+; the lower bound is the real check
    CHECK(nue_time_counter_frequency() > 0);
}

TEST_CASE(allocator_hooks_and_alignment) {
    static int allocs, frees;
    allocs = frees = 0;
    nue_allocator al;
    NUE_STRUCT_INIT(&al);
    al.alloc = [](void*, size_t size, size_t) -> void* { ++allocs; return std::malloc(size); };
    al.free = [](void*, void* p, size_t) { ++frees; std::free(p); };
    {
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.backend = NUE_BACKEND_NULL;
        d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD;
        d.user_data_path = nullptr;
        d.allocator = &al;
        std::string tmp = nue_test::unique_temp_dir("alloc");
        d.user_data_path = tmp.c_str();
        CHECK_EQ(nue_init(&d), NUE_OK);
        void* p = nue_alloc(100, 64);
        CHECK(p != nullptr);
        CHECK_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);
        std::memset(p, 0xAB, 100);
        p = nue_realloc(p, 400, 64);
        CHECK_EQ(reinterpret_cast<uint8_t*>(p)[99], 0xAB);
        nue_free(p);
        CHECK(allocs >= 2);
        CHECK_EQ(allocs, frees);
        nue_shutdown();
        std::filesystem::remove_all(tmp);
    }
}

TEST_MAIN_BEGIN
    RUN(init_refcount_and_version_check);
    RUN(shutdown_without_init_is_harmless);
    RUN(hash_known_vectors);
    RUN(config_types_and_ini_roundtrip);
    RUN(config_text_parsing_and_errors);
    RUN(config_declare_clamps_and_watch);
    RUN(log_levels_tags_ring_and_callback_sink);
    RUN(log_file_sink_rotates);
    RUN(threads_mutex_cond_semaphore_tls);
    RUN(deterministic_time_and_sleep);
    RUN(real_clock_is_monotonic_and_precise_sleep_is_close);
    RUN(allocator_hooks_and_alignment);
TEST_MAIN_END
