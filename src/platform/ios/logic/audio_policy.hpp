// AVAudioSession policy as pure decisions. The glue (apple/nue_ios_audio.mm) applies them.
#pragma once
#include <cstdint>

namespace nue::ios {

// How the game's audio coexists with the rest of the device.
enum class AudioMixPolicy : uint8_t {
    exclusive,        // Playback, no mixing: takes over the device, ignores the silent switch (default for a racing game with a soundtrack)
    respect_silent,   // SoloAmbient: stops other audio, obeys the silent switch
    mix_with_others,  // Playback + mixWithOthers: the player's own music keeps playing
    duck_others,      // Playback + duckOthers: music lowers while the game makes sound
};

enum class SessionCategory : uint8_t { solo_ambient, ambient, playback };
struct SessionConfig {
    SessionCategory category;
    bool mix_with_others;
    bool duck_others;
    bool interrupt_spoken_audio_and_mix_with_others;
};
SessionConfig session_config_for(AudioMixPolicy);

enum class InterruptionPhase : uint8_t { began, ended };
enum class AudioAction : uint8_t { none, pause, resume };
// AVAudioSessionInterruptionOptionShouldResume is delivered with `ended`. `app_active`: do not restart audio while the app is not active.
AudioAction on_interruption(InterruptionPhase, bool should_resume, bool app_active);

// AVAudioSessionRouteChangeReason raw values (stable ABI): 0 unknown, 1 newDeviceAvailable, 2 oldDeviceUnavailable, 3 categoryChange,
// 4 override, 6 wakeFromSleep, 7 noSuitableRouteForCategory, 8 routeConfigurationChange.
enum class RouteChange : uint8_t { other, device_added, device_removed, category_changed, configuration_changed };
RouteChange route_change_from_raw(int reason);
// Headphones unplugged: pause the game's audio (Apple HIG for playback). The engine restarts the device if the stream stopped.
AudioAction on_route_change(RouteChange);

enum class RouteKind : uint8_t { built_in_speaker, wired, bluetooth_a2dp, bluetooth_le, airplay, usb, other };
// Preferred IO buffer duration for AVAudioSession.setPreferredIOBufferDuration. Bluetooth is not controllable (the OS decides).
double preferred_io_buffer_seconds(RouteKind, bool low_power_mode);
// Preferred sample rate: the game mixes at 48 kHz-friendly rates; 0 = leave to the system.
double preferred_sample_rate(RouteKind);

}  // namespace nue::ios
