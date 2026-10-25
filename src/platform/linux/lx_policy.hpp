// Policies that turn "what machine is this" into concrete settings for SDL3 and the loaders:
// audio driver order and latency, video driver and fullscreen behaviour, input hints.
// The result is plain data (environment variables and SDL hint strings): the SDL3 glue applies
// it before SDL_Init, and tests check it without a window, a sound card or SDL.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "lx_fs.hpp"
#include "lx_session.hpp"
#include "lx_xdg.hpp"

namespace nue::lx {

struct HintSet {
    std::vector<std::pair<std::string, std::string>> env;    // setenv(name, value, overwrite = false)
    std::vector<std::pair<std::string, std::string>> hints;  // SDL_SetHint(name, value)
    std::vector<std::string> notes;
    std::vector<std::string> warnings;
    void hint(const char* n, std::string v) { hints.emplace_back(n, std::move(v)); }
    void setenv_(const char* n, std::string v) { env.emplace_back(n, std::move(v)); }
};

// ---- audio ---------------------------------------------------------------------------
struct AudioOptions {
    bool null_device = false;      // force the SDL "dummy" driver (tests, CI, --no-audio)
    bool low_latency = true;
    int sample_rate = 48000;
    int buffer_frames = 512;       // device buffer for the mixer; ~10.7 ms at 48 kHz
    std::string app_name = "NUE";
    std::string driver;            // explicit SDL audio driver, empty = automatic
};
struct AudioAvailability {
    bool pipewire_socket = false, pulse_socket = false, alsa_devices = false;
};
AudioAvailability detect_audio(const IEnv& env, const IFileSystem& fs, const XdgDirs& xdg);
HintSet plan_audio(const AudioAvailability& a, const AudioOptions& o, const SessionInfo& s);

// ---- video / window ------------------------------------------------------------------
enum class FullscreenMode { Windowed, Borderless, Exclusive };
struct VideoOptions {
    bool headless = false;                 // offscreen driver, no window on the desktop (tests)
    std::string driver;                    // explicit SDL video driver
    FullscreenMode requested = FullscreenMode::Borderless;
    bool grab_cursor_fullscreen = true;
};
struct VideoPlan {
    HintSet hints;
    FullscreenMode fullscreen = FullscreenMode::Borderless;  // what the backend will actually use
    bool allow_video_mode_change = false;  // only exclusive-like behaviour on X11 outside gamescope
    bool use_pixel_density = true;         // size the swap chain in pixels, not logical points
    bool relative_mouse_in_game = true;
    bool confine_cursor = true;
    bool prefer_desktop_fullscreen = true;
    bool respect_compositor_scale = true;  // Wayland fractional scaling is reported through pixel density
};
VideoPlan plan_video(const VideoOptions& o, const SessionInfo& s);

// ---- input ---------------------------------------------------------------------------
struct InputDevice {
    std::string name;
    uint16_t vendor = 0, product = 0;
    std::string handlers;
    bool is_gamepad = false;     // has a js handler or a known controller id
    bool steam_virtual = false;  // Steam Input virtual gamepad (28de:11ff)
    bool steam_deck = false;     // Deck built-in controller (28de:1205)
};
std::vector<InputDevice> read_input_devices(const IFileSystem& fs);
struct InputAdvice {
    bool uaccess_rules_present = false;  // systemd/udev gives logged-in users access to joysticks
    bool steam_rules_present = false;
    bool uinput_present = false;
    std::vector<std::string> notes;
};
InputAdvice check_input_setup(const IFileSystem& fs);
HintSet plan_input(const std::vector<InputDevice>& devs, const SessionInfo& s);

}  // namespace nue::lx
