// Windows Game Mode and gaming-related system state.
//
// What an application CAN do: be a well-behaved foreground full-screen (or borderless-covering-the-monitor) window with a flip-model swap
// chain; detect whether Windows put the system in Game Mode (effective power mode), whether Game Bar / capture are running, whether
// fullscreen optimizations are disabled for this exe; exclude its window from capture. What it CANNOT do: switch Game Mode on or off
// (it is automatic for a recognised foreground game, and the user's choice in Settings), or force Xbox Game Bar to show/hide.
#pragma once
#include <string>

#include "nue_win/common.hpp"
#include "nue_win/power.hpp"

namespace nue::win {

struct GameModeInfo {
    // User settings (HKCU\Software\Microsoft\GameBar): Unknown when the value was never written (Windows then uses its default, which is ON).
    Support game_mode_allowed = Support::Unknown;        // AutoGameModeEnabled / AllowAutoGameMode
    Support game_bar_enabled = Support::Unknown;         // UseNexusForGameBarEnabled
    Support background_capture_enabled = Support::Unknown;   // HKCU\...\GameDVR AppCaptureEnabled
    Support capture_forbidden_by_policy = Support::Unknown;  // HKLM\SOFTWARE\Policies\Microsoft\Windows\GameDVR AllowGameDVR == 0
    // Live state
    EffectivePowerMode power_mode = EffectivePowerMode::Unknown;
    bool game_mode_active = false;                       // power_mode == GameMode
    int notification_state = -1;                         // SHQueryUserNotificationState (QUNS_*), -1 when the call failed
    bool windows_treats_us_as_fullscreen_game = false;   // QUNS_RUNNING_D3D_FULL_SCREEN
    bool game_bar_running = false;                       // GameBar.exe / GameBarFTServer.exe / GameBarPresenceWriter.exe / XboxGameBarWidgets.exe
    Support fullscreen_optimizations_disabled = Support::Unknown;   // AppCompat layer DISABLEDXMAXIMIZEDWINDOWEDMODE for the running exe
};
GameModeInfo query_game_mode_info();
const char* notification_state_name(int quns);

// Window capture: Game Bar, Snipping Tool, Teams, OBS "Window capture" (DXGI/GraphicsCapture based) see black instead of the window.
// WDA_EXCLUDEFROMCAPTURE needs Windows 10 2004+ (build 19041); older builds get WDA_MONITOR (black rectangle in captures).
Status window_exclude_from_capture(HWND hwnd, bool exclude);
bool window_is_excluded_from_capture(HWND hwnd);

// Fullscreen optimizations. They are per-exe user settings (Properties > Compatibility); a program may read them and, if the user wants,
// write its own entry in HKCU. Writing is opt-in and never done by the tests on the real key.
struct CompatLayerStore { const wchar_t* subkey = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers"; };
Support query_fullscreen_optimizations_disabled(const std::wstring& exe_path, const CompatLayerStore& store = {});
Status set_fullscreen_optimizations_disabled(const std::wstring& exe_path, bool disabled, const CompatLayerStore& store = {});

// Pure parser of the AppCompat layer string ("~ HIGHDPIAWARE DISABLEDXMAXIMIZEDWINDOWEDMODE"), exposed for tests.
bool layers_contain(const std::wstring& layers, const wchar_t* flag);
std::wstring layers_with_flag(const std::wstring& layers, const wchar_t* flag, bool present);

}  // namespace nue::win
