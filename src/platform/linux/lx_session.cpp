#include "lx_session.hpp"

#include "lx_sysinfo.hpp"
#include "lx_text.hpp"

namespace nue::lx {

const char* to_string(DisplayServer d) {
    switch (d) {
        case DisplayServer::X11: return "x11";
        case DisplayServer::Wayland: return "wayland";
        default: return "none";
    }
}

SessionInfo detect_session(const IEnv& env, const IFileSystem& fs) {
    SessionInfo s;
    s.desktop = to_lower_ascii(env.get_or("XDG_CURRENT_DESKTOP"));
    s.session_type = to_lower_ascii(env.get_or("XDG_SESSION_TYPE"));
    const bool have_wayland = !env.get_or("WAYLAND_DISPLAY").empty();
    const bool have_x11 = !env.get_or("DISPLAY").empty();
    s.headless = !have_wayland && !have_x11;
    s.wayland_session = s.session_type == "wayland" || have_wayland;
    s.xwayland = s.wayland_session && have_x11;
    // SDL3 prefers Wayland when WAYLAND_DISPLAY is usable and SDL_VIDEO_DRIVER does not say otherwise.
    std::string forced = to_lower_ascii(env.get_or("SDL_VIDEO_DRIVER"));
    if (!forced.empty()) {
        s.display_server = forced == "wayland" ? DisplayServer::Wayland : forced == "x11" ? DisplayServer::X11 : DisplayServer::None;
    } else if (have_wayland) {
        s.display_server = DisplayServer::Wayland;
    } else if (have_x11) {
        s.display_server = DisplayServer::X11;
    }

    std::string text;
    if (fs.read_file("/etc/os-release", text) || fs.read_file("/usr/lib/os-release", text)) {
        OsRelease os = parse_os_release(text);
        s.distro_id = os.id;
        s.steamos = os.id == "steamos";
        if (os.id == "steamrt") {
            s.steam_runtime = true;
            s.steam_runtime_name = os.version_id.empty() ? os.name : os.version_id;
            // steamrt os-release: VERSION_CODENAME=sniper
            for (std::string_view line : split_lines(text)) {
                if (starts_with(line, "VERSION_CODENAME=")) s.steam_runtime_name = std::string(trim(line.substr(17)));
            }
        }
    }
    if (!s.steam_runtime && (fs.exists("/run/pressure-vessel") || env.is_set("PRESSURE_VESSEL_RUNTIME"))) {
        s.steam_runtime = true;
        s.notes.push_back("pressure-vessel container detected");
    }
    if (s.steam_runtime && s.steam_runtime_name.empty()) s.steam_runtime_name = to_lower_ascii(env.get_or("STEAM_RUNTIME_NAME"));

    // Valve hardware.
    const std::string vendor = fs.read_line("/sys/class/dmi/id/sys_vendor");
    const std::string product = fs.read_line("/sys/class/dmi/id/product_name");
    if (iequals_ascii(vendor, "Valve")) {
        if (product == "Jupiter") { s.steam_deck = true; s.deck_model = "lcd"; }
        else if (product == "Galileo") { s.steam_deck = true; s.deck_model = "oled"; }
    }
    if (!s.steam_deck && env.truthy("SteamDeck")) {
        s.steam_deck = true;
        s.notes.push_back("SteamDeck=1 in the environment (Steam sets it on Deck hardware)");
    }

    // gamescope: no single reliable marker, so combine several weak ones and record why.
    if (env.is_set("GAMESCOPE_WAYLAND_DISPLAY")) {
        s.gamescope = true;
        s.gamescope_reason = "GAMESCOPE_WAYLAND_DISPLAY";
    } else if (contains_ci(s.desktop, "gamescope") || contains_ci(env.get_or("XDG_SESSION_DESKTOP"), "gamescope")) {
        s.gamescope = true;
        s.gamescope_reason = "desktop name contains gamescope";
    } else if (env.truthy("SteamGamepadUI")) {
        s.gamescope = true;
        s.gamescope_reason = "SteamGamepadUI=1 (Gaming Mode)";
    }

    s.steam_client = env.is_set("SteamAppId") || env.is_set("SteamGameId") || env.is_set("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    s.steam_overlay = contains_ci(env.get_or("LD_PRELOAD"), "gameoverlayrenderer");

    s.flatpak_id = env.get_or("FLATPAK_ID");
    s.flatpak = fs.exists("/.flatpak-info") || !s.flatpak_id.empty();
    s.snap = env.is_set("SNAP") && env.is_set("SNAP_NAME");
    s.appimage = env.is_set("APPIMAGE");
    s.container = !s.flatpak && !s.steam_runtime && (fs.exists("/.dockerenv") || fs.exists("/run/.containerenv") || env.is_set("container"));
    std::string version;
    fs.read_file("/proc/version", version, 4096);
    s.wsl = contains_ci(version, "microsoft") || env.is_set("WSL_DISTRO_NAME");
    s.wslg = s.wsl && fs.exists("/mnt/wslg");
    s.ssh = env.is_set("SSH_CONNECTION") || env.is_set("SSH_TTY");

    if (s.steam_deck) s.notes.push_back(std::string("Steam Deck ") + (s.deck_model.empty() ? "" : s.deck_model));
    if (s.gamescope) s.notes.push_back("gamescope: " + s.gamescope_reason);
    if (s.flatpak) s.notes.push_back("Flatpak sandbox" + (s.flatpak_id.empty() ? std::string() : " (" + s.flatpak_id + ")"));
    if (s.snap) s.notes.push_back("Snap confinement");
    if (s.appimage) s.notes.push_back("AppImage");
    if (s.headless) s.notes.push_back("no DISPLAY and no WAYLAND_DISPLAY: headless (dummy video driver)");
    return s;
}

}  // namespace nue::lx
