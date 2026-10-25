#include "lx_steamos.hpp"

#include <algorithm>
#include <cmath>

#include "lx_diag.hpp"
#include "lx_text.hpp"

namespace nue::lx {

const char* to_string(SteamHardware h) {
    switch (h) {
        case SteamHardware::DeckLcd: return "Steam Deck LCD";
        case SteamHardware::DeckOled: return "Steam Deck OLED";
        case SteamHardware::SteamMachine: return "Steam Machine";
        case SteamHardware::SteamFrame: return "Steam Frame";
        case SteamHardware::OtherValve: return "Valve hardware";
        default: return "none";
    }
}

const char* to_string(SteamOsFlavor f) {
    switch (f) {
        case SteamOsFlavor::SteamOS: return "SteamOS";
        case SteamOsFlavor::Bazzite: return "Bazzite";
        case SteamOsFlavor::ChimeraOS: return "ChimeraOS";
        case SteamOsFlavor::HoloISO: return "HoloISO";
        case SteamOsFlavor::Nobara: return "Nobara";
        case SteamOsFlavor::SteamFork: return "SteamFork";
        case SteamOsFlavor::Jovian: return "Jovian-NixOS";
        case SteamOsFlavor::UniversalBlue: return "Universal Blue gaming image";
        case SteamOsFlavor::GenericGamingImage: return "SteamOS-like gaming image";
        default: return "none";
    }
}

const char* to_string(SessionMode m) {
    switch (m) {
        case SessionMode::Gaming: return "gaming mode";
        case SessionMode::Desktop: return "desktop mode";
        default: return "unknown";
    }
}

const char* to_string(GlyphFamily g) {
    switch (g) {
        case GlyphFamily::KeyboardMouse: return "keyboard+mouse";
        case GlyphFamily::Xbox: return "xbox";
        case GlyphFamily::PlayStation: return "playstation";
        case GlyphFamily::Nintendo: return "nintendo";
        case GlyphFamily::Steam: return "steam";
        default: return "generic";
    }
}

// ---------------------------------------------------------------------------- detection

namespace {

bool id_has(const OsRelease& os, const char* word) {
    if (os.id == word) return true;
    for (std::string_view like : split(os.id_like, ' '))
        if (like == word) return true;
    return false;
}

const char* compiled_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return "x86_64";
#endif
}

}  // namespace

SteamOsInfo detect_steamos(const IEnv& env, const IFileSystem& fs, const SessionInfo& session) {
    SteamOsInfo s;

    // ---- hardware
    const std::string vendor = fs.read_line("/sys/class/dmi/id/sys_vendor");
    const std::string product = fs.read_line("/sys/class/dmi/id/product_name");
    if (iequals_ascii(vendor, "Valve")) {
        if (product == "Jupiter") { s.hardware = SteamHardware::DeckLcd; s.hardware_reason = "DMI product Jupiter"; }
        else if (product == "Galileo") { s.hardware = SteamHardware::DeckOled; s.hardware_reason = "DMI product Galileo"; }
        else {
            // Steam Machine / Steam Frame have no known DMI name here: infer from the architecture and say so.
            s.hardware_is_heuristic = true;
            if (std::string(compiled_arch()) == "aarch64") { s.hardware = SteamHardware::SteamFrame; s.hardware_reason = "Valve vendor, arm64, unknown product '" + product + "'"; }
            else { s.hardware = SteamHardware::SteamMachine; s.hardware_reason = "Valve vendor, x86-64, unknown product '" + product + "'"; }
        }
    } else if (env.truthy("SteamDeck")) {
        s.hardware = SteamHardware::DeckLcd;
        s.hardware_is_heuristic = true;
        s.hardware_reason = "SteamDeck=1 without DMI data: panel assumed LCD";
    }

    // ---- OS flavour
    std::string text;
    OsRelease os;
    if (fs.read_file("/etc/os-release", text) || fs.read_file("/usr/lib/os-release", text)) os = parse_os_release(text);
    s.distro_id = os.id;
    s.variant_id = os.variant_id;
    s.distro_version = os.version_id;
    s.distro_pretty = os.pretty_name.empty() ? os.name : os.pretty_name;
    if (os.id == "steamos") { s.flavor = SteamOsFlavor::SteamOS; s.flavor_reason = "os-release ID=steamos"; }
    else if (os.id == "bazzite" || id_has(os, "bazzite")) { s.flavor = SteamOsFlavor::Bazzite; s.flavor_reason = "os-release ID=bazzite"; }
    else if (os.id == "chimeraos") { s.flavor = SteamOsFlavor::ChimeraOS; s.flavor_reason = "os-release ID=chimeraos"; }
    else if (os.id == "holoiso") { s.flavor = SteamOsFlavor::HoloISO; s.flavor_reason = "os-release ID=holoiso"; }
    else if (os.id == "steamfork") { s.flavor = SteamOsFlavor::SteamFork; s.flavor_reason = "os-release ID=steamfork"; }
    else if (os.id == "nobara" && (fs.exists("/usr/bin/gamescope-session") || fs.exists("/usr/share/wayland-sessions/gamescope-session-steam.desktop") || contains_ci(os.variant_id, "steam"))) {
        s.flavor = SteamOsFlavor::Nobara;
        s.flavor_reason = "Nobara with a gamescope session";
    } else if (os.id == "nixos" && (fs.exists("/run/current-system/sw/bin/steamos-manager") || fs.exists("/run/current-system/sw/bin/jupiter-biosupdate"))) {
        s.flavor = SteamOsFlavor::Jovian;
        s.flavor_reason = "NixOS with steamos-manager (Jovian-NixOS, heuristic)";
    } else if (fs.exists("/usr/share/ublue-os/image-info.json") && (fs.exists("/usr/bin/gamescope-session") || fs.exists("/usr/bin/steamos-session-select"))) {
        s.flavor = SteamOsFlavor::UniversalBlue;
        s.flavor_reason = "Universal Blue image with a gamescope session";
    } else if (id_has(os, "steamos")) {
        s.flavor = SteamOsFlavor::SteamOS;
        s.flavor_reason = "os-release ID_LIKE contains steamos";
    } else if (fs.exists("/usr/bin/steamos-session-select") || fs.exists("/usr/bin/gamescope-session") || fs.exists("/usr/share/wayland-sessions/gamescope-session.desktop")) {
        s.flavor = SteamOsFlavor::GenericGamingImage;
        s.flavor_reason = "gamescope session binaries present (heuristic)";
    }

    // ---- immutable root
    std::string mi;
    if (fs.exists("/run/ostree-booted")) {
        s.immutable_root = true;
        s.immutable_reason = "ostree deployment (/run/ostree-booted)";
    }
    if (fs.read_file("/proc/self/mountinfo", mi)) {
        auto mounts = parse_mountinfo(mi);
        if (const MountEntry* root = mount_for_path(mounts, "/")) {
            if (root->read_only && !s.immutable_root) {
                s.immutable_root = true;
                s.immutable_reason = "root file system mounted read-only";
            }
        }
        for (const auto& m : mounts)
            if (starts_with(m.mount_point, "/run/media/") && m.mount_point.size() > 11) s.sd_cards.push_back(m.mount_point);
    }
    if (s.flavor == SteamOsFlavor::SteamOS && !s.immutable_root) s.notes.push_back("SteamOS root is writable now (steamos-readonly disabled?): installs outside $HOME are wiped by OS updates");

    // ---- session mode
    if (env.truthy("SteamGamepadUI")) { s.mode = SessionMode::Gaming; s.mode_reason = "SteamGamepadUI=1"; }
    else if (env.is_set("GAMESCOPE_WAYLAND_DISPLAY")) { s.mode = SessionMode::Gaming; s.mode_reason = "GAMESCOPE_WAYLAND_DISPLAY"; }
    else if (session.gamescope) { s.mode = SessionMode::Gaming; s.mode_reason = session.gamescope_reason; }
    else if (!session.desktop.empty() || !session.session_type.empty()) {
        s.mode = SessionMode::Desktop;
        s.mode_reason = "desktop environment " + (session.desktop.empty() ? session.session_type : session.desktop);
    }

    // ---- Steam client launch environment
    std::string gid = env.get_or("SteamGameId");
    if (gid.empty()) gid = env.get_or("SteamAppId");
    uint64_t id = 0;
    if (!gid.empty() && parse_u64(gid, id)) {
        s.launched_by_steam = true;
        s.game_id = id;
        s.non_steam_shortcut = id > 0xFFFFFFFFull || (id & 0x80000000ull) != 0;
    } else if (env.is_set("SteamGameId") || env.is_set("SteamAppId") || env.is_set("STEAM_COMPAT_CLIENT_INSTALL_PATH")) {
        s.launched_by_steam = true;
    }
    s.compat_data_path = env.get_or("STEAM_COMPAT_DATA_PATH");
    s.proton = !s.compat_data_path.empty();
    s.runtime_container = session.steam_runtime;

    const std::string home = env.get_or("HOME");
    std::string root = env.get_or("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    if (root.empty() && !home.empty()) {
        const std::string xdg_data = env.get_or("XDG_DATA_HOME", home + "/.local/share");
        const std::string candidates[] = {xdg_data + "/Steam", home + "/.steam/steam", home + "/.local/share/Steam"};
        for (const auto& c : candidates)
            if (fs.is_dir(c)) { root = c; break; }
        const std::string fp = home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam";
        if (root.empty() && fs.is_dir(fp)) { root = fp; s.steam_in_flatpak = true; }
    }
    if (!s.steam_in_flatpak && root.find("/.var/app/com.valvesoftware.Steam/") != std::string::npos) s.steam_in_flatpak = true;
    s.steam_root = root;

    if (s.hardware_is_heuristic && s.hardware != SteamHardware::None) s.notes.push_back("hardware model is inferred: " + s.hardware_reason);
    if (s.launched_by_steam && s.non_steam_shortcut) s.notes.push_back("started from a Non-Steam shortcut");
    if (s.proton) s.notes.push_back("STEAM_COMPAT_DATA_PATH is set: this process runs under Proton/compat tools, not as a native Linux game");
    return s;
}

// ---------------------------------------------------------------------------- VDF

const VdfNode* VdfNode::find(const std::string& k) const {
    for (const auto& c : children)
        if (iequals_ascii(c.key, k)) return &c;
    return nullptr;
}

std::string VdfNode::get(const std::string& k, const std::string& fallback) const {
    const VdfNode* n = find(k);
    return n && !n->is_object ? n->value : fallback;
}

namespace {

struct VdfReader {
    const std::string& s;
    size_t i = 0;
    explicit VdfReader(const std::string& text) : s(text) {}

    void skip() {
        for (;;) {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n') ++i;
                continue;
            }
            break;
        }
    }
    bool string(std::string& out) {
        skip();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                ++i;
                switch (s[i]) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    default: out += s[i]; break;  // \\ \" and unknown escapes keep the character
                }
            } else {
                out += s[i];
            }
            ++i;
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool object(VdfNode& parent, int depth) {
        if (depth > 32) return false;
        for (;;) {
            skip();
            if (i >= s.size()) return depth == 0;  // only the top level may end without a brace
            if (s[i] == '}') {
                if (depth == 0) return false;
                ++i;
                return true;
            }
            VdfNode n;
            if (!string(n.key)) return false;
            skip();
            if (i < s.size() && s[i] == '{') {
                ++i;
                n.is_object = true;
                if (!object(n, depth + 1)) return false;
            } else if (!string(n.value)) {
                return false;
            }
            parent.children.push_back(std::move(n));
        }
    }
};

}  // namespace

bool parse_vdf(const std::string& text, VdfNode& root) {
    root = VdfNode();
    root.is_object = true;
    VdfReader r(text);
    return r.object(root, 0);
}

std::vector<std::string> steam_library_paths(const IFileSystem& fs, const std::string& steam_root) {
    std::vector<std::string> out;
    auto add = [&](std::string p) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (!p.empty() && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(std::move(p));
    };
    if (steam_root.empty()) return out;
    add(steam_root);
    std::string text;
    VdfNode root;
    if (!fs.read_file(steam_root + "/steamapps/libraryfolders.vdf", text, 1 << 20) || !parse_vdf(text, root)) return out;
    const VdfNode* folders = root.find("libraryfolders");
    if (!folders) return out;
    for (const auto& entry : folders->children) {
        if (entry.is_object) add(entry.get("path"));  // current format: "0" { "path" "..." ... }
        else add(entry.value);                         // old format: "1" "/path"
    }
    return out;
}

std::string find_app_install(const IFileSystem& fs, const std::vector<std::string>& libraries, uint64_t app_id) {
    for (const auto& lib : libraries) {
        std::string text;
        VdfNode root;
        if (!fs.read_file(lib + "/steamapps/appmanifest_" + std::to_string(app_id) + ".acf", text, 1 << 20) || !parse_vdf(text, root)) continue;
        const VdfNode* state = root.find("AppState");
        if (!state) continue;
        const std::string dir = state->get("installdir");
        if (dir.empty() || dir.find("..") != std::string::npos) continue;
        const std::string full = lib + "/steamapps/common/" + dir;
        if (fs.is_dir(full)) return full;
    }
    return std::string();
}

// ---------------------------------------------------------------------------- display

PanelInfo panel_for(SteamHardware hw) {
    PanelInfo p;
    if (hw == SteamHardware::DeckLcd) {
        p.known = true;
        p.width = 1280; p.height = 800;
        p.min_hz = 40; p.max_hz = 60;
        p.fps_limits = {15, 30, 40, 45, 60};
    } else if (hw == SteamHardware::DeckOled) {
        p.known = true;
        p.width = 1280; p.height = 800;
        p.min_hz = 45; p.max_hz = 90;
        p.fps_limits = {15, 30, 40, 45, 60, 90};
        p.oled = true;
        p.vrr = true;
        p.hdr = true;
    }
    return p;
}

FramePacing plan_frame_pacing(const SteamOsInfo& s, const SessionInfo& session, const FramePacingRequest& r) {
    FramePacing p;
    p.target_fps = std::max(1, r.game_fps);
    const bool gs = session.gamescope || s.gaming_mode();
    p.present_vsync = gs ? true : r.vsync_wanted;  // under gamescope tearing cannot happen: present FIFO and let it drive the panel
    const PanelInfo panel = panel_for(s.hardware);

    if (r.display_hz <= 0) {
        p.own_limiter = true;
        p.notes.push_back("refresh rate unknown: pacing with the engine limiter at the target and presenting with vsync");
        return p;
    }
    const double ratio = r.display_hz / p.target_fps;
    const double nearest = std::round(ratio);
    const bool integer_ratio = nearest >= 1.0 && std::fabs(ratio - nearest) < 0.02;
    if (integer_ratio && nearest == 1.0) {
        p.own_limiter = false;
        p.notes.push_back("display refresh matches the target: vsync alone paces the game");
    } else if (integer_ratio) {
        p.own_limiter = true;  // FIFO would present at the refresh rate: sleep to the lower target
        p.notes.push_back("target divides the refresh rate evenly: the engine limiter holds " + std::to_string(p.target_fps) + " fps");
    } else if (r.display_hz < p.target_fps) {
        p.own_limiter = false;
        p.notes.push_back("display runs slower than the game's rate: a fixed-rate game will run slow");
        p.suggested_steam_limit = p.target_fps;
    } else {
        p.own_limiter = true;
        p.judder_expected = true;
        p.notes.push_back("refresh is not a multiple of the target: frame pacing will judder");
        // choose a Steam frame limit equal to the target (gamescope then switches the panel to that rate)
        for (int lim : panel.fps_limits)
            if (lim == p.target_fps) p.suggested_steam_limit = lim;
        if (!p.suggested_steam_limit) {
            for (int lim : panel.fps_limits)
                if (lim % p.target_fps == 0) { p.suggested_steam_limit = lim; break; }
        }
    }
    if (p.suggested_steam_limit)
        p.notes.push_back("suggest the Steam per-game frame limit " + std::to_string(p.suggested_steam_limit) + " (quick access menu > performance)");
    return p;
}

DisplayPlan plan_display(const SteamOsInfo& s, const SessionInfo& session) {
    DisplayPlan d;
    const PanelInfo panel = panel_for(s.hardware);
    d.native_w = panel.width;
    d.native_h = panel.height;
    d.fullscreen_borderless = true;
    d.allow_internal_scaling = session.gamescope || s.gaming_mode();
    d.hdr_available = panel.hdr && (session.gamescope || s.gaming_mode());
    if (panel.known) d.notes.push_back("panel " + std::to_string(panel.width) + "x" + std::to_string(panel.height) + ", refresh " + std::to_string(panel.min_hz) + "-" + std::to_string(panel.max_hz) + " Hz");
    if (session.gamescope) d.notes.push_back("gamescope composes the output: borderless fullscreen at the size it reports, never a mode switch; scaling filter is chosen in the Steam UI");
    if (d.hdr_available) d.notes.push_back("HDR is available through gamescope (Vulkan HDR10 swapchain); the engine renderer is SDR and does not use it");
    if (!panel.known && s.steam_hardware()) d.notes.push_back("panel of this Valve device is not in the table: use the window size reported by the compositor");
    return d;
}

// ---------------------------------------------------------------------------- controllers

GlyphFamily glyph_family_for(uint16_t vendor, uint16_t product, bool steam_input_active, bool on_steam_deck) {
    (void)product;
    switch (vendor) {
        case 0x28de: return GlyphFamily::Steam;       // Valve: Steam Deck / Steam Controller / virtual pad
        case 0x045e: return GlyphFamily::Xbox;
        case 0x054c: return GlyphFamily::PlayStation;
        case 0x057e: return GlyphFamily::Nintendo;
        default: break;
    }
    if (steam_input_active && on_steam_deck) return GlyphFamily::Steam;
    return GlyphFamily::Generic;
}

bool parse_ignore_devices(const std::string& text, std::vector<std::pair<uint16_t, uint16_t>>& out) {
    out.clear();
    for (std::string_view tok : split(text, ',')) {
        tok = trim(tok);
        if (tok.empty()) continue;
        size_t slash = tok.find('/');
        uint64_t v, p;
        if (slash == std::string_view::npos || !parse_u64(tok.substr(0, slash), v, 16) || !parse_u64(tok.substr(slash + 1), p, 16) || v > 0xFFFF || p > 0xFFFF) {
            out.clear();
            return false;
        }
        out.emplace_back(static_cast<uint16_t>(v), static_cast<uint16_t>(p));
    }
    return true;
}

SteamInputPlan plan_steam_input(const SteamOsInfo& s, const SessionInfo& session, const IEnv& env, const std::vector<InputDevice>& devs) {
    (void)session;
    SteamInputPlan p;
    std::vector<std::pair<uint16_t, uint16_t>> ignore, except;
    parse_ignore_devices(env.get_or("SDL_GAMECONTROLLER_IGNORE_DEVICES"), ignore);
    parse_ignore_devices(env.get_or("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"), except);
    p.physical_hidden = !ignore.empty() || !except.empty();
    auto in_list = [](const std::vector<std::pair<uint16_t, uint16_t>>& l, const InputDevice& d) {
        return std::any_of(l.begin(), l.end(), [&](const auto& e) { return e.first == d.vendor && e.second == d.product; });
    };
    bool virtual_pad = false, deck_pad = false;
    const InputDevice* first_visible = nullptr;
    for (const auto& d : devs) {
        if (d.steam_virtual) virtual_pad = true;
        if (d.steam_deck) deck_pad = true;
        if (!d.is_gamepad) continue;
        bool hidden = in_list(ignore, d) || (!except.empty() && !in_list(except, d));
        if (hidden) continue;
        ++p.visible_gamepads;
        if (!first_visible) first_visible = &d;
    }
    p.active = s.launched_by_steam && virtual_pad;
    const bool deck = s.hardware == SteamHardware::DeckLcd || s.hardware == SteamHardware::DeckOled;
    p.raw_deck_access = deck && !s.launched_by_steam && deck_pad;
    p.gyro_via_sdl = p.raw_deck_access;
    p.back_buttons_via_sdl = p.raw_deck_access;
    if (deck) p.default_glyphs = GlyphFamily::Steam;
    else if (first_visible) p.default_glyphs = glyph_family_for(first_visible->vendor, first_visible->product, p.active, false);

    if (p.active) {
        p.advice.push_back("Steam Input is active: use the virtual gamepad only; gyro, trackpads and back buttons reach the game only through a Steam Input template (the default 'Gamepad' layout maps the pad, sticks and triggers)");
        if (!p.physical_hidden) p.advice.push_back("SDL_GAMECONTROLLER_IGNORE_DEVICES is not set: physical pads are not hidden, the game may see each controller twice (open only the virtual pad)");
    } else if (s.launched_by_steam) {
        p.advice.push_back("launched by Steam but no virtual gamepad is visible: Steam Input may be disabled for this game, controllers arrive raw");
    }
    if (p.raw_deck_access) p.advice.push_back("not under Steam: the Deck controller is read directly (HIDAPI); gyro and the four back buttons are available through SDL");
    if (s.non_steam_shortcut) p.advice.push_back("Non-Steam shortcut: Steam applies its default 'Gamepad' template; ship a controller configuration for anything more");
    return p;
}

KeyboardRequest plan_onscreen_keyboard(const SteamOsInfo& s, const IFileSystem& fs, const IEnv& env) {
    KeyboardRequest k;
    auto find_in_path = [&](const std::string& exe) -> std::string {
        const std::string path_var = env.get_or("PATH", "/usr/bin:/usr/local/bin:/usr/games");  // split() returns views into it
        for (std::string_view dir : split(path_var, ':')) {
            std::string c = std::string(dir) + "/" + exe;
            if (fs.exists(c)) return c;
        }
        return std::string();
    };
    const std::string url = "steam://open/keyboard";
    if (s.steam_in_flatpak) {
        std::string fp = find_in_path("flatpak");
        if (!fp.empty()) {
            k.argv = {fp, "run", "com.valvesoftware.Steam", "-ifrunning", url};
            k.reason = "Steam runs as a Flatpak";
            return k;
        }
    }
    std::string steam = find_in_path("steam");
    if (!steam.empty()) {
        k.argv = {steam, "-ifrunning", url};  // asks the running client; starts nothing if Steam is not running
        k.reason = "Steam client binary";
        return k;
    }
    std::string xdg = find_in_path("xdg-open");
    if (!xdg.empty()) {
        k.argv = {xdg, url};
        k.reason = "xdg-open handles the steam:// scheme";
        return k;
    }
    k.reason = "no steam or xdg-open binary found: use the Steamworks floating keyboard when linked, or the OS keyboard";
    return k;
}

// ---------------------------------------------------------------------------- power

namespace {
bool read_int(const IFileSystem& fs, const std::string& path, int64_t& v) { return parse_i64(fs.read_line(path), v); }

int active_mhz(const IFileSystem& fs, const std::string& path) {
    std::string text;
    if (!fs.read_file(path, text, 4096)) return -1;
    for (std::string_view line : split_lines(text)) {
        if (line.find('*') == std::string_view::npos) continue;
        size_t colon = line.find(':');
        std::string_view rest = trim(colon == std::string_view::npos ? line : line.substr(colon + 1));
        size_t m = 0;
        while (m < rest.size() && rest[m] >= '0' && rest[m] <= '9') ++m;
        uint64_t v;
        if (m && parse_u64(rest.substr(0, m), v)) return static_cast<int>(v);
    }
    return -1;
}
}  // namespace

AmdGpuStatus read_amdgpu_status(const IFileSystem& fs) {
    AmdGpuStatus g;
    std::vector<DirEntry> cards;
    if (!fs.list_dir("/sys/class/drm", cards)) return g;
    std::sort(cards.begin(), cards.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    for (const auto& c : cards) {
        if (!starts_with(c.name, "card") || c.name.find('-') != std::string::npos) continue;
        const std::string dev = "/sys/class/drm/" + c.name + "/device/";
        uint64_t vendor;
        if (!parse_u64(fs.read_line(dev + "vendor"), vendor, 16) || vendor != 0x1002) continue;
        g.present = true;
        g.card = c.name;
        int64_t v;
        if (read_int(fs, dev + "gpu_busy_percent", v)) g.busy_percent = static_cast<int>(v);
        g.perf_level = fs.read_line(dev + "power_dpm_force_performance_level");
        g.sclk_mhz = active_mhz(fs, dev + "pp_dpm_sclk");
        g.mclk_mhz = active_mhz(fs, dev + "pp_dpm_mclk");
        std::vector<DirEntry> hw;
        if (fs.list_dir(dev + "hwmon", hw)) {
            std::sort(hw.begin(), hw.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
            for (const auto& h : hw) {
                const std::string base = dev + "hwmon/" + h.name + "/";
                if (read_int(fs, base + "power1_cap", v) && v > 0) g.power_cap_w = static_cast<double>(v) / 1e6;
                if (read_int(fs, base + "power1_average", v) && v >= 0) g.power_now_w = static_cast<double>(v) / 1e6;
                if (read_int(fs, base + "temp1_input", v)) g.temp_c = static_cast<int>(v / 1000);
            }
        }
        break;  // the first AMD GPU is the APU on every Valve device
    }
    return g;
}

int suggest_fps_cap(const SteamOsInfo& s, const PowerInfo& power, bool thermal_serious, int wanted) {
    PanelInfo panel = panel_for(s.hardware);
    if (panel.fps_limits.empty() || wanted <= 0) return wanted;
    int steps = 0;
    const int pct = power.best_percent();
    if (power.on_battery && pct >= 0) {
        if (pct <= 10) steps += 2;
        else if (pct <= 20) steps += 1;
    }
    if (thermal_serious) steps += 1;
    std::vector<int> lims = panel.fps_limits;
    std::sort(lims.begin(), lims.end(), [](int a, int b) { return a > b; });
    int cap = wanted;
    for (int i = 0; i < steps; ++i) {
        int next = 0;
        for (int l : lims)
            if (l < cap && l >= 30) { next = l; break; }
        if (!next) break;
        cap = next;
    }
    return cap;
}

std::vector<DBusInterfaceInfo> parse_introspection_xml(const std::string& xml) {
    std::vector<DBusInterfaceInfo> out;
    auto attr = [](std::string_view tag, const char* name) -> std::string {
        std::string key = std::string(name) + "=\"";
        size_t p = tag.find(key);
        if (p == std::string_view::npos) return std::string();
        p += key.size();
        size_t e = tag.find('"', p);
        return e == std::string_view::npos ? std::string() : std::string(tag.substr(p, e - p));
    };
    size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string_view tag(xml.data() + pos + 1, end - pos - 1);
        if (starts_with(tag, "interface ")) {
            DBusInterfaceInfo i;
            i.name = attr(tag, "name");
            if (!i.name.empty()) out.push_back(std::move(i));
        } else if (starts_with(tag, "property ") && !out.empty()) {
            out.back().properties.emplace_back(attr(tag, "name"), attr(tag, "type"));
        } else if (starts_with(tag, "/interface")) {
            // properties belong to the interface being closed; nothing to do, order is kept by out.back()
        }
        pos = end + 1;
    }
    return out;
}

bool is_steamos_manager_property_of_interest(const std::string& name) {
    for (const char* w : {"TdpLimit", "GpuPerformanceLevel", "ChargeLevel", "CpuScaling", "FanControl", "ManualGpuClock", "WifiPowerManagement"})
        if (name.find(w) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------- storage

SteamOsPaths plan_steamos_paths(const SteamOsInfo& s, const XdgDirs& xdg, const IEnv& env, const std::string& org, const std::string& app) {
    SteamOsPaths p;
    auto join = [&](const std::string& base) { return base.empty() ? std::string() : base + "/" + org + "/" + app; };
    p.user_data = join(xdg.data_home);
    p.cache = join(xdg.cache_home);
    p.settings = join(xdg.config_home);
    p.install_must_stay_in_home = s.immutable_root;
    const std::string mesa = env.get_or("MESA_SHADER_CACHE_DIR");
    if (s.launched_by_steam && !mesa.empty() && mesa[0] == '/') {
        p.steam_shader_dir = mesa;
        p.shader_cache = mesa + "/nue";  // inside the directory Steam manages (cleared with the game, shown in storage settings)
        p.notes.push_back("shader cache goes under Steam's per-app directory " + mesa);
    } else if (!p.cache.empty()) {
        p.shader_cache = p.cache + "/shaders";
        p.notes.push_back("shader cache in $XDG_CACHE_HOME (internal storage, survives OS updates)");
    }
    if (s.immutable_root) p.notes.push_back("immutable root: install the game under $HOME (~/.local/share/" + org + "/" + app + ") or a Steam library, never /opt or /usr");
    if (!s.sd_cards.empty()) p.notes.push_back("removable media mounted: " + s.sd_cards[0] + (s.sd_cards.size() > 1 ? " (+" + std::to_string(s.sd_cards.size() - 1) + " more)" : "") + "; keep saves on internal storage so removing the card never loses them");
    if (s.proton) p.notes.push_back("compatdata exists for Proton titles; native saves do not go there");
    return p;
}

}  // namespace nue::lx
