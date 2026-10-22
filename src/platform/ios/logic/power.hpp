// Thermal / Low Power Mode / memory budget / display refresh policy as pure functions.
#pragma once
#include <cstdint>

namespace nue::ios {

enum class ThermalLevel : uint8_t { nominal, fair, serious, critical };
enum class MemoryPressure : uint8_t { normal, moderate, high, critical };

// NSProcessInfoThermalState: 0 nominal, 1 fair, 2 serious, 3 critical. Unknown values are treated as critical (safe side).
ThermalLevel thermal_from_raw(int ns_process_info_thermal_state);
const char* thermal_name(ThermalLevel);

// Remaining process memory as reported by os_proc_available_memory() (bytes before the jetsam limit).
// Thresholds are about the room LEFT, not about a total size, and are a starting policy: the caller may pass its own MemoryThresholds.
struct MemoryThresholds {
    uint64_t moderate_below = 400ull << 20;
    uint64_t high_below = 250ull << 20;
    uint64_t critical_below = 120ull << 20;
};
MemoryPressure classify_memory(uint64_t available_bytes, const MemoryThresholds& t = {});
const char* memory_pressure_name(MemoryPressure);

// What the engine should do about quality. `max_fps` 0 means "no cap beyond the display".
struct QualityAdvice {
    uint16_t max_fps;
    uint8_t resolution_scale_percent;   // 100 = native
    bool reduce_effects;
    bool warn_player;
};
// target_fps: what the game wants (60 for NFSU2). Low Power Mode and thermal states only ever lower it.
QualityAdvice advise_quality(ThermalLevel, bool low_power_mode, uint16_t target_fps);

// CADisplayLink / CAMetalLayer preferred frame rate range (CAFrameRateRange, iOS 15+).
struct FrameRateRange { float minimum, preferred, maximum; };
// display_max_hz: UIScreen.maximumFramesPerSecond (60 or 120). Result honours QualityAdvice::max_fps.
// On ProMotion iPhones the ranges above 60 Hz need CADisableMinimumFrameDurationOnPhone = YES; at 60 or below it is not needed.
FrameRateRange choose_frame_rate(uint16_t display_max_hz, uint16_t cap_fps);

}  // namespace nue::ios
