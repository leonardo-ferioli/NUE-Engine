// Null implementations of Display and Window: deterministic, no OS. They let the window system of legacy_win32 be tested headless.
#pragma once
#include "nue_legacy/platform/display.hpp"
#include "nue_legacy/platform/window.hpp"
#include <deque>
#include <string>
#include <vector>

namespace nue::legacy::platform::null {

class NullDisplay final : public Display {
public:
    explicit NullDisplay(uint32_t width = 1280, uint32_t height = 720, uint32_t refresh_hz = 60);
    std::vector<DisplayInfo> list(bool include_modes = true) override {
        auto copy = displays;
        if (!include_modes) for (auto& d : copy) d.modes.clear();
        return copy;
    }
    void set_dpi_aware() override { dpi_aware = true; }
    std::vector<DisplayInfo> displays;
    bool dpi_aware = false;
};

class NullWindow final : public Window {
public:
    Status create(std::string_view title_, uint32_t w, uint32_t h, WindowMode m, uint32_t) override { title = title_; width = w; height = h; mode = m; exists = true; return Status::ok; }
    Status adopt(NativeSurface s) override { surface = s; exists = true; return Status::ok; }
    bool adopted() override { return exists; }
    void destroy() override { exists = false; }
    NativeSurface native_surface() override { return surface; }
    void set_frame(FrameStyle f) override { frame = f; }
    void refresh_frame() override { ++refreshes; }
    void place_outer(int32_t x, int32_t y, int32_t w, int32_t h) override { outer = {x, y, x + w, y + h}; ++placements; }
    void place_client_centered(int32_t cw, int32_t ch, int32_t dw, int32_t dh, bool show) override {
        const int32_t x = (dw - cw) / 2, y = (dh - ch) / 2;
        outer = {x, y, x + cw, y + ch};
        shown = shown || show;
        ++placements;
    }
    Rect outer_rect() override { return outer; }
    Rect client_rect() override { return {0, 0, outer.right - outer.left, outer.bottom - outer.top}; }
    bool cursor_inside_client() override { return cursor_inside; }
    void show_cursor(bool show) override { cursor_visible = show; }
    void show_cursor_forced() override { cursor_visible = true; }
    void release_cursor_clip() override { ++clip_releases; }
    bool focused() override { return exists; }
    size_t poll_events(std::span<WindowEvent> out) override {
        size_t n = 0;
        while (n < out.size() && !events.empty()) { out[n++] = events.front(); events.pop_front(); }
        return n;
    }

    std::string title;
    uint32_t width = 0, height = 0;
    WindowMode mode = WindowMode::windowed;
    NativeSurface surface;
    FrameStyle frame;
    Rect outer;
    bool exists = false, shown = false, cursor_inside = true, cursor_visible = true;
    unsigned refreshes = 0, placements = 0, clip_releases = 0;
    std::deque<WindowEvent> events;
};

}  // namespace nue::legacy::platform::null
