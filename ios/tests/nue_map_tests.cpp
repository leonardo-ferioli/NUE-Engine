// Host tests for the mapping between the iOS backend and the public NUE types (uses the real nue/*.h headers).
#include <cstdio>
#include <cstring>

#include "nue_map.hpp"

using namespace nue::ios;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static nue_power_request zero_request() {
    nue_power_request r;
    std::memset(&r, 0, sizeof r);
    r.struct_size = sizeof r;
    return r;
}

int main() {
    // thermal / battery
    CHECK(map_thermal(ThermalLevel::nominal) == NUE_THERMAL_NOMINAL);
    CHECK(map_thermal(ThermalLevel::critical) == NUE_THERMAL_CRITICAL);
    CHECK(map_thermal(thermal_from_raw(2)) == NUE_THERMAL_SERIOUS);
    CHECK(map_battery_state(1) == NUE_POWER_ON_BATTERY && map_battery_state(2) == NUE_POWER_CHARGING);
    CHECK(map_battery_state(3) == NUE_POWER_CHARGED && map_battery_state(0) == NUE_POWER_UNKNOWN && map_battery_state(7) == NUE_POWER_UNKNOWN);
    CHECK(on_ac_power(NUE_POWER_CHARGING) && on_ac_power(NUE_POWER_CHARGED) && !on_ac_power(NUE_POWER_ON_BATTERY));
    CHECK(battery_percent(-1.0f) == -1 && battery_percent(0.0f) == 0 && battery_percent(0.756f) == 76 && battery_percent(1.0f) == 100 && battery_percent(2.0f) == 100);
    float nan = 0.0f; nan = nan / nan;
    CHECK(battery_percent(nan) == -1);

    auto pl = make_power_payload(NUE_THERMAL_FAIR, NUE_POWER_CHARGING, 55, true, NUE_GAME_MODE_UNKNOWN);
    CHECK(pl.thermal == NUE_THERMAL_FAIR && pl.power_state == NUE_POWER_CHARGING && pl.percent == 55 && pl.low_power == 1 && pl.game_mode == 0);

    PowerSnapshot a{NUE_THERMAL_NOMINAL, NUE_POWER_ON_BATTERY, 80, false};
    PowerSnapshot b = a;
    CHECK(power_changes(a, b) == 0);
    b.percent = 79; CHECK(power_changes(a, b) == change_power);
    b = a; b.thermal = NUE_THERMAL_SERIOUS; CHECK(power_changes(a, b) == change_thermal);
    b = a; b.low_power = true; b.state = NUE_POWER_CHARGING; CHECK(power_changes(a, b) == (change_low_power | change_power));

    // QoS
    CHECK(qos_for_priority(NUE_THREAD_PRIORITY_LOW) == Qos::utility);
    CHECK(qos_for_priority(NUE_THREAD_PRIORITY_DEFAULT) == Qos::normal && qos_for_priority(NUE_THREAD_PRIORITY_NORMAL) == Qos::normal);
    CHECK(qos_for_priority(NUE_THREAD_PRIORITY_ABOVE_NORMAL) == Qos::user_initiated);
    CHECK(qos_for_priority(NUE_THREAD_PRIORITY_HIGH) == Qos::user_interactive && qos_for_priority(NUE_THREAD_PRIORITY_REALTIME) == Qos::user_interactive);
    // Round trip is monotonic: a higher requested priority never reads back lower.
    nue_thread_priority order[] = {NUE_THREAD_PRIORITY_LOW, NUE_THREAD_PRIORITY_NORMAL, NUE_THREAD_PRIORITY_ABOVE_NORMAL, NUE_THREAD_PRIORITY_HIGH};
    for (int i = 0; i + 1 < 4; ++i) CHECK(priority_for_qos(qos_for_priority(order[i])) <= priority_for_qos(qos_for_priority(order[i + 1])));
    CHECK(qos_for_core_class(NUE_CPU_EFFICIENCY_CORES) == Qos::background && qos_for_core_class(NUE_CPU_PERFORMANCE_CORES) == Qos::user_initiated);
    CHECK(qos_for_core_class(NUE_CPU_DEFAULT) == Qos::normal);
    CHECK(posix_priority_for(NUE_THREAD_PRIORITY_LOW) < posix_priority_for(NUE_THREAD_PRIORITY_NORMAL));
    CHECK(posix_priority_for(NUE_THREAD_PRIORITY_NORMAL) < posix_priority_for(NUE_THREAD_PRIORITY_HIGH));
    CHECK(posix_priority_for(NUE_THREAD_PRIORITY_REALTIME) <= 47);   // never reaches the real-time band

    // power requests
    nue_power_request r = zero_request();
    auto p = plan_power_request(r);
    CHECK(p.release_all && !p.keep_awake && p.applied == 0 && !p.partial);
    r.keep_awake = NUE_KEEP_AWAKE_DISPLAY;
    p = plan_power_request(r);
    CHECK(!p.release_all && p.keep_awake && (p.applied & NUE_POWER_APPLIED_KEEP_AWAKE) && !p.partial);
    r.keep_awake = NUE_KEEP_AWAKE_SYSTEM;
    CHECK(plan_power_request(r).keep_awake);
    r = zero_request(); r.target_frame_time_us = 16667;
    p = plan_power_request(r);
    CHECK(p.fps > 59.9f && p.fps < 60.1f && (p.applied & NUE_POWER_APPLIED_FRAME_TIME) && !p.partial);
    r = zero_request(); r.target_frame_time_us = 1;
    CHECK(plan_power_request(r).fps <= 240.0f);
    r = zero_request(); r.profile = NUE_PERF_HIGH; r.game_hint = NUE_GAME_HINT_GAMEPLAY; r.keep_awake = NUE_KEEP_AWAKE_DISPLAY;
    p = plan_power_request(r);
    CHECK(p.partial && (p.applied & NUE_POWER_APPLIED_KEEP_AWAKE) && !(p.applied & NUE_POWER_APPLIED_PROFILE) && !(p.applied & NUE_POWER_APPLIED_GAME_HINT));
    r = zero_request(); r.timer_resolution_us = 500;
    p = plan_power_request(r);
    CHECK(p.partial && !(p.applied & NUE_POWER_APPLIED_TIMER));

    // safe area: iPhone landscape with notch (insets l=59 t=0 r=59 b=21 on 852x393)
    auto s = safe_area_rect(852, 393, 59, 0, 59, 21);
    CHECK(s.x == 59 && s.y == 0 && s.w == 734 && s.h == 372);
    s = safe_area_rect(100, 100, 0, 0, 0, 0);
    CHECK(s.x == 0 && s.y == 0 && s.w == 100 && s.h == 100);
    s = safe_area_rect(100, 100, 80, 0, 80, 0);   // insets larger than the window: never negative
    CHECK(s.w == 0 && s.h == 100);
    s = safe_area_rect(100, 100, -5, -5, -5, -5);
    CHECK(s.x == 0 && s.w == 100);

    // storage
    CHECK(sanitize_component("nfsu2", "x") == "nfsu2");
    CHECK(sanitize_component("My Game!", "x") == "My_Game_");
    CHECK(sanitize_component("", "fb") == "fb" && sanitize_component(nullptr, "fb") == "fb");
    CHECK(sanitize_component("..", "fb") == "fb" && sanitize_component(".", "fb") == "fb");
    CHECK(sanitize_component("../../etc", "fb") == ".._.._etc");   // separators cannot survive
    CHECK(sanitize_component("a/b\\c", "fb") == "a_b_c");
    CHECK(storage_subpath(NUE_ROOT_DATA, "NUE", "nfsu2") == "NUE/nfsu2/GameData");
    CHECK(storage_subpath(NUE_ROOT_USER, "NUE", "nfsu2") == "NUE/nfsu2");
    CHECK(storage_subpath(NUE_ROOT_SETTINGS, "NUE", "nfsu2") == "NUE/nfsu2/settings");
    CHECK(storage_subpath(NUE_ROOT_CACHE, nullptr, nullptr) == "NUE/nfsu2");
    CHECK(storage_subpath(NUE_ROOT_CUSTOM, "NUE", "nfsu2").empty());
    CHECK(storage_subpath(999, "NUE", "nfsu2").empty());

    // crash / log
    CHECK(std::strstr(signal_reason(11), "SIGSEGV") && std::strstr(signal_reason(6), "SIGABRT") && std::strcmp(signal_reason(1234), "signal") == 0);
    CHECK(os_log_type_for(NUE_LOG_DEBUG) == 2 && os_log_type_for(NUE_LOG_INFO) == 1 && os_log_type_for(NUE_LOG_ERROR) == 0x10 && os_log_type_for(NUE_LOG_FATAL) == 0x11);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
