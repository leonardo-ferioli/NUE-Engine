#include "nue_map.hpp"

#include <algorithm>
#include <cstring>

namespace nue::ios {

nue_thermal_state map_thermal(ThermalLevel t) {
    switch (t) {
    case ThermalLevel::nominal: return NUE_THERMAL_NOMINAL;
    case ThermalLevel::fair: return NUE_THERMAL_FAIR;
    case ThermalLevel::serious: return NUE_THERMAL_SERIOUS;
    case ThermalLevel::critical: return NUE_THERMAL_CRITICAL;
    }
    return NUE_THERMAL_UNKNOWN;
}

nue_power_state map_battery_state(int s) {
    switch (s) {
    case 1: return NUE_POWER_ON_BATTERY;
    case 2: return NUE_POWER_CHARGING;
    case 3: return NUE_POWER_CHARGED;
    default: return NUE_POWER_UNKNOWN;   // 0, and the Simulator (no battery information)
    }
}

bool on_ac_power(nue_power_state s) { return s == NUE_POWER_CHARGING || s == NUE_POWER_CHARGED; }

int32_t battery_percent(float level) {
    if (!(level >= 0.0f)) return -1;   // also rejects NaN
    return static_cast<int32_t>(std::min(level, 1.0f) * 100.0f + 0.5f);
}

nue_event_power make_power_payload(nue_thermal_state thermal, nue_power_state state, int32_t percent, bool low_power, nue_game_mode_state gm) {
    nue_event_power p{};
    p.thermal = static_cast<uint32_t>(thermal);
    p.power_state = static_cast<uint32_t>(state);
    p.percent = percent;
    p.low_power = low_power ? 1 : 0;
    p.game_mode = static_cast<uint8_t>(gm);
    return p;
}

unsigned power_changes(const PowerSnapshot& a, const PowerSnapshot& b) {
    unsigned r = 0;
    if (a.state != b.state || a.percent != b.percent) r |= change_power;
    if (a.thermal != b.thermal) r |= change_thermal;
    if (a.low_power != b.low_power) r |= change_low_power;
    return r;
}

Qos qos_for_priority(nue_thread_priority p) {
    switch (p) {
    case NUE_THREAD_PRIORITY_LOW: return Qos::utility;
    case NUE_THREAD_PRIORITY_DEFAULT:
    case NUE_THREAD_PRIORITY_NORMAL: return Qos::normal;
    case NUE_THREAD_PRIORITY_ABOVE_NORMAL: return Qos::user_initiated;
    case NUE_THREAD_PRIORITY_HIGH:
    case NUE_THREAD_PRIORITY_REALTIME: return Qos::user_interactive;
    }
    return Qos::normal;
}

nue_thread_priority priority_for_qos(Qos q) {
    switch (q) {
    case Qos::user_interactive: return NUE_THREAD_PRIORITY_HIGH;
    case Qos::user_initiated: return NUE_THREAD_PRIORITY_ABOVE_NORMAL;
    case Qos::normal: return NUE_THREAD_PRIORITY_NORMAL;
    case Qos::utility: case Qos::background: return NUE_THREAD_PRIORITY_LOW;
    }
    return NUE_THREAD_PRIORITY_NORMAL;
}

Qos qos_for_core_class(nue_cpu_preference c) {
    switch (c) {
    case NUE_CPU_EFFICIENCY_CORES: return Qos::background;
    case NUE_CPU_PERFORMANCE_CORES: return Qos::user_initiated;
    default: return Qos::normal;
    }
}

int posix_priority_for(nue_thread_priority p) {
    switch (p) {
    case NUE_THREAD_PRIORITY_LOW: return 20;
    case NUE_THREAD_PRIORITY_DEFAULT:
    case NUE_THREAD_PRIORITY_NORMAL: return 31;   // the Darwin default for SCHED_OTHER
    case NUE_THREAD_PRIORITY_ABOVE_NORMAL: return 37;
    case NUE_THREAD_PRIORITY_HIGH: return 45;
    case NUE_THREAD_PRIORITY_REALTIME: return 47;   // capped below the kernel's real-time band on purpose
    }
    return 31;
}

PowerPlan plan_power_request(const nue_power_request& r) {
    PowerPlan p;
    const bool wants_anything = r.profile != NUE_PERF_DEFAULT || r.game_hint != NUE_GAME_HINT_NONE || r.keep_awake != 0 ||
                                r.gpu != NUE_GPU_DEFAULT || r.cpu != NUE_CPU_DEFAULT || r.timer_resolution_us != 0 ||
                                r.target_frame_time_us != 0 || r.disable_power_throttling || r.prefer_low_latency_audio;
    p.release_all = !wants_anything;
    if (r.keep_awake & (NUE_KEEP_AWAKE_DISPLAY | NUE_KEEP_AWAKE_SYSTEM)) {
        p.keep_awake = true;               // iOS has one switch: the idle timer. It keeps the display on, which also keeps the system awake.
        p.applied |= NUE_POWER_APPLIED_KEEP_AWAKE;
    }
    if (r.target_frame_time_us != 0) {
        p.fps = 1000000.0f / static_cast<float>(r.target_frame_time_us);
        if (p.fps > 240.0f) p.fps = 240.0f;
        p.applied |= NUE_POWER_APPLIED_FRAME_TIME;
    }
    // Not honourable on iOS: performance profiles and Game Mode hints (the OS decides; there is no API), GPU/CPU selection, timer
    // resolution, throttling opt-out, audio thread class. A request for any of them makes the result partial.
    if (r.profile != NUE_PERF_DEFAULT || r.game_hint != NUE_GAME_HINT_NONE || r.gpu != NUE_GPU_DEFAULT ||
        r.timer_resolution_us != 0 || r.disable_power_throttling || r.prefer_low_latency_audio)
        p.partial = true;
    if (r.cpu != NUE_CPU_DEFAULT) p.partial = true;   // per-thread only (QoS); nue_thread_set_core_class does the real work
    return p;
}

nue_rect_i safe_area_rect(float w, float h, float l, float t, float r, float b) {
    auto clamp0 = [](float v) { return v < 0 ? 0.0f : v; };
    l = clamp0(l); t = clamp0(t); r = clamp0(r); b = clamp0(b);
    nue_rect_i out{};
    out.x = static_cast<int32_t>(l + 0.5f);
    out.y = static_cast<int32_t>(t + 0.5f);
    out.w = std::max<int32_t>(0, static_cast<int32_t>(w + 0.5f) - out.x - static_cast<int32_t>(r + 0.5f));
    out.h = std::max<int32_t>(0, static_cast<int32_t>(h + 0.5f) - out.y - static_cast<int32_t>(b + 0.5f));
    return out;
}

std::string sanitize_component(const char* s, const char* fallback) {
    std::string r;
    if (s) {
        for (; *s; ++s) {
            const unsigned char c = static_cast<unsigned char>(*s);
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            r.push_back(ok ? static_cast<char>(c) : '_');
        }
    }
    if (r.empty() || r == "." || r == "..") return fallback ? fallback : "NUE";
    return r;
}

std::string storage_subpath(int kind, const char* org, const char* app_id) {
    const std::string base = sanitize_component(org, "NUE") + "/" + sanitize_component(app_id, "nfsu2");
    switch (kind) {
    case NUE_ROOT_DATA: return base + "/GameData";
    case NUE_ROOT_USER: return base;
    case NUE_ROOT_CACHE: return base;
    case NUE_ROOT_SETTINGS: return base + "/settings";
    case NUE_ROOT_TEMP: return base;
    default: return std::string();
    }
}

const char* signal_reason(int sig) {
    switch (sig) {
    case 4: return "SIGILL (illegal instruction)";
    case 5: return "SIGTRAP (trap / breakpoint)";
    case 6: return "SIGABRT (abort)";
    case 8: return "SIGFPE (arithmetic exception)";
    case 10: return "SIGBUS (bus error)";
    case 11: return "SIGSEGV (invalid memory access)";
    case 12: return "SIGSYS (bad system call)";
    default: return "signal";
    }
}

uint8_t os_log_type_for(nue_log_level l) {
    switch (l) {
    case NUE_LOG_TRACE: case NUE_LOG_DEBUG: return 2;   // OS_LOG_TYPE_DEBUG
    case NUE_LOG_INFO: return 1;                        // OS_LOG_TYPE_INFO
    case NUE_LOG_WARN: return 0;                        // OS_LOG_TYPE_DEFAULT
    case NUE_LOG_ERROR: return 0x10;                    // OS_LOG_TYPE_ERROR
    case NUE_LOG_FATAL: return 0x11;                    // OS_LOG_TYPE_FAULT
    default: return 0;
    }
}

}  // namespace nue::ios
