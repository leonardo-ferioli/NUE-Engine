// Fitting a picture of a given size into an area (a window, a monitor) without distorting it: scaled to the largest size that fits, centred.
// Used by the bgfx adapter's present (letterbox / pillarbox) and by the launcher's "window scaled to the screen" mode.
#pragma once
#include <algorithm>
#include <cstdint>

namespace nue {

struct FitRect { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };

// `content` w x h inside the area (area_x, area_y, area_w, area_h). Equal aspect ratios fill the area exactly; otherwise bars appear on two sides.
inline FitRect fit_aspect(uint32_t content_w, uint32_t content_h, int32_t area_x, int32_t area_y, uint32_t area_w, uint32_t area_h) {
    FitRect r;
    if (!content_w || !content_h || !area_w || !area_h) return r;
    // compare content_w / content_h with area_w / area_h without floating point
    const uint64_t lhs = uint64_t(content_w) * area_h, rhs = uint64_t(area_w) * content_h;
    if (lhs == rhs) { r.width = area_w; r.height = area_h; }
    else if (lhs > rhs) { r.width = area_w; r.height = std::max<uint32_t>(1, uint32_t(uint64_t(area_w) * content_h / content_w)); }   // content is wider: fit the width
    else { r.height = area_h; r.width = std::max<uint32_t>(1, uint32_t(uint64_t(area_h) * content_w / content_h)); }                  // content is taller: fit the height
    r.x = area_x + int32_t((area_w - r.width) / 2);
    r.y = area_y + int32_t((area_h - r.height) / 2);
    return r;
}

}  // namespace nue
