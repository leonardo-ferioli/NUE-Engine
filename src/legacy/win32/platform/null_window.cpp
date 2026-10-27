#include "nue_legacy/platform/null_window.hpp"

namespace nue::legacy::platform::null {

NullDisplay::NullDisplay(uint32_t width, uint32_t height, uint32_t refresh_hz) {
    DisplayInfo info;
    info.id = 0;
    info.name = "null0";
    info.primary = true;
    info.current = {width, height, refresh_hz * 1000u};
    info.desktop_width = width;
    info.desktop_height = height;
    info.modes = {{width, height, refresh_hz * 1000u}, {width / 2, height / 2, refresh_hz * 1000u}};
    displays.push_back(std::move(info));
}

}  // namespace nue::legacy::platform::null
