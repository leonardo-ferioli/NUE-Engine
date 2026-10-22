// Win32 window handling beside SDL3.
//
// The recompiled game creates its own HWND (through CreateWindowExA in the guest import layer). The engine therefore ATTACHES to an
// existing window: a window subclass (comctl32 SetWindowSubclass) that observes focus, size, DPI, display and power messages and adds
// cursor control, without replacing the game's window procedure and without swallowing any message except where stated.
// Mode switching (windowed / borderless / exclusive) is planned by pure functions and applied with SetWindowPos.
#pragma once
#include <functional>
#include <memory>

#include "nue_win/common.hpp"
#include "nue_win/display.hpp"
#include "nue_win/input.hpp"

namespace nue::win {

// ---- Mode planning ----------------------------------------------------------------------------------------------------------------------
enum class WindowMode : uint8_t { Windowed, Borderless, Exclusive };
const char* window_mode_name(WindowMode m);

struct WindowModeRequest {
    WindowMode mode = WindowMode::Windowed;
    HMONITOR monitor = nullptr;                    // null = the monitor the window is on
    uint32_t client_width = 0, client_height = 0;  // windowed: wanted client area; 0 = keep the current size
    bool fit_to_work_area = false;                 // windowed: largest client area with the content aspect that fits the work area ("window scaled to the screen")
    uint32_t content_width = 0, content_height = 0;   // aspect reference for fit_to_work_area (0 = client_width/height)
    bool clamp_to_work_area = true;                // windowed: shrink (keeping aspect) a client area that would not fit on the monitor
    bool center = true;                            // windowed: centre in the work area; false keeps the current top-left when possible
};

// What the plan is computed from (all plain data, so tests can feed any monitor layout and DPI).
struct WindowSnapshot {
    DWORD style = 0, ex_style = 0;
    bool has_menu = false;
    RECT window_rect{};           // current outer rectangle (screen coordinates)
    SIZE client_size{};           // current client area
    bool visible = true;
};
struct MonitorGeometry {
    RECT monitor{}, work{};
    uint32_t dpi = 96;
};
struct WindowPlan {
    WindowMode mode = WindowMode::Windowed;
    DWORD style = 0, ex_style = 0;
    RECT window_rect{};           // outer rectangle to set
    SIZE client_size{};           // resulting client area
    bool topmost = false;
    bool covers_monitor = false;
    FitRect content{};            // where the picture goes inside the client area when its aspect differs (bars), if content size was given
};
// Styles to use when leaving windowed mode and when entering borderless: `original` is the windowed style remembered by the controller.
WindowPlan plan_window_mode(const WindowSnapshot& original_windowed, const WindowSnapshot& current, const MonitorGeometry& monitor, const WindowModeRequest& request);
// Window styles as the plan wants them (exposed for tests).
DWORD borderless_style(DWORD windowed_style);
DWORD borderless_ex_style(DWORD windowed_ex_style);

Status apply_window_plan(HWND hwnd, const WindowPlan& plan);

// Remembers the windowed state when leaving it, so that Windowed restores exactly what the game had.
class WindowModeController {
public:
    WindowMode mode() const { return mode_; }
    Status set_mode(HWND hwnd, const WindowModeRequest& request, WindowPlan* applied = nullptr);
    // The plan that set_mode would apply, without touching the window.
    Status plan(HWND hwnd, const WindowModeRequest& request, WindowPlan& out) const;
    // Forget the remembered windowed state (the game changed its style by itself).
    void reset() { mode_ = WindowMode::Windowed; have_windowed_ = false; }
private:
    WindowMode mode_ = WindowMode::Windowed;
    bool have_windowed_ = false;
    WindowSnapshot windowed_;
};

// ---- Attached window -----------------------------------------------------------------------------------------------------------------------
struct WindowCallbacks {
    std::function<void(bool focused)> on_focus;
    std::function<void(uint32_t width, uint32_t height, bool minimized)> on_size;                 // client size
    std::function<void(uint32_t dpi, const RECT& suggested)> on_dpi_changed;
    std::function<void(uint32_t width, uint32_t height, uint32_t bits_per_pixel)> on_display_change;
    std::function<void()> on_suspend;
    std::function<void()> on_resume;
    std::function<void(bool locked)> on_session_lock;         // workstation lock / unlock
    std::function<void()> on_device_change;                   // WM_DEVICECHANGE: audio / HID hot-plug hint
    std::function<bool()> on_close_request;                   // return false to swallow the close (the only message the subclass can consume besides WM_SETCURSOR)
    std::function<void(const RawInputEvent&)> on_raw_input;   // only while raw input is enabled
};

class WindowAttachment {
public:
    // Must be called on the thread that created `hwnd`. The attachment never owns the window.
    static std::unique_ptr<WindowAttachment> attach(HWND hwnd, WindowCallbacks callbacks, Status* status = nullptr);
    ~WindowAttachment();
    WindowAttachment(const WindowAttachment&) = delete;
    WindowAttachment& operator=(const WindowAttachment&) = delete;

    HWND hwnd() const { return hwnd_; }
    bool attached() const { return hwnd_ != nullptr; }
    void detach();                 // also done automatically when the window is destroyed

    bool focused() const { return focused_; }
    uint32_t dpi() const { return dpi_; }
    WindowModeController& mode_controller() { return mode_; }
    Status set_mode(const WindowModeRequest& r, WindowPlan* applied = nullptr) { return mode_.set_mode(hwnd_, r, applied); }

    // Cursor. Hiding is done by answering WM_SETCURSOR (no global ShowCursor counter is touched), so nothing can leak after a crash.
    void set_cursor_visible(bool visible);
    bool cursor_visible() const { return cursor_visible_; }
    // Confines the pointer to the client area while the window is focused; released automatically on focus loss, minimise and detach.
    Status set_cursor_clip(bool clip);
    bool cursor_clipped() const { return clip_wanted_; }
    // Relative mouse: pointer hidden and clipped, deltas from Raw Input (unaccelerated). Needs the window thread's message loop.
    Status set_relative_mouse(bool enabled);
    bool relative_mouse() const { return relative_; }
    bool take_relative_motion(int32_t& dx, int32_t& dy);
    RawInput& raw_input() { return raw_; }

    // Testing seam: when set, clipping goes through this instead of ClipCursor (the test must not confine the real pointer).
    void set_clip_hook(std::function<void(const RECT* rect)> hook) { clip_hook_ = std::move(hook); }

private:
    WindowAttachment() = default;
    static LRESULT CALLBACK subclass_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM, bool& pass_on);
    void apply_clip();
    void release_clip();

    HWND hwnd_ = nullptr;
    WindowCallbacks cb_;
    WindowModeController mode_;
    RawInput raw_;
    bool focused_ = true;
    bool cursor_visible_ = true, clip_wanted_ = false, clip_active_ = false, relative_ = false, session_registered_ = false, resumed_pending_ = false;
    uint32_t dpi_ = 96;
    std::function<void(const RECT*)> clip_hook_;
};

// Puts back everything the engine could have left changed on the desktop: pointer clip released, pointer shown, display modes restored,
// window affinity untouched. Safe to call from a crash handler, an atexit handler or a watchdog; idempotent.
void emergency_restore_desktop();

}  // namespace nue::win
