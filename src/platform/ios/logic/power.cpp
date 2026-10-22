#include "power.hpp"

#include <algorithm>

namespace nue::ios {

ThermalLevel thermal_from_raw(int raw) {
    switch (raw) {
    case 0: return ThermalLevel::nominal;
    case 1: return ThermalLevel::fair;
    case 2: return ThermalLevel::serious;
    default: return ThermalLevel::critical;
    }
}

const char* thermal_name(ThermalLevel t) {
    switch (t) {
    case ThermalLevel::nominal: return "nominal";
    case ThermalLevel::fair: return "fair";
    case ThermalLevel::serious: return "serious";
    case ThermalLevel::critical: return "critical";
    }
    return "?";
}

MemoryPressure classify_memory(uint64_t available, const MemoryThresholds& t) {
    if (available < t.critical_below) return MemoryPressure::critical;
    if (available < t.high_below) return MemoryPressure::high;
    if (available < t.moderate_below) return MemoryPressure::moderate;
    return MemoryPressure::normal;
}

const char* memory_pressure_name(MemoryPressure m) {
    switch (m) {
    case MemoryPressure::normal: return "normal";
    case MemoryPressure::moderate: return "moderate";
    case MemoryPressure::high: return "high";
    case MemoryPressure::critical: return "critical";
    }
    return "?";
}

QualityAdvice advise_quality(ThermalLevel thermal, bool low_power, uint16_t target_fps) {
    QualityAdvice a{target_fps, 100, false, false};
    switch (thermal) {
    case ThermalLevel::nominal: break;
    case ThermalLevel::fair: break;   // informational only: no visible change
    case ThermalLevel::serious:
        a.max_fps = std::min<uint16_t>(a.max_fps ? a.max_fps : 60, 30);
        a.resolution_scale_percent = 85;
        a.reduce_effects = true;
        break;
    case ThermalLevel::critical:
        a.max_fps = std::min<uint16_t>(a.max_fps ? a.max_fps : 60, 30);
        a.resolution_scale_percent = 70;
        a.reduce_effects = true;
        a.warn_player = true;
        break;
    }
    if (low_power) {
        a.max_fps = std::min<uint16_t>(a.max_fps ? a.max_fps : 60, 30);
        a.reduce_effects = true;
    }
    return a;
}

FrameRateRange choose_frame_rate(uint16_t display_max_hz, uint16_t cap_fps) {
    float cap = cap_fps ? static_cast<float>(cap_fps) : static_cast<float>(display_max_hz);
    cap = std::min(cap, static_cast<float>(display_max_hz));
    if (cap < 1.0f) cap = 60.0f;
    // The system picks a refresh rate that is a divisor/multiple inside [minimum, maximum]. For a fixed 60 or 30 target use an exact
    // range so ProMotion does not drift (e.g. 48 or 80 Hz) and frame pacing stays regular.
    return {cap, cap, cap};
}

}  // namespace nue::ios
