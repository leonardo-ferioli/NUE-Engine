// Native SteamOS support: what hardware and OS flavour this is, whether the game runs in Gaming Mode
// (gamescope session) or Desktop Mode, how it was launched by Steam, where Steam libraries and SD cards
// are, what the Deck panel allows (resolution, refresh range, frame-limit steps), how to pace frames
// next to gamescope, how Steam Input changes controller handling, and the power / amdgpu state of a Deck.
// All of it is pure logic over IFileSystem / IEnv: it runs on a captured Deck tree and in unit tests.
//
// Facts that could not be verified on real hardware are marked "believed" in the comments; the
// Steam Deck panel numbers come from Valve's public specifications and observed Steam UI behaviour.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "lx_fs.hpp"
#include "lx_policy.hpp"
#include "lx_session.hpp"
#include "lx_sysinfo.hpp"

namespace nue::lx {

// ---------------------------------------------------------------------------- detection

enum class SteamHardware { None, DeckLcd, DeckOled, SteamMachine, SteamFrame, OtherValve };
const char* to_string(SteamHardware h);

enum class SteamOsFlavor { None, SteamOS, Bazzite, ChimeraOS, HoloISO, Nobara, SteamFork, Jovian, UniversalBlue, GenericGamingImage };
const char* to_string(SteamOsFlavor f);

enum class SessionMode { Unknown, Gaming, Desktop };
const char* to_string(SessionMode m);

struct SteamOsInfo {
    SteamHardware hardware = SteamHardware::None;
    std::string hardware_reason;     // how it was decided (DMI, device tree, environment, heuristic)
    bool hardware_is_heuristic = false;  // Steam Machine / Steam Frame are inferred, not matched to a known product name

    SteamOsFlavor flavor = SteamOsFlavor::None;  // None = not a SteamOS-like system
    std::string distro_id, variant_id, distro_version, distro_pretty, flavor_reason;
    bool immutable_root = false;     // read-only or ostree root: nothing may be installed outside $HOME / the library
    std::string immutable_reason;

    SessionMode mode = SessionMode::Unknown;
    std::string mode_reason;

    // Steam client environment
    bool launched_by_steam = false;
    uint64_t game_id = 0;            // SteamGameId (or SteamAppId)
    bool non_steam_shortcut = false; // "Add a Non-Steam Game": the id has the shortcut marker bits
    bool proton = false;             // STEAM_COMPAT_DATA_PATH set: a Windows build under Proton (this layer is the native one)
    std::string compat_data_path;
    std::string steam_root;          // Steam client install directory, "" if not found
    bool steam_in_flatpak = false;
    bool runtime_container = false;  // pressure-vessel / Steam Linux Runtime
    std::vector<std::string> sd_cards;  // mount points under /run/media (removable media)
    std::vector<std::string> notes;

    bool steam_hardware() const { return hardware != SteamHardware::None; }
    bool steamos_like() const { return flavor != SteamOsFlavor::None; }
    bool gaming_mode() const { return mode == SessionMode::Gaming; }
};

SteamOsInfo detect_steamos(const IEnv& env, const IFileSystem& fs, const SessionInfo& session);

// ---------------------------------------------------------------------------- Steam libraries (VDF)

struct VdfNode {
    std::string key;
    std::string value;               // for leaf nodes
    std::vector<VdfNode> children;   // for objects
    bool is_object = false;
    const VdfNode* find(const std::string& k) const;           // direct child, case-insensitive key
    std::string get(const std::string& k, const std::string& fallback = std::string()) const;
};
// Valve KeyValues text format ("key" "value" and "key" { ... }). Returns the root pseudo-node holding the top-level entries.
bool parse_vdf(const std::string& text, VdfNode& root);

// <steam_root>/steamapps/libraryfolders.vdf, always including the root library itself.
std::vector<std::string> steam_library_paths(const IFileSystem& fs, const std::string& steam_root);
// Install directory of `app_id` (appmanifest_<id>.acf installdir) inside the libraries; "" if not installed.
std::string find_app_install(const IFileSystem& fs, const std::vector<std::string>& libraries, uint64_t app_id);

// ---------------------------------------------------------------------------- display (Deck panels, gamescope)

struct PanelInfo {
    bool known = false;
    int width = 0, height = 0;
    int min_hz = 0, max_hz = 0;      // refresh range gamescope can drive (believed: LCD 40-60, OLED 45-90)
    std::vector<int> fps_limits;     // steps of the per-game frame limit in the Steam quick access menu (believed)
    bool oled = false, vrr = false, hdr = false;
};
PanelInfo panel_for(SteamHardware hw);

struct FramePacingRequest {
    int game_fps = 60;               // the game's own simulation / presentation rate (NFSU2: 60)
    double display_hz = 0;           // current refresh reported by the window; 0 = unknown
    bool vsync_wanted = true;
    bool fixed_rate_game = true;     // the game cannot run at arbitrary rates (logic tied to 60 Hz)
};
struct FramePacing {
    int target_fps = 60;
    bool present_vsync = true;       // FIFO present: gamescope owns the real refresh
    bool own_limiter = false;        // the engine sleeps to target_fps (nue_sleep_until) instead of relying on vsync
    bool judder_expected = false;    // target is not a divisor/equal of the refresh
    int suggested_steam_limit = 0;   // frame limit the player should select in the Steam UI (0 = none needed)
    std::vector<std::string> notes;
};
FramePacing plan_frame_pacing(const SteamOsInfo& s, const SessionInfo& session, const FramePacingRequest& r);

struct DisplayPlan {
    int native_w = 0, native_h = 0;
    bool fullscreen_borderless = true;   // the compositor owns the output
    bool allow_internal_scaling = true;  // render below native size: gamescope upscales (FSR / NIS / integer / linear chosen in the Steam UI)
    bool hdr_available = false;          // panel + gamescope can do it; the engine's renderer is SDR today
    bool use_pixel_density = true;
    std::vector<std::string> notes;
};
DisplayPlan plan_display(const SteamOsInfo& s, const SessionInfo& session);

// ---------------------------------------------------------------------------- controllers

enum class GlyphFamily { KeyboardMouse, Xbox, PlayStation, Nintendo, Steam, Generic };
const char* to_string(GlyphFamily g);
GlyphFamily glyph_family_for(uint16_t vendor, uint16_t product, bool steam_input_active, bool on_steam_deck);

// SDL_GAMECONTROLLER_IGNORE_DEVICES-style list ("0x28de/0x1205,0x054c/0x0ce6"): Steam sets it so games see only
// the virtual pads. Returns false for text that is not a list.
bool parse_ignore_devices(const std::string& text, std::vector<std::pair<uint16_t, uint16_t>>& out);

struct SteamInputPlan {
    bool active = false;                 // a Steam virtual pad exists and Steam launched us
    bool physical_hidden = false;        // SDL_GAMECONTROLLER_IGNORE_DEVICES hides physical pads
    int visible_gamepads = 0;
    bool raw_deck_access = false;        // no Steam: the Deck pad is read directly through HIDAPI
    bool gyro_via_sdl = false;           // gyro readable through SDL sensors (only without Steam Input)
    bool back_buttons_via_sdl = false;   // L4/R4/L5/R5 as SDL paddle buttons (only without Steam Input)
    GlyphFamily default_glyphs = GlyphFamily::Generic;
    std::vector<std::string> advice;
};
SteamInputPlan plan_steam_input(const SteamOsInfo& s, const SessionInfo& session, const IEnv& env, const std::vector<InputDevice>& devices);

// Request for the on-screen keyboard. Steam's own overlay keyboard is opened with a steam:// URL.
struct KeyboardRequest {
    std::vector<std::string> argv;       // program and arguments to exec (no shell), empty if impossible
    std::string reason;
};
KeyboardRequest plan_onscreen_keyboard(const SteamOsInfo& s, const IFileSystem& fs, const IEnv& env);

// ---------------------------------------------------------------------------- power (Deck)

struct AmdGpuStatus {
    bool present = false;
    std::string card;
    int busy_percent = -1;
    std::string perf_level;           // power_dpm_force_performance_level: auto, low, high, manual...
    int sclk_mhz = -1, mclk_mhz = -1; // current clocks from pp_dpm_sclk / pp_dpm_mclk (the '*' line)
    double power_cap_w = -1;          // hwmon power1_cap, the TDP limit when the OS programs it
    double power_now_w = -1;          // hwmon power1_average
    int temp_c = -1;
};
AmdGpuStatus read_amdgpu_status(const IFileSystem& fs);

// Highest "friendly" frame cap not above `wanted_fps` for the current conditions: below 20 % battery or under
// thermal pressure it steps down through the panel's frame-limit list. Returns wanted_fps when no cap is needed.
int suggest_fps_cap(const SteamOsInfo& s, const PowerInfo& power, bool thermal_serious, int wanted_fps);

// steamos-manager (D-Bus, system bus, read-only use): the OS service that owns TDP, GPU performance level and
// charge limit on SteamOS. Interface names changed between releases, so the service is introspected and only
// whitelisted properties are read.
struct DBusInterfaceInfo {
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;  // name, D-Bus type signature
};
std::vector<DBusInterfaceInfo> parse_introspection_xml(const std::string& xml);
// Properties worth showing (TDP limit and range, GPU performance level, charge limit, CPU scaling, fan control).
bool is_steamos_manager_property_of_interest(const std::string& name);

// ---------------------------------------------------------------------------- storage

struct SteamOsPaths {
    std::string user_data;         // saves: ~/.local/share/<org>/<app>, survives OS updates
    std::string cache;
    std::string settings;
    std::string shader_cache;      // our own pipeline / shader cache (see plan_shader_cache)
    std::string steam_shader_dir;  // Steam's per-app shader cache directory when Steam provided one
    bool install_must_stay_in_home = false;   // immutable root
    std::vector<std::string> notes;
};
SteamOsPaths plan_steamos_paths(const SteamOsInfo& s, const XdgDirs& xdg, const IEnv& env, const std::string& org, const std::string& app_id);

}  // namespace nue::lx
