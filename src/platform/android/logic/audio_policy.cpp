#include "audio_policy.hpp"

#include <algorithm>

namespace nue::android {

AudioRoute route_from_device_type(int t) {
    switch (t) {
    case 1:
    case 2: return AudioRoute::Speaker;
    case 3:
    case 4:
    case 5:
    case 6: return AudioRoute::Wired;
    case 7:
    case 8:
    case 23:
    case 26:
    case 27:
    case 30: return AudioRoute::Bluetooth;
    case 9:
    case 10:
    case 25: return AudioRoute::Hdmi;
    case 11:
    case 12:
    case 22: return AudioRoute::Usb;
    default: return AudioRoute::Unknown;
    }
}

bool route_has_high_latency(AudioRoute r, int device_type) {
    if (r != AudioRoute::Bluetooth) return false;
    // BLE audio (LE Audio) is better than A2DP / SCO but still tens of ms above the wired path.
    (void)device_type;
    return true;
}

AudioPlan plan_audio(const AudioProps& p, int desired_rate, int target_latency_ms) {
    AudioPlan plan;
    plan.sample_rate = p.native_sample_rate > 0 ? p.native_sample_rate : 48000;
    plan.needs_resample = desired_rate > 0 && desired_rate != plan.sample_rate;
    plan.low_latency_mode = p.low_latency || p.pro;
    int burst = p.frames_per_buffer > 0 ? p.frames_per_buffer : 192;
    long long wanted = (long long)plan.sample_rate * std::max(1, target_latency_ms) / 1000;
    long long bursts = std::max<long long>(2, (wanted + burst - 1) / burst);
    long long frames = std::min<long long>(bursts * burst, plan.sample_rate); // at most one second
    // keep it a whole number of bursts after clamping
    frames = std::max<long long>(2 * burst, frames / burst * burst);
    plan.buffer_frames = static_cast<int>(frames);
    return plan;
}

FocusAction AudioFocusModel::on_change(int change) {
    switch (change) {
    case 1: // GAIN
        lost_permanently_ = false;
        if (paused_) {
            paused_ = false;
            ducked_ = false;
            return FocusAction::Resume;
        }
        if (ducked_) {
            ducked_ = false;
            return FocusAction::Unduck;
        }
        return FocusAction::None;
    case -1: // LOSS: another app took over for good; do not resume by ourselves
        lost_permanently_ = true;
        ducked_ = false;
        if (!paused_) {
            paused_ = true;
            return FocusAction::Pause;
        }
        return FocusAction::None;
    case -2: // LOSS_TRANSIENT
        if (!paused_) {
            paused_ = true;
            ducked_ = false;
            return FocusAction::Pause;
        }
        return FocusAction::None;
    case -3: // LOSS_TRANSIENT_CAN_DUCK
        if (!paused_ && !ducked_) {
            ducked_ = true;
            return FocusAction::Duck;
        }
        return FocusAction::None;
    default: return FocusAction::None;
    }
}

} // namespace nue::android
