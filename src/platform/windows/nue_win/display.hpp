// Displays: monitors, per-monitor DPI, display modes, refresh rate (exact rational), HDR / advanced colour, aspect-preserving fit.
#pragma once
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

// ---- Aspect fit (same arithmetic as nue_d3d9's window_fit.hpp, integer only) --------------------------------------------------------
struct FitRect { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };
// `content` w x h scaled to the largest size that fits (area_x, area_y, area_w, area_h), centred; bars appear on two sides otherwise.
FitRect fit_aspect(uint32_t content_w, uint32_t content_h, int32_t area_x, int32_t area_y, uint32_t area_w, uint32_t area_h);

// ---- DPI ---------------------------------------------------------------------------------------------------------------------------
enum class DpiAwareness : uint8_t { Unaware, System, PerMonitor, PerMonitorV2, Unknown };
const char* dpi_awareness_name(DpiAwareness a);
// Call once, before any window is created (or put the equivalent in the application manifest, which is the supported way for an exe).
// Falls back from Per-Monitor V2 to Per-Monitor to System. Returns the level obtained.
DpiAwareness enable_best_dpi_awareness(Status* detail = nullptr);
DpiAwareness query_process_dpi_awareness();
uint32_t window_dpi(HWND hwnd);            // GetDpiForWindow, 96 if unavailable
uint32_t monitor_dpi(HMONITOR monitor);    // effective DPI (GetDpiForMonitor), 96 if unavailable

// ---- Monitors ------------------------------------------------------------------------------------------------------------------------
struct DisplayMode {
    uint32_t width = 0, height = 0, bits_per_pixel = 32, refresh_hz = 0;
    bool interlaced = false;
    bool operator==(const DisplayMode& o) const { return width == o.width && height == o.height && bits_per_pixel == o.bits_per_pixel && refresh_hz == o.refresh_hz && interlaced == o.interlaced; }
};

struct MonitorInfo {
    HMONITOR handle = nullptr;
    std::wstring device_name;       // \\.\DISPLAY1 (GDI name; use it with the display-mode functions)
    std::string friendly_name;      // from the EDID via DisplayConfig, may be empty
    RECT monitor_rect{}, work_rect{};
    bool primary = false;
    uint32_t dpi = 96;
    double scale = 1.0;             // dpi / 96
    DisplayMode current;            // as set in Windows (EnumDisplaySettings ENUM_CURRENT_SETTINGS)
    uint32_t refresh_numerator = 0, refresh_denominator = 0;   // exact, from DisplayConfig (e.g. 60000/1001); 0 when unknown
    double refresh_exact() const { return refresh_denominator ? double(refresh_numerator) / double(refresh_denominator) : double(current.refresh_hz); }
    Support hdr_supported = Support::Unknown;   // advanced colour supported by this display
    Support hdr_enabled = Support::Unknown;     // "Use HDR" is on in Windows Settings
    Support wide_color_enforced = Support::Unknown;
    uint32_t bits_per_color = 0;
    float sdr_white_level_nits = 0;             // the SDR content brightness slider, in nits (HDR on)
    int32_t width() const { return monitor_rect.right - monitor_rect.left; }
    int32_t height() const { return monitor_rect.bottom - monitor_rect.top; }
};

// Enumerates the active monitors (EnumDisplayMonitors + DisplayConfig). Primary monitor first.
bool enumerate_monitors(std::vector<MonitorInfo>& out);
bool monitor_info_from_handle(HMONITOR h, MonitorInfo& out);
// Monitor holding the largest part of the window (nearest if none), or the primary monitor when `hwnd` is null.
bool monitor_of_window(HWND hwnd, MonitorInfo& out);
RECT virtual_screen_rect();

// ---- Display modes --------------------------------------------------------------------------------------------------------------------
// All distinct modes the monitor supports, sorted by width, height, refresh. Modes that Windows lists as "raw" (not reported to
// applications by default) are included only when `include_raw`.
std::vector<DisplayMode> enumerate_display_modes(const std::wstring& device_name, bool include_raw = false);
// Picks the mode with the given size and the closest refresh rate that is not above `refresh_hz` (or the highest below it); width/height must match exactly.
bool find_display_mode(const std::vector<DisplayMode>& modes, uint32_t width, uint32_t height, uint32_t refresh_hz, DisplayMode& out);
// CDS_TEST: asks Windows whether the mode could be set. Changes nothing.
Status test_display_mode(const std::wstring& device_name, const DisplayMode& m);
// Exclusive full-screen mode switch. CDS_FULLSCREEN makes Windows revert the mode when the process exits or loses the foreground; always
// pair with restore_display_mode(). Not used by the tests (it would flicker the user's screen).
Status set_display_mode(const std::wstring& device_name, const DisplayMode& m);
Status restore_display_mode(const std::wstring& device_name);
// Restores every device that set_display_mode() changed and has not restored yet (crash / exit safety net).
void restore_all_changed_display_modes();

}  // namespace nue::win
