// What kind of Linux environment is the game running in? (desktop session type, Steam Deck /
// Gaming Mode / gamescope, Steam Runtime container, Flatpak, Snap, AppImage, WSL...)
#pragma once
#include <string>
#include <vector>

#include "lx_fs.hpp"

namespace nue::lx {

enum class DisplayServer { None, X11, Wayland };

const char* to_string(DisplayServer d);

struct SessionInfo {
    DisplayServer display_server = DisplayServer::None;  // best guess of the server SDL would pick
    bool wayland_session = false;   // the user session is Wayland (a game may still run on XWayland)
    bool xwayland = false;          // Wayland session and DISPLAY is set
    std::string desktop;            // XDG_CURRENT_DESKTOP (lower-case)
    std::string session_type;       // XDG_SESSION_TYPE (lower-case)

    bool steam_deck = false;        // Valve hardware: Jupiter (LCD) / Galileo (OLED)
    std::string deck_model;         // "lcd", "oled" or ""
    bool steamos = false;           // /etc/os-release ID=steamos
    bool gamescope = false;         // running inside a gamescope session (Gaming Mode or nested)
    std::string gamescope_reason;
    bool steam_client = false;      // launched by Steam (SteamAppId / SteamGameId present)
    bool steam_overlay = false;     // gameoverlayrenderer in LD_PRELOAD
    bool steam_runtime = false;     // inside a Steam Linux Runtime container (soldier / sniper / ...)
    std::string steam_runtime_name; // "sniper", "soldier", ... when known

    bool flatpak = false;
    std::string flatpak_id;
    bool snap = false;
    bool appimage = false;
    bool container = false;         // docker/podman (not Flatpak / pressure-vessel)
    bool wsl = false;
    bool wslg = false;
    bool ssh = false;
    bool headless = false;          // no DISPLAY and no WAYLAND_DISPLAY

    std::string distro_id;          // os-release ID
    std::vector<std::string> notes; // human readable decisions for the log
};

SessionInfo detect_session(const IEnv& env, const IFileSystem& fs);

}  // namespace nue::lx
