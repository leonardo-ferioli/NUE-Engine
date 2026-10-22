// Thermal governor + ADPF target planning.  Pure logic.
#pragma once
#include <cstdint>

namespace nue::android {

enum class ThermalLevel : int32_t { Nominal = 0, Fair, Serious, Critical };

// PowerManager.THERMAL_STATUS_*: NONE 0, LIGHT 1, MODERATE 2, SEVERE 3, CRITICAL 4, EMERGENCY 5, SHUTDOWN 6
ThermalLevel thermal_level_from_status(int status);

struct ThermalAdvice {
    ThermalLevel level = ThermalLevel::Nominal;
    int fps_cap = 0;           // 0 = no cap
    float quality_scale = 1.0f; // 0..1 hint for internal resolution / effects
    bool drop_performance_mode = false;
};

struct ThermalConfig {
    float target_fps = 60.0f;
    // Headroom (PowerManager.getThermalHeadroom): 1.0 = severe throttling.  Acting before the status flips
    // gives a gentler landing; the thresholds have hysteresis so the quality does not oscillate.
    float headroom_warn = 0.85f;
    float headroom_clear = 0.70f;
    int relax_ms = 20000;       // wait this long below the clear threshold before restoring quality
};

class ThermalGovernor {
public:
    explicit ThermalGovernor(ThermalConfig cfg = {}) : cfg_(cfg) {}
    // `headroom` is NaN when unsupported.  Returns the advice valid from now on.
    const ThermalAdvice& update(int status, float headroom, int64_t now_ms);
    const ThermalAdvice& advice() const { return advice_; }

private:
    ThermalAdvice make(ThermalLevel l) const;
    ThermalConfig cfg_;
    ThermalAdvice advice_;
    int64_t cool_since_ms_ = -1;
};

// ADPF: the work duration the hint session is told to meet.  A frame of 16.7 ms is split into game work
// (the session threads) and slack for the compositor; 85 % lets the governor raise clocks before a miss.
int64_t adpf_target_ns(float fps, float fraction = 0.85f);

// Decides whether the session target must be re-sent (fps changed by more than 1 %).
class AdpfTargetTracker {
public:
    bool needs_update(int64_t new_target_ns);
    int64_t current() const { return current_; }

private:
    int64_t current_ = 0;
};

} // namespace nue::android
