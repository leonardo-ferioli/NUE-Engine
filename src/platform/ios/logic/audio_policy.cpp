#include "audio_policy.hpp"

namespace nue::ios {

SessionConfig session_config_for(AudioMixPolicy p) {
    switch (p) {
    case AudioMixPolicy::exclusive: return {SessionCategory::playback, false, false, false};
    case AudioMixPolicy::respect_silent: return {SessionCategory::solo_ambient, false, false, false};
    case AudioMixPolicy::mix_with_others: return {SessionCategory::playback, true, false, false};
    case AudioMixPolicy::duck_others: return {SessionCategory::playback, true, true, false};
    }
    return {SessionCategory::playback, false, false, false};
}

AudioAction on_interruption(InterruptionPhase phase, bool should_resume, bool app_active) {
    if (phase == InterruptionPhase::began) return AudioAction::pause;
    return (should_resume && app_active) ? AudioAction::resume : AudioAction::none;
}

RouteChange route_change_from_raw(int reason) {
    switch (reason) {
    case 1: return RouteChange::device_added;
    case 2: return RouteChange::device_removed;
    case 3: return RouteChange::category_changed;
    case 8: return RouteChange::configuration_changed;
    default: return RouteChange::other;
    }
}

AudioAction on_route_change(RouteChange c) { return c == RouteChange::device_removed ? AudioAction::pause : AudioAction::none; }

double preferred_io_buffer_seconds(RouteKind k, bool low_power) {
    double s;
    switch (k) {
    case RouteKind::wired: case RouteKind::usb: s = 0.005; break;
    case RouteKind::built_in_speaker: s = 0.008; break;
    case RouteKind::airplay: s = 0.040; break;
    case RouteKind::bluetooth_a2dp: case RouteKind::bluetooth_le: s = 0.020; break;   // advisory only
    case RouteKind::other: s = 0.010; break;
    }
    if (low_power && s < 0.010) s = 0.010;   // fewer wake-ups saves battery
    return s;
}

double preferred_sample_rate(RouteKind k) {
    switch (k) {
    case RouteKind::bluetooth_a2dp: case RouteKind::bluetooth_le: case RouteKind::airplay: return 0;   // the codec decides
    default: return 48000.0;   // native rate of every current iPhone/iPad speaker and wired path: avoids resampling
    }
}

}  // namespace nue::ios
