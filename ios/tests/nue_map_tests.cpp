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

    // ---- API 0.1.1 ----
    CHECK(map_app_state(LifecycleState::active) == NUE_APP_STATE_FOREGROUND && map_app_state(LifecycleState::inactive) == NUE_APP_STATE_INACTIVE);
    CHECK(map_app_state(LifecycleState::background) == NUE_APP_STATE_BACKGROUND && map_app_state(LifecycleState::not_running) == NUE_APP_STATE_TERMINATING);
    CHECK(map_pressure(MemoryPressure::normal) == NUE_PRESSURE_NONE && map_pressure(MemoryPressure::moderate) == NUE_PRESSURE_MODERATE);
    CHECK(map_pressure(MemoryPressure::high) == NUE_PRESSURE_HIGH && map_pressure(MemoryPressure::critical) == NUE_PRESSURE_CRITICAL);
    // The pressure ordering never inverts when mapped.
    for (int i = 0; i < 3; ++i) CHECK(map_pressure(static_cast<MemoryPressure>(i)) < map_pressure(static_cast<MemoryPressure>(i + 1)));
    auto mp = make_memory_payload(MemoryPressure::high, 123456);
    CHECK(mp.level == NUE_PRESSURE_HIGH && mp.available_bytes == 123456);

    nue_memory_info mi;
    std::memset(&mi, 0, sizeof mi);
    mi.struct_size = sizeof mi; mi.physical_total = 6ull << 30; mi.physical_available = 2ull << 30;
    fill_memory_info(&mi, 400ull << 20, 1500ull << 20, 5ull << 30, MemoryPressure::moderate);
    CHECK(mi.struct_size == sizeof mi && mi.process_used == (400ull << 20) && mi.process_limit == (1900ull << 20) && mi.process_available == (1500ull << 20));
    CHECK(mi.unified_memory && mi.gpu_dedicated_total == 0 && mi.gpu_shared_total == (5ull << 30) && mi.pressure == NUE_PRESSURE_MODERATE);
    fill_memory_info(&mi, 400ull << 20, 0, 0, MemoryPressure::normal);   // no OS figure: limit unknown, available falls back to physical
    CHECK(mi.process_limit == 0 && mi.process_available == mi.physical_available);

    // GPU caps for an iPhone-class GPU (Apple8): no BC, ASTC / ETC2 / PVRTC1 yes, D24S8 no.
    MetalProbe a15; a15.highest_apple_family = 8; a15.unified_memory = true; a15.physical_memory = 6ull << 30;
    DeviceCaps dc = derive_caps(a15);
    nue_gpu_caps gc;
    std::memset(&gc, 0, sizeof gc);
    gc.struct_size = sizeof gc;
    fill_gpu_caps(&gc, a15, dc);
    CHECK(gc.struct_size == sizeof gc && gc.probed && gc.renderer == NUE_RENDERER_METAL && gc.unified_memory);
    CHECK(gc.format[NUE_FMT_BC1] == 0 && gc.format[NUE_FMT_BC3] == 0 && gc.format[NUE_FMT_BC7] == 0);
    CHECK((gc.format[NUE_FMT_ASTC_LDR] & NUE_FMTSUP_SAMPLE) && (gc.format[NUE_FMT_ASTC_LDR] & NUE_FMTSUP_SRGB) && gc.format[NUE_FMT_ASTC_HDR] != 0);
    CHECK(gc.format[NUE_FMT_ETC2_RGBA] != 0 && gc.format[NUE_FMT_ETC1] != 0 && gc.format[NUE_FMT_PVRTC1] != 0);
    CHECK(gc.format[NUE_FMT_PVRTC2] == 0 && gc.format[NUE_FMT_ATC] == 0);
    CHECK(gc.format[NUE_FMT_DEPTH24_STENCIL8] == 0 && (gc.format[NUE_FMT_DEPTH32F_STENCIL8] & NUE_FMTSUP_RENDER_TARGET));
    CHECK((gc.format[NUE_FMT_RGBA8] & NUE_FMTSUP_RENDER_TARGET) && (gc.format[NUE_FMT_RGBA8] & NUE_FMTSUP_SRGB) && (gc.format[NUE_FMT_BGRA8] & NUE_FMTSUP_BLEND));
    CHECK(gc.max_texture_2d == 16384 && gc.max_render_targets == 8 && gc.max_msaa_samples == 4 && gc.dedicated_memory_bytes == 0 && gc.tessellation);
    // Compressed formats are never render targets.
    for (int f = NUE_FMT_BC1; f <= NUE_FMT_ATC; ++f) CHECK(!(gc.format[f] & NUE_FMTSUP_RENDER_TARGET));
    // ASTC HDR needs Apple6+; an A8-class GPU (Apple2) has LDR only; an A7 (Apple1) has neither ASTC nor ETC2, but PVRTC.
    MetalProbe a8; a8.highest_apple_family = 2;
    nue_gpu_caps g8; std::memset(&g8, 0, sizeof g8); g8.struct_size = sizeof g8;
    fill_gpu_caps(&g8, a8, derive_caps(a8));
    CHECK(g8.format[NUE_FMT_ASTC_LDR] != 0 && g8.format[NUE_FMT_ASTC_HDR] == 0 && g8.max_texture_2d == 8192);
    MetalProbe a7; a7.highest_apple_family = 1;
    nue_gpu_caps g7; std::memset(&g7, 0, sizeof g7); g7.struct_size = sizeof g7;
    fill_gpu_caps(&g7, a7, derive_caps(a7));
    CHECK(g7.format[NUE_FMT_ASTC_LDR] == 0 && g7.format[NUE_FMT_ETC2_RGB] == 0 && g7.format[NUE_FMT_PVRTC1] != 0 && g7.max_render_targets == 4);
    // A Mac-class device (BC supported): BC yes, PVRTC no, D24S8 yes.
    MetalProbe mac; mac.highest_apple_family = 7; mac.supports_bc = true;
    nue_gpu_caps gm; std::memset(&gm, 0, sizeof gm); gm.struct_size = sizeof gm;
    fill_gpu_caps(&gm, mac, derive_caps(mac));
    CHECK(gm.format[NUE_FMT_BC1] != 0 && gm.format[NUE_FMT_BC7] != 0 && gm.format[NUE_FMT_PVRTC1] == 0 && gm.format[NUE_FMT_DEPTH24_STENCIL8] != 0);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
