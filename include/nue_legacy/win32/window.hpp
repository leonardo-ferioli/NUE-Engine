// Windows and messages (user32): window classes, windows, the message queues and the loop (GetMessage / PeekMessage / PostMessage / PostQuitMessage /
// TranslateMessage / DispatchMessage), DefWindowProc, focus and activation, the cursor, capture, clipping, window metrics.
//
// A window here is a table entry with the state a program can observe (class, style, rectangle, parent, visibility, focus). Window procedures are the
// guest's: the facade supplies one callback that runs the guest procedure `proc` with a message; this library only passes opaque ids. Messages come
// from two places: what programs post, and what the platform reports (window events and input events, translated into WM_* here, which is what lets
// the same guest loop run on a host that has no Win32 window system). On Windows the real HWND of the game's window keeps priority (docs/ABI.md 2.7
// of NUE: "the real HWND and its procedure are attached"); this implementation is what every other host uses.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/win32/handles.hpp"
#include "nue_legacy/platform/clock.hpp"
#include "nue_legacy/platform/display.hpp"
#include "nue_legacy/platform/input.hpp"
#include "nue_legacy/platform/threads.hpp"
#include "nue_legacy/platform/window.hpp"
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::win32 {

namespace wm {
enum : uint32_t {
    null = 0, create = 1, destroy = 2, move = 3, size = 5, activate = 6, setfocus = 7, killfocus = 8, close = 0x10, quit = 0x12, paint = 0xF, erasebkgnd = 0x14,
    showwindow = 0x18, activateapp = 0x1C, setcursor = 0x20, nccreate = 0x81, ncdestroy = 0x82, nccalcsize = 0x83, nchittest = 0x84, ncactivate = 0x86,
    keydown = 0x100, keyup = 0x101, char_ = 0x102, syskeydown = 0x104, syskeyup = 0x105, syschar = 0x106, syscommand = 0x112,
    mousemove = 0x200, lbuttondown = 0x201, lbuttonup = 0x202, rbuttondown = 0x204, rbuttonup = 0x205, mbuttondown = 0x207, mbuttonup = 0x208, mousewheel = 0x20A,
    xbuttondown = 0x20B, xbuttonup = 0x20C, user = 0x400
};
}
namespace ws { enum : uint32_t { popup = 0x80000000u, child = 0x40000000u, visible = 0x10000000u, caption = 0x00C00000u, border = 0x00800000u, sysmenu = 0x00080000u,
                                  thickframe = 0x00040000u, minimizebox = 0x00020000u, maximizebox = 0x00010000u, overlapped = 0, overlappedwindow = 0x00CF0000u, minimize = 0x20000000u, maximize = 0x01000000u }; }
namespace ws_ex { enum : uint32_t { topmost = 8, toolwindow = 0x80, appwindow = 0x40000, clientedge = 0x200, windowedge = 0x100 }; }
namespace gwl { enum : int32_t { wndproc = -4, hinstance = -6, hwndparent = -8, id = -12, style = -16, exstyle = -20, userdata = -21 }; }
namespace sw { enum : uint32_t { hide = 0, shownormal = 1, showminimized = 2, showmaximized = 3, shownoactivate = 4, show = 5, minimize = 6, restore = 9, showdefault = 10 }; }
namespace pm { enum : uint32_t { noremove = 0, remove = 1, noyield = 2 }; }
namespace sm { enum : int32_t { cxscreen = 0, cyscreen = 1, cxvscroll = 2, cyhscroll = 3, cycaption = 4, cxborder = 5, cyborder = 6, cxframe = 32, cyframe = 33, cxfixedframe = 7, cyfixedframe = 8, cmonitors = 80, cxvirtualscreen = 78, cyvirtualscreen = 79 }; }

struct Msg {                                  // MSG
    Handle hwnd = 0;
    uint32_t message = 0;
    uint32_t wparam = 0;
    int32_t lparam = 0;
    uint32_t time = 0;
    int32_t pt_x = 0, pt_y = 0;
};

struct WndClass {                             // WNDCLASSEX
    uint32_t style = 0;
    uint32_t proc = 0;                        // opaque id of the guest window procedure
    int32_t class_extra = 0, window_extra = 0;
    uint32_t instance = 0, icon = 0, cursor = 0, background = 0;
    std::string menu_name, name;
};

struct CreateParams {
    uint32_t ex_style = 0, style = 0;
    std::string class_name, title;
    int32_t x = 0x80000000, y = 0x80000000, width = 0x80000000, height = 0x80000000;   // CW_USEDEFAULT
    Handle parent = 0;
    uint32_t menu = 0, instance = 0, param = 0;
};

struct Rect32 { int32_t left = 0, top = 0, right = 0, bottom = 0; };

class WindowSystem {
public:
    // Runs the guest window procedure `proc`; the result is the LRESULT. Called on the thread that dispatches the message.
    using WndProcInvoker = std::function<int32_t(uint32_t proc, Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam)>;
    WindowSystem(platform::Clock& clock, platform::Threads& threads, HandleTable& handles, std::function<uint32_t()> current_thread_id);
    ~WindowSystem();
    void set_wndproc_invoker(WndProcInvoker invoker);
    // The platform window and input of the host: their events become messages. Either may be null (a test posts messages by hand).
    void attach_platform(platform::Window* window, platform::Input* input, platform::Display* display);

    // ---- classes and windows
    Ret<uint16_t> register_class(const WndClass&);                // the atom, 0 with ERROR_CLASS_ALREADY_EXISTS
    Ret<bool> unregister_class(std::string_view name);            // ERROR_CLASS_DOES_NOT_EXIST, or ERROR_BUSY (170) when windows of the class exist
    Ret<Handle> create_window(const CreateParams&);               // sends WM_NCCREATE, WM_NCCALCSIZE, WM_CREATE (and WM_SHOWWINDOW, WM_SIZE, WM_MOVE when visible)
    Ret<bool> destroy_window(Handle);                             // WM_DESTROY, WM_NCDESTROY; children first
    bool is_window(Handle) const;
    Ret<bool> show_window(Handle, uint32_t command);              // the previous visibility (true when it was visible)
    Ret<bool> update_window(Handle);                              // WM_PAINT when something is invalid
    Ret<bool> is_iconic(Handle);
    Ret<Rect32> get_window_rect(Handle);
    Ret<Rect32> get_client_rect(Handle);
    Ret<bool> set_window_pos(Handle, int32_t x, int32_t y, int32_t width, int32_t height);   // simplified SetWindowPos (move and size)
    Ret<int32_t> get_window_long(Handle, int32_t index);
    Ret<int32_t> set_window_long(Handle, int32_t index, int32_t value);   // returns the previous value
    static Rect32 adjust_window_rect(Rect32 client, uint32_t style, bool menu, uint32_t ex_style);
    int32_t get_system_metrics(int32_t index) const;

    // ---- focus, activation, foreground
    Handle set_focus(Handle);                                     // returns the previous focus window (0 when none); sends WM_KILLFOCUS / WM_SETFOCUS
    Handle get_focus() const;
    Ret<bool> set_foreground_window(Handle);
    Handle get_foreground_window() const;
    Handle set_active_window(Handle);                             // returns the previous active window
    Handle get_active_window() const;
    Handle get_desktop_window() const { return desktop_; }

    // ---- messages
    // GetMessage: blocks until a message is available; returns 1, 0 for WM_QUIT (the Ret value), -1 on error.
    Ret<int32_t> get_message(Msg& out, Handle hwnd, uint32_t min, uint32_t max);
    Ret<bool> peek_message(Msg& out, Handle hwnd, uint32_t min, uint32_t max, uint32_t flags);
    Ret<bool> post_message(Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam);   // hwnd 0 posts to the calling thread; 0xFFFF broadcasts
    Ret<bool> post_thread_message(uint32_t thread_id, uint32_t message, uint32_t wparam, int32_t lparam);
    void post_quit_message(int32_t exit_code);
    bool translate_message(const Msg&);                           // posts WM_CHAR / WM_SYSCHAR for key-down messages that make a character
    int32_t dispatch_message(const Msg&);                         // runs the window's procedure
    int32_t send_message(Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam);   // direct call on the owning thread's procedure
    int32_t def_window_proc(Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam);
    void pump_platform();                                         // turns pending platform events into messages (called by get/peek)

    // ---- cursor and mouse
    int32_t show_cursor(bool show);                               // the new display counter
    uint32_t set_cursor(uint32_t cursor);                         // the previous cursor
    bool get_cursor_pos(int32_t& x, int32_t& y);
    Handle set_capture(Handle);                                   // the previous capture window
    bool release_capture();
    Handle get_capture() const;
    bool clip_cursor(const Rect32* rect);                         // null releases
    uint32_t load_cursor(uint32_t instance, std::string_view name_or_id);   // a stable pseudo handle per resource
    uint32_t load_icon(uint32_t instance, std::string_view name_or_id);

    // The queue length and counters, for tests and the report of the bridge.
    size_t pending_messages(uint32_t thread_id) const;
    uint64_t messages_dispatched() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Handle desktop_ = 0;
};

// HID usage (platform scancode) -> Windows virtual-key code, 0 for a usage that has none.
uint32_t virtual_key_from_hid(uint32_t hid);

}  // namespace nue::legacy::win32
