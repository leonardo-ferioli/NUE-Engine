#include "lx_policy.hpp"

#include <algorithm>

#include "lx_text.hpp"

namespace nue::lx {

// ---------------------------------------------------------------------------- audio

AudioAvailability detect_audio(const IEnv& env, const IFileSystem& fs, const XdgDirs& xdg) {
    AudioAvailability a;
    const std::string rt = xdg.runtime_dir;
    std::string pw = env.get_or("PIPEWIRE_REMOTE");
    if (!pw.empty()) a.pipewire_socket = fs.exists(pw[0] == '/' ? pw : rt + "/" + pw);
    else if (!rt.empty()) a.pipewire_socket = fs.exists(rt + "/pipewire-0");
    std::string ps = env.get_or("PULSE_SERVER");
    if (starts_with(ps, "unix:")) a.pulse_socket = fs.exists(ps.substr(5));
    else if (!ps.empty()) a.pulse_socket = true;  // tcp: server, trust the user
    else if (!rt.empty()) a.pulse_socket = fs.exists(rt + "/pulse/native");
    a.alsa_devices = fs.exists("/dev/snd/controlC0") || fs.exists("/proc/asound/cards");
    return a;
}

HintSet plan_audio(const AudioAvailability& a, const AudioOptions& o, const SessionInfo& s) {
    HintSet h;
    h.hint("SDL_AUDIO_DEVICE_APP_NAME", o.app_name);
    h.hint("SDL_AUDIO_DEVICE_STREAM_NAME", o.app_name);
    h.hint("SDL_AUDIO_DEVICE_STREAM_ROLE", "game");
    std::string drivers;
    if (o.null_device) {
        drivers = "dummy";
        h.notes.push_back("audio: null device requested (SDL dummy driver)");
    } else if (!o.driver.empty()) {
        drivers = o.driver;
        h.notes.push_back("audio: driver forced to " + o.driver);
    } else if (a.pipewire_socket) {
        drivers = "pipewire,pulseaudio,alsa";
    } else if (a.pulse_socket) {
        drivers = "pulseaudio,alsa";
    } else if (a.alsa_devices) {
        drivers = "alsa";
    } else {
        drivers = "dummy";
        h.warnings.push_back(s.flatpak ? "audio: no sound server socket and no /dev/snd (Flatpak needs --socket=pulseaudio)" : "audio: no PipeWire, PulseAudio or ALSA device found, running silent");
    }
    h.hint("SDL_AUDIO_DRIVER", drivers);
    h.notes.push_back("audio driver order: " + drivers);

    if (o.low_latency && drivers != "dummy") {
        const int frames = std::max(64, o.buffer_frames);
        h.hint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", std::to_string(frames));
        // PipeWire reads PIPEWIRE_LATENCY ("frames/rate"); PulseAudio reads PULSE_LATENCY_MSEC.
        h.setenv_("PIPEWIRE_LATENCY", std::to_string(frames) + "/" + std::to_string(o.sample_rate));
        h.setenv_("PULSE_LATENCY_MSEC", std::to_string(std::max(5, frames * 1000 / o.sample_rate * 2)));
        h.notes.push_back("audio latency target: " + std::to_string(frames) + " frames at " + std::to_string(o.sample_rate) + " Hz");
    }
    return h;
}

// ---------------------------------------------------------------------------- video

VideoPlan plan_video(const VideoOptions& o, const SessionInfo& s) {
    VideoPlan p;
    HintSet& h = p.hints;
    if (o.headless) {
        h.hint("SDL_VIDEO_DRIVER", "offscreen");
        h.notes.push_back("video: headless, SDL offscreen driver");
    } else if (!o.driver.empty()) {
        h.hint("SDL_VIDEO_DRIVER", o.driver);
        h.notes.push_back("video: driver forced to " + o.driver);
    } else if (s.headless) {
        h.hint("SDL_VIDEO_DRIVER", "offscreen");
        h.warnings.push_back("video: no DISPLAY or WAYLAND_DISPLAY, falling back to the offscreen driver");
    } else if (s.display_server == DisplayServer::Wayland) {
        h.hint("SDL_VIDEO_DRIVER", "wayland,x11");
    } else {
        h.hint("SDL_VIDEO_DRIVER", "x11,wayland");
    }

    h.hint("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0");  // fullscreen games must survive alt-tab on X11
    h.hint("SDL_VIDEO_ALLOW_SCREENSAVER", "0");

    p.fullscreen = o.requested;
    // Mode switching only works on X11 and is unsafe where a compositor owns the output.
    const bool x11 = s.display_server == DisplayServer::X11;
    p.allow_video_mode_change = x11 && !s.gamescope && !s.steam_deck && o.requested == FullscreenMode::Exclusive;
    if (o.requested == FullscreenMode::Exclusive && !p.allow_video_mode_change) {
        p.fullscreen = FullscreenMode::Borderless;
        h.notes.push_back(s.gamescope ? "fullscreen: exclusive requested, borderless used (gamescope scales the output itself)"
                                      : "fullscreen: exclusive requested, borderless desktop fullscreen used (video mode switching is unsafe here)");
    }
    if (s.gamescope) {
        p.fullscreen = o.requested == FullscreenMode::Windowed ? FullscreenMode::Windowed : FullscreenMode::Borderless;
        p.prefer_desktop_fullscreen = true;
        h.notes.push_back("gamescope: render at the window size it reports, never change the display mode");
    }
    p.confine_cursor = o.grab_cursor_fullscreen && !s.steam_deck;  // the Deck has no mouse by default
    if (s.wayland_session && !s.xwayland) h.notes.push_back("Wayland: cursor confinement uses pointer constraints, mode changes are not possible");
    return p;
}

// ---------------------------------------------------------------------------- input

std::vector<InputDevice> read_input_devices(const IFileSystem& fs) {
    std::vector<InputDevice> out;
    std::string text;
    if (!fs.read_file("/proc/bus/input/devices", text)) return out;
    InputDevice cur;
    bool open = false;
    auto flush = [&] {
        if (!open) return;
        cur.steam_virtual = cur.vendor == 0x28de && cur.product == 0x11ff;
        cur.steam_deck = cur.vendor == 0x28de && cur.product == 0x1205;
        const bool js = cur.handlers.find("js") != std::string::npos;
        cur.is_gamepad = js || cur.steam_virtual || cur.steam_deck;
        out.push_back(std::move(cur));
        cur = InputDevice();
        open = false;
    };
    for (std::string_view line : split_lines(text)) {
        if (line.empty()) { flush(); continue; }
        open = true;
        if (starts_with(line, "I: ")) {
            for (std::string_view tok : split(line.substr(3), ' ')) {
                uint64_t v;
                if (starts_with(tok, "Vendor=") && parse_u64(tok.substr(7), v, 16)) cur.vendor = static_cast<uint16_t>(v);
                else if (starts_with(tok, "Product=") && parse_u64(tok.substr(8), v, 16)) cur.product = static_cast<uint16_t>(v);
            }
        } else if (starts_with(line, "N: Name=")) {
            std::string_view n = line.substr(8);
            if (n.size() >= 2 && n.front() == '"' && n.back() == '"') n = n.substr(1, n.size() - 2);
            cur.name = n;
        } else if (starts_with(line, "H: Handlers=")) {
            cur.handlers = std::string(trim(line.substr(12)));
        }
    }
    flush();
    return out;
}

InputAdvice check_input_setup(const IFileSystem& fs) {
    InputAdvice a;
    const char* const dirs[] = {"/usr/lib/udev/rules.d", "/lib/udev/rules.d", "/etc/udev/rules.d", "/run/udev/rules.d"};
    for (const char* d : dirs) {
        std::vector<DirEntry> ents;
        if (!fs.list_dir(d, ents)) continue;
        for (const auto& e : ents) {
            std::string n = to_lower_ascii(e.name);
            if (n.find("steam") != std::string::npos) a.steam_rules_present = true;
            if (n.find("joystick") != std::string::npos || n == "70-uaccess.rules") a.uaccess_rules_present = true;
        }
    }
    a.uinput_present = fs.exists("/dev/uinput");
    if (!a.uaccess_rules_present) a.notes.push_back("no joystick/uaccess udev rule found: gamepads may be unreadable for non-root users (install the 'steam-devices' package or systemd's 70-joystick.rules)");
    if (!a.steam_rules_present) a.notes.push_back("no Steam udev rules found: Steam Input and some HID controllers need 60-steam-input.rules (package steam-devices)");
    return a;
}

HintSet plan_input(const std::vector<InputDevice>& devs, const SessionInfo& s) {
    HintSet h;
    h.hint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");  // keep controllers alive when gamescope/Steam holds focus
    const bool have_virtual = std::any_of(devs.begin(), devs.end(), [](const InputDevice& d) { return d.steam_virtual; });
    if (s.steam_client && have_virtual) {
        h.notes.push_back("input: Steam Input virtual gamepad present, using it as is (no HIDAPI access to physical pads)");
    } else if (s.steam_deck && !s.steam_client) {
        h.hint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1");
        h.notes.push_back("input: Steam Deck without Steam: reading the built-in controls directly through HIDAPI");
    }
    int pads = static_cast<int>(std::count_if(devs.begin(), devs.end(), [](const InputDevice& d) { return d.is_gamepad; }));
    h.notes.push_back("input: " + std::to_string(pads) + " gamepad device(s) visible");
    return h;
}

}  // namespace nue::lx
