#include "thermal_policy.hpp"

#include <cmath>

namespace nue::android {

ThermalLevel thermal_level_from_status(int status) {
    if (status <= 0) return ThermalLevel::Nominal;
    if (status == 1) return ThermalLevel::Fair;
    if (status <= 3) return ThermalLevel::Serious;
    return ThermalLevel::Critical;
}

ThermalAdvice ThermalGovernor::make(ThermalLevel l) const {
    ThermalAdvice a;
    a.level = l;
    switch (l) {
    case ThermalLevel::Nominal: break;
    case ThermalLevel::Fair:
        a.quality_scale = 0.95f;
        break;
    case ThermalLevel::Serious:
        a.quality_scale = 0.8f;
        a.fps_cap = cfg_.target_fps > 30.0f ? 30 : 0;
        a.drop_performance_mode = true;
        break;
    case ThermalLevel::Critical:
        a.quality_scale = 0.6f;
        a.fps_cap = 30;
        a.drop_performance_mode = true;
        break;
    }
    return a;
}

const ThermalAdvice& ThermalGovernor::update(int status, float headroom, int64_t now_ms) {
    ThermalLevel wanted = thermal_level_from_status(status);
    if (!std::isnan(headroom)) {
        // The headroom sees the trend before the status does: treat a high value as at least Fair/Serious.
        if (headroom >= 1.0f) wanted = wanted < ThermalLevel::Serious ? ThermalLevel::Serious : wanted;
        else if (headroom >= cfg_.headroom_warn) wanted = wanted < ThermalLevel::Fair ? ThermalLevel::Fair : wanted;
    }
    bool cool = wanted < advice_.level;
    if (!cool) {
        cool_since_ms_ = -1;
        if (wanted > advice_.level) advice_ = make(wanted);
        return advice_;
    }
    // Going cooler: only after it stayed cool for relax_ms and the headroom (when known) is below "clear".
    if (!std::isnan(headroom) && headroom > cfg_.headroom_clear) {
        cool_since_ms_ = -1;
        return advice_;
    }
    if (cool_since_ms_ < 0) cool_since_ms_ = now_ms;
    if (now_ms - cool_since_ms_ >= cfg_.relax_ms) {
        // one step at a time so a single cool reading does not undo the protection
        advice_ = make(static_cast<ThermalLevel>(static_cast<int>(advice_.level) - 1));
        cool_since_ms_ = now_ms;
    }
    return advice_;
}

int64_t adpf_target_ns(float fps, float fraction) {
    if (fps <= 0.0f) return 0;
    return static_cast<int64_t>(double(1e9) / fps * fraction);
}

bool AdpfTargetTracker::needs_update(int64_t new_target_ns) {
    if (new_target_ns <= 0) return false;
    if (current_ > 0) {
        double rel = std::fabs(double(new_target_ns - current_)) / double(current_);
        if (rel < 0.01) return false;
    }
    current_ = new_target_ns;
    return true;
}

} // namespace nue::android
