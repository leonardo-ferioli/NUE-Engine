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


nue_app_state map_app_state(LifecycleState s) {
    switch (s) {
    case LifecycleState::active: return NUE_APP_STATE_FOREGROUND;
    case LifecycleState::inactive: return NUE_APP_STATE_INACTIVE;
    case LifecycleState::background: return NUE_APP_STATE_BACKGROUND;
    case LifecycleState::not_running: return NUE_APP_STATE_TERMINATING;
    }
    return NUE_APP_STATE_FOREGROUND;
}

nue_memory_pressure map_pressure(MemoryPressure p) {
    switch (p) {
    case MemoryPressure::normal: return NUE_PRESSURE_NONE;
    case MemoryPressure::moderate: return NUE_PRESSURE_MODERATE;
    case MemoryPressure::high: return NUE_PRESSURE_HIGH;
    case MemoryPressure::critical: return NUE_PRESSURE_CRITICAL;
    }
    return NUE_PRESSURE_NONE;
}

void fill_memory_info(nue_memory_info* m, uint64_t footprint, uint64_t os_available, uint64_t working_set, MemoryPressure pressure) {
    m->process_used = footprint;
    if (os_available != 0) {
        m->process_limit = footprint + os_available;
        m->process_available = os_available;
    } else {
        m->process_limit = 0;                         // no OS figure: unknown, never a made-up number
        m->process_available = m->physical_available;
    }
    m->gpu_dedicated_total = 0;
    m->gpu_dedicated_available = 0;
    m->gpu_shared_total = working_set;
    m->unified_memory = NUE_TRUE;
    m->pressure = map_pressure(pressure);
}

nue_event_memory make_memory_payload(MemoryPressure level, uint64_t available) {
    nue_event_memory e{};
    e.level = static_cast<uint32_t>(map_pressure(level));
    e.available_bytes = available;
    return e;
}

nue_format_support format_support(nue_format_family f, const MetalProbe& p, const DeviceCaps& c) {
    auto has = [&](TexFormat t) { return c.texture_format[static_cast<size_t>(t)]; };
    const nue_format_support color = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_FILTER | NUE_FMTSUP_MIPMAPS;
    const nue_format_support target = NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_BLEND | NUE_FMTSUP_MSAA;
    const nue_format_support depth = NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET | NUE_FMTSUP_MSAA;
    const bool astc = has(TexFormat::astc4x4);
    const bool bc = has(TexFormat::bc1);
    const int fam = p.highest_apple_family;
    switch (f) {
    case NUE_FMT_RGBA8: case NUE_FMT_BGRA8: return color | target | NUE_FMTSUP_SRGB;
    case NUE_FMT_RGB565: return has(TexFormat::r5g6b5) ? color | target : 0;
    case NUE_FMT_RGBA4444: return has(TexFormat::a4r4g4b4) ? color | target : 0;
    case NUE_FMT_RGB5A1: return has(TexFormat::a1r5g5b5) ? color | target : 0;
    case NUE_FMT_R8: case NUE_FMT_RG8: case NUE_FMT_R16F: case NUE_FMT_RGBA16F: case NUE_FMT_RGB10A2: return color | target;
    // 32-bit float: filtering is conservative (not claimed) until measured on hardware.
    case NUE_FMT_RGBA32F: return NUE_FMTSUP_SAMPLE | NUE_FMTSUP_RENDER_TARGET;
    case NUE_FMT_DEPTH16: case NUE_FMT_DEPTH32F: case NUE_FMT_DEPTH32F_STENCIL8: return depth;
    case NUE_FMT_DEPTH24_STENCIL8: return c.depth24_stencil8 ? depth : 0;   // a Mac-only pixel format
    case NUE_FMT_BC1: case NUE_FMT_BC2: case NUE_FMT_BC3: case NUE_FMT_BC7: return bc ? color | NUE_FMTSUP_SRGB : 0;
    case NUE_FMT_BC4: case NUE_FMT_BC5: case NUE_FMT_BC6H: return bc ? color : 0;
    case NUE_FMT_ETC1: case NUE_FMT_EAC_R11: case NUE_FMT_EAC_RG11: return has(TexFormat::etc2_rgba8) ? color : 0;   // ETC1 data decodes through ETC2_RGB8
    case NUE_FMT_ETC2_RGB: case NUE_FMT_ETC2_RGBA: return has(TexFormat::etc2_rgba8) ? color | NUE_FMTSUP_SRGB : 0;
    case NUE_FMT_ASTC_LDR: return astc ? color | NUE_FMTSUP_SRGB : 0;
    case NUE_FMT_ASTC_HDR: return (astc && fam >= 6) ? color : 0;            // A13 / Apple6 and later
    case NUE_FMT_PVRTC1: return (fam >= 1 && !bc) ? color | NUE_FMTSUP_SRGB : 0;   // every iPhone/iPad GPU; not Apple-silicon Macs
    case NUE_FMT_PVRTC2: case NUE_FMT_ATC: return 0;
    case NUE_FMT_FAMILY_COUNT: break;
    }
    return 0;
}

void fill_gpu_caps(nue_gpu_caps* g, const MetalProbe& p, const DeviceCaps& c) {
    const uint32_t size = g->struct_size;
    std::memset(g, 0, sizeof *g);
    g->struct_size = size ? size : static_cast<uint32_t>(sizeof *g);
    g->probed = NUE_TRUE;
    g->renderer = NUE_RENDERER_METAL;
    for (int i = 0; i < static_cast<int>(NUE_FMT_FAMILY_COUNT); ++i) g->format[i] = format_support(static_cast<nue_format_family>(i), p, c);
    const int fam = p.highest_apple_family;
    g->max_texture_2d = c.max_texture_size;
    g->max_texture_3d = 2048;
    g->max_texture_cube = c.max_texture_size;
    g->max_array_layers = 2048;
    g->max_anisotropy = c.max_anisotropy;
    g->max_render_targets = fam >= 2 ? 8 : 4;
    g->max_msaa_samples = c.max_samples;
    g->unified_memory = p.unified_memory ? NUE_TRUE : NUE_FALSE;
    g->npot_mipmaps = NUE_TRUE;
    g->texture_compare = NUE_TRUE;
    g->compute = NUE_TRUE;
    g->instancing = c.instancing ? NUE_TRUE : NUE_FALSE;
    g->tessellation = fam >= 3 ? NUE_TRUE : NUE_FALSE;
    g->dedicated_memory_bytes = 0;   // unified memory
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
