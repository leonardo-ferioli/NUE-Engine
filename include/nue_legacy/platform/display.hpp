// Platform API: displays (monitors) and their modes.
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <string>
#include <vector>

namespace nue::legacy::platform {

struct DisplayMode {
    uint32_t width = 0, height = 0;
    uint32_t refresh_millihertz = 0;   // 0 when the system does not say
};

struct DisplayInfo {
    uint32_t id = 0;
    std::string name;
    bool primary = false;
    DisplayMode current;                          // the mode the display runs in now
    uint32_t desktop_width = 0, desktop_height = 0;   // the display's area of the desktop (equals `current` unless the OS scales)
    float content_scale = 1.0f;                   // DPI factor
    struct { int32_t left = 0, top = 0, right = 0, bottom = 0; } safe_area_px;   // insets (notches, rounded corners); zero on desktops
    std::vector<DisplayMode> modes;               // every mode the display offers (empty if the system reports none)
};

class Display {
public:
    virtual ~Display() = default;
    // All displays; the primary one is flagged. Never empty on a machine that has a screen. `include_modes` = false skips the
    // (slow) enumeration of every mode when only the current size and refresh rate are wanted.
    virtual std::vector<DisplayInfo> list(bool include_modes = true) = 0;
    // Real pixel sizes for the desktop and the window (no OS bitmap scaling); a no-op where the OS never scales.
    virtual void set_dpi_aware() = 0;
};

}  // namespace nue::legacy::platform
