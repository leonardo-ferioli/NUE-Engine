// Diagnostics: reports, notes, breadcrumbs, assertions, counters, frame statistics, trace output; system info and capabilities.
#include "test_env.h"
#include "nue_test.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using nue_test::Engine;

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_CASE(manual_report_contains_notes_breadcrumbs_and_log_tail) {
    Engine e;
    nue_diag_set_note("track", "Olympic Plaza");
    nue_diag_set_note("car", "Skyline");
    nue_diag_set_note("car", nullptr);   // removed again
    nue_diag_breadcrumb("entered race");
    nue_diag_breadcrumb("collision 3");
    NUE_LOGW("test", "something odd happened");
    char dir[512];
    size_t need = 0;
    CHECK_EQ(nue_diag_write_report("unit test", dir, sizeof(dir), &need), NUE_OK);
    std::string text = slurp(std::string(dir) + "/report.txt");
    CHECK(text.find("reason: unit test") != std::string::npos);
    CHECK(text.find("track = Olympic Plaza") != std::string::npos);
    CHECK(text.find("Skyline") == std::string::npos);
    CHECK(text.find("entered race") != std::string::npos);
    CHECK(text.find("collision 3") != std::string::npos);
    CHECK(text.find("something odd happened") != std::string::npos);
    CHECK(text.find("NUE test") != std::string::npos);
    char list[2048];
    CHECK_EQ(nue_diag_list_reports(list, sizeof(list), &need), NUE_OK);
    CHECK(std::string(list).find(dir) != std::string::npos);
    CHECK_EQ(nue_diag_clear_reports(), NUE_OK);
    nue_diag_list_reports(list, sizeof(list), &need);
    CHECK_STR(list, "");
}

TEST_CASE(report_rotation_keeps_the_newest) {
    Engine e;
    // no crash handler in the generic layer, so rotation is exercised through the root the manual reports use
    char dir[512]; size_t need;
    for (int i = 0; i < 5; ++i) CHECK_EQ(nue_diag_write_report("r", dir, sizeof(dir), &need), NUE_OK);
    char list[4096];
    nue_diag_list_reports(list, sizeof(list), &need);
    int lines = 0;
    for (const char* p = list; *p; ++p) lines += *p == '\n';
    CHECK_EQ(lines, 5);
}

TEST_CASE(crash_handler_needs_a_platform_layer) {
    Engine e;
    nue_crash_desc cd;
    NUE_STRUCT_INIT(&cd);
    GENERIC_EQ(nue_diag_install_crash_handler(&cd), NUE_ERR_UNSUPPORTED);
    GENERIC_EQ(nue_diag_uninstall_crash_handler(), NUE_OK_NOOP);
    GENERIC(CHECK(!nue_has_capability(NUE_CAP_CRASH_HANDLER)));
}

TEST_CASE(test_crash_is_guarded) {
    nue_test::Engine e(0);   // not deterministic
    CHECK_EQ(nue_diag_test_crash(NUE_TEST_CRASH_NULL_WRITE), NUE_ERR_ACCESS_DENIED);   // would crash the test process otherwise
}

TEST_CASE(asserts_use_the_handler) {
    Engine e;
    static int hits;
    static std::string last_expr;
    hits = 0;
    nue_diag_set_assert_handler([](void*, const char* expr, const char*, const char*, int) -> nue_assert_action { ++hits; last_expr = expr; return NUE_ASSERT_IGNORE; }, nullptr);
    CHECK_EQ(nue_diag_assert_failed("x > 0", "x must be positive", __FILE__, __LINE__), NUE_ASSERT_IGNORE);
    CHECK_EQ(hits, 1);
    CHECK_STR(last_expr.c_str(), "x > 0");
    nue_diag_set_assert_handler(nullptr, nullptr);
}

TEST_CASE(counters_are_shared_by_name) {
    Engine e;
    nue_counter_id a = 0, b = 0, c = 0;
    CHECK_EQ(nue_counter_register("test.draws", NUE_COUNTER_ACCUMULATOR, &a), NUE_OK);
    CHECK_EQ(nue_counter_register("test.draws", NUE_COUNTER_ACCUMULATOR, &b), NUE_OK);
    CHECK_EQ(a, b);
    nue_counter_register("test.mem", NUE_COUNTER_GAUGE, &c);
    CHECK(a != c);
    nue_counter_add(a, 5);
    nue_counter_add(a, 7);
    nue_counter_set(c, 42);
    CHECK_EQ(nue_counter_get(a), 12);
    CHECK_EQ(nue_counter_get(c), 42);
    CHECK_EQ(nue_counter_get(0), 0);   // invalid ids are ignored
    nue_counter_add(9999, 1);
    uint32_t n = 0;
    nue_counter_snapshot(nullptr, 0, &n);
    CHECK(n >= 2);
}

TEST_CASE(frame_stats_percentiles_on_the_virtual_clock) {
    Engine e;
    // 100 frames: 99 at 16 ms and a single 50 ms hitch
    for (int i = 0; i < 100; ++i) {
        nue_frame_mark();
        nue_null_advance_time(i == 50 ? 50000000 : 16000000);
    }
    nue_frame_mark();
    nue_frame_stats st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_frame_get_stats(0, &st), NUE_OK);
    CHECK_EQ(st.window_frames, 100u);
    CHECK_NEAR(st.frame_ms_p50, 16.0, 0.01);
    CHECK_NEAR(st.frame_ms_p95, 16.0, 0.01);
    CHECK_NEAR(st.frame_ms_max, 50.0, 0.01);
    CHECK_NEAR(st.frame_ms_avg, (99 * 16.0 + 50.0) / 100.0, 0.01);
    CHECK_NEAR(st.fps, 1000.0 / ((99 * 16.0 + 50.0) / 100.0), 0.5);
    CHECK_EQ(st.total_frames, 101u);
    // a narrower window sees only the recent frames
    nue_frame_stats recent;
    NUE_STRUCT_INIT(&recent);
    nue_frame_get_stats(0.5, &recent);
    CHECK(recent.window_frames < 100u);
}

TEST_CASE(trace_writes_chrome_json) {
    Engine e;
    CHECK(!nue_trace_enabled());
    CHECK_EQ(nue_trace_start("user:trace.json"), NUE_OK);
    CHECK(nue_trace_enabled());
    NUE_TRACE_BEGIN("frame");
    NUE_TRACE_BEGIN("update \"quoted\"");
    NUE_TRACE_END();
    NUE_TRACE_END();
    CHECK_EQ(nue_trace_stop(), NUE_OK);
    char native[512]; size_t need;
    nue_vfs_resolve("user:trace.json", NUE_FALSE, native, sizeof(native), &need);
    std::string json = slurp(native);
    CHECK(json.find("\"ph\":\"B\"") != std::string::npos);
    CHECK(json.find("\"ph\":\"E\"") != std::string::npos);
    CHECK(json.find("update \\\"quoted\\\"") != std::string::npos);
    CHECK(json.front() == '[');
}

TEST_CASE(system_info_and_capabilities) {
    Engine e;
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    CHECK_EQ(nue_system_get_info(&si), NUE_OK);
    CHECK(si.cpu_logical_cores >= 1);
    CHECK(si.arch != NUE_ARCH_KIND_UNKNOWN);
    CHECK_EQ(si.display_count, 1u);
    CHECK(nue_has_capability(NUE_CAP_ASYNC_IO));
    CHECK(nue_has_capability(NUE_CAP_AUDIO_OUTPUT));
    CHECK(nue_has_capability(NUE_CAP_ATTACH_NATIVE_WINDOW));
    CHECK(!nue_has_capability(NUE_CAP_TOUCH));
    GENERIC(CHECK(!nue_has_capability(NUE_CAP_GAME_MODE)));
    CHECK(!nue_has_capability(NUE_CAP_COUNT));
    CHECK_STR(nue_capability_name(NUE_CAP_TOUCH), "NUE_CAP_TOUCH");
    CHECK_STR(nue_capability_name(NUE_CAP_GFX_D3D9_LAYER), "NUE_CAP_GFX_D3D9_LAYER");
    uint64_t bits[2] = {0, 0};
    CHECK_EQ(nue_get_capabilities(bits, 2), 2u);
    CHECK(((bits[NUE_CAP_ASYNC_IO / 64] >> (NUE_CAP_ASYNC_IO % 64)) & 1) == 1);
    for (int c = 0; c < NUE_CAP_COUNT; ++c) CHECK(std::strncmp(nue_capability_name(nue_capability(c)), "NUE_CAP_", 8) == 0);
}

TEST_CASE(mixer_unit_helpers) {
    CHECK_NEAR(nue_db_to_linear(0), 1.0, 1e-6);
    CHECK_NEAR(nue_db_to_linear(-6.0206f), 0.5, 1e-4);
    CHECK_NEAR(nue_linear_to_db(0.1f), -20.0, 1e-3);
    CHECK_NEAR(nue_linear_to_db(0), -200.0, 1e-3);
    CHECK_NEAR(nue_centibels_to_linear(0), 1.0, 1e-6);
    CHECK_NEAR(nue_centibels_to_linear(-600), 0.5012, 1e-3);   // -6 dB
    CHECK_NEAR(nue_centibels_to_linear(-10000), 0.0, 1e-9);
    CHECK_EQ(nue_linear_to_centibels(1.0f), 0);
    CHECK_EQ(nue_linear_to_centibels(0.0f), -10000);
    CHECK(std::abs(nue_linear_to_centibels(nue_centibels_to_linear(-1234)) + 1234) <= 1);
    CHECK_NEAR(nue_pan_from_centibels(0), 0.0, 1e-9);
    CHECK_NEAR(nue_pan_from_centibels(10000), 1.0, 1e-6);
    CHECK_NEAR(nue_pan_from_centibels(-10000), -1.0, 1e-6);
    CHECK(nue_pan_from_centibels(-600) < 0);
}

TEST_MAIN_BEGIN
    RUN(manual_report_contains_notes_breadcrumbs_and_log_tail);
    RUN(report_rotation_keeps_the_newest);
    RUN(crash_handler_needs_a_platform_layer);
    RUN(test_crash_is_guarded);
    RUN(asserts_use_the_handler);
    RUN(counters_are_shared_by_name);
    RUN(frame_stats_percentiles_on_the_virtual_clock);
    RUN(trace_writes_chrome_json);
    RUN(system_info_and_capabilities);
    RUN(mixer_unit_helpers);
TEST_MAIN_END
