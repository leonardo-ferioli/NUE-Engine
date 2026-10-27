// Platform API: the game's window.
//
// On Windows the guest creates its window through CreateWindowExA (the mods override the call for the windowed modes), so the Window
// *adopts* it; on other hosts `create` makes the window. The bgfx adapter reads the native handle from `native_surface()`.
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <string_view>

namespace nue::legacy::platform {

struct Rect { int32_t left = 0, top = 0, right = 0, bottom = 0; };

struct NativeSurface {
    enum class Kind { none, win32_hwnd, xlib_window, wayland_surface, android_native_window, cametal_layer } kind = Kind::none;
    void* a = nullptr;   // HWND / Window / wl_surface / ANativeWindow* / CAMetalLayer*
    void* b = nullptr;   // display connection where one is needed
};

enum class WindowMode { windowed, borderless, fullscreen };

enum class EventKind { close, focus_gained, focus_lost, resized, display_changed, suspended, resumed, surface_lost, surface_ready, terminating };
struct WindowEvent { EventKind kind = EventKind::close; uint32_t width = 0, height = 0; };

// Frame (decoration) of a window: what the windowed modes of the widescreen fix choose between.
struct FrameStyle {
    bool borderless = true;    // no caption, no frame
    bool resizable = false;    // thick frame and maximize box (only meaningful when not borderless)
};

class Window {
public:
    virtual ~Window() = default;

    virtual Status create(std::string_view title, uint32_t width, uint32_t height, WindowMode mode, uint32_t display_id) = 0;
    virtual Status adopt(NativeSurface existing) = 0;
    virtual bool   adopted() = 0;
    virtual void   destroy() = 0;
    virtual NativeSurface native_surface() = 0;

    // Decoration and placement. Positions are desktop pixels.
    virtual void set_frame(FrameStyle) = 0;             // changes the decoration only (scroll bars are always removed)
    virtual void refresh_frame() = 0;                   // applies a decoration change without moving or resizing
    virtual void place_outer(int32_t x, int32_t y, int32_t width, int32_t height) = 0;   // outer rectangle, frame refreshed
    // Places the window so that its client area is `client_width x client_height`, centred on a display of display_width x display_height
    // (the frame is added around it); `show` also makes it visible and not topmost.
    virtual void place_client_centered(int32_t client_width, int32_t client_height, int32_t display_width, int32_t display_height, bool show) = 0;
    virtual Rect outer_rect() = 0;
    virtual Rect client_rect() = 0;                     // origin (0,0)

    // Cursor.
    virtual bool cursor_inside_client() = 0;
    virtual void show_cursor(bool show) = 0;
    virtual void show_cursor_forced() = 0;              // makes the cursor visible whatever the show count was
    virtual void release_cursor_clip() = 0;
    virtual bool focused() = 0;

    // Window events for hosts whose loop is not the guest's own (the Windows backend delivers none: the guest pumps its own messages).
    virtual size_t poll_events(std::span<WindowEvent> out) = 0;
};

}  // namespace nue::legacy::platform
