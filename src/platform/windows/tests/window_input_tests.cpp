#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "nue_win/input.hpp"
#include "nue_win/window.hpp"
#include "test_framework.hpp"
#include "test_window.hpp"

using namespace nue::win;

static MonitorGeometry geometry_1080p(uint32_t dpi = 96) {
    MonitorGeometry g;
    g.monitor = RECT{0, 0, 1920, 1080};
    g.work = RECT{0, 0, 1920, 1040};   // taskbar 40 px
    g.dpi = dpi;
    return g;
}

static WindowSnapshot windowed_snapshot(int client_w = 800, int client_h = 600) {
    WindowSnapshot s;
    s.style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    s.ex_style = 0;
    RECT r{0, 0, client_w, client_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s.window_rect = RECT{100, 100, 100 + (r.right - r.left), 100 + (r.bottom - r.top)};
    s.client_size = SIZE{client_w, client_h};
    return s;
}

static void test_plan() {
    TEST_BEGIN("window mode planning (pure)");
    const WindowSnapshot w = windowed_snapshot();
    WindowModeRequest req;
    req.mode = WindowMode::Borderless;
    WindowPlan p = plan_window_mode(w, w, geometry_1080p(), req);
    CHECK(p.covers_monitor && !p.topmost);
    CHECK(p.window_rect.left == 0 && p.window_rect.top == 0 && p.window_rect.right == 1920 && p.window_rect.bottom == 1080);
    CHECK((p.style & WS_POPUP) && !(p.style & WS_CAPTION) && !(p.style & WS_THICKFRAME) && (p.style & WS_VISIBLE));
    CHECK(p.client_size.cx == 1920 && p.client_size.cy == 1080);

    req.mode = WindowMode::Exclusive;
    p = plan_window_mode(w, w, geometry_1080p(), req);
    CHECK(p.topmost && p.covers_monitor);

    req = WindowModeRequest{};
    req.client_width = 1280;
    req.client_height = 720;
    p = plan_window_mode(w, w, geometry_1080p(), req);
    CHECK(p.client_size.cx == 1280 && p.client_size.cy == 720);
    CHECK(!p.covers_monitor && p.style == w.style);
    const int outer_w = p.window_rect.right - p.window_rect.left, outer_h = p.window_rect.bottom - p.window_rect.top;
    CHECK(p.window_rect.left == (1920 - outer_w) / 2 && p.window_rect.top == (1040 - outer_h) / 2);   // centred in the work area

    // larger than the work area: shrunk keeping the aspect
    req.client_width = 3840;
    req.client_height = 2160;
    p = plan_window_mode(w, w, geometry_1080p(), req);
    CHECK(p.client_size.cx * 9 == p.client_size.cy * 16 || std::abs(p.client_size.cx * 9 - p.client_size.cy * 16) < 16);
    CHECK(p.window_rect.right - p.window_rect.left <= 1920 && p.window_rect.bottom - p.window_rect.top <= 1040);
    CHECK(p.window_rect.left >= 0 && p.window_rect.top >= 0);

    // "window scaled to the screen" for a 4:3 picture: the largest 4:3 client that fits the work area, bars handled by the content rect
    req = WindowModeRequest{};
    req.fit_to_work_area = true;
    req.content_width = 640;
    req.content_height = 480;
    p = plan_window_mode(w, w, geometry_1080p(), req);
    CHECK(std::abs(p.client_size.cx * 3 - p.client_size.cy * 4) <= 3);
    CHECK(p.window_rect.bottom - p.window_rect.top <= 1040);
    CHECK(p.window_rect.bottom - p.window_rect.top >= 1040 - 4);   // height-limited: fills the work area vertically
    CHECK(std::abs(int(p.content.width) - p.client_size.cx) <= 2 && std::abs(int(p.content.height) - p.client_size.cy) <= 2);   // integer rounding of the 4:3 fit

    // fit with a content that differs from the chosen client area: bars
    req = WindowModeRequest{};
    req.client_width = 1280;
    req.client_height = 800;   // 16:10 window
    req.content_width = 16;
    req.content_height = 9;
    p = plan_window_mode(w, w, geometry_1080p(), req);
    CHECK(p.content.width == 1280 && p.content.height == 720 && p.content.y == 40);

    // DPI: the frame is larger at 200 %
    req = WindowModeRequest{};
    req.client_width = 1000;
    req.client_height = 700;
    const WindowPlan p96 = plan_window_mode(w, w, geometry_1080p(96), req);
    const WindowPlan p192 = plan_window_mode(w, w, geometry_1080p(192), req);
    CHECK((p192.window_rect.right - p192.window_rect.left) > (p96.window_rect.right - p96.window_rect.left));

    // second monitor to the left of the primary, with negative coordinates
    MonitorGeometry left;
    left.monitor = RECT{-1920, 0, 0, 1080};
    left.work = left.monitor;
    req = WindowModeRequest{};
    req.mode = WindowMode::Borderless;
    p = plan_window_mode(w, w, left, req);
    CHECK(p.window_rect.left == -1920 && p.window_rect.right == 0);
    req = WindowModeRequest{};
    req.client_width = 800;
    req.client_height = 600;
    p = plan_window_mode(w, w, left, req);
    CHECK(p.window_rect.left >= -1920 && p.window_rect.right <= 0);
}

static void test_apply_real_window() {
    TEST_BEGIN("window mode switching on a hidden window");
    nuetest::HiddenWindow w(true, 800, 600);
    CHECK(w.hwnd != nullptr);
    const DWORD style0 = DWORD(GetWindowLongPtrW(w.hwnd, GWL_STYLE));
    RECT c0;
    GetClientRect(w.hwnd, &c0);
    MonitorInfo mon;
    CHECK(monitor_of_window(w.hwnd, mon));

    WindowModeController ctl;
    WindowModeRequest req;
    req.mode = WindowMode::Borderless;
    CHECK_OK(ctl.set_mode(w.hwnd, req));
    CHECK(ctl.mode() == WindowMode::Borderless);
    RECT r;
    GetWindowRect(w.hwnd, &r);
    CHECK(r.left == mon.monitor_rect.left && r.top == mon.monitor_rect.top && r.right == mon.monitor_rect.right && r.bottom == mon.monitor_rect.bottom);
    CHECK(!(GetWindowLongPtrW(w.hwnd, GWL_STYLE) & WS_CAPTION));
    CHECK(!IsWindowVisible(w.hwnd));   // switching never shows a hidden window

    req.mode = WindowMode::Exclusive;
    CHECK_OK(ctl.set_mode(w.hwnd, req));
    CHECK(GetWindowLongPtrW(w.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST);

    req = WindowModeRequest{};   // windowed, same size as before
    CHECK_OK(ctl.set_mode(w.hwnd, req));
    CHECK(ctl.mode() == WindowMode::Windowed);
    CHECK(DWORD(GetWindowLongPtrW(w.hwnd, GWL_STYLE)) == style0);
    CHECK(!(GetWindowLongPtrW(w.hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST));
    RECT c1;
    GetClientRect(w.hwnd, &c1);
    CHECK(c1.right - c1.left == c0.right - c0.left && c1.bottom - c1.top == c0.bottom - c0.top);

    req.client_width = 640;
    req.client_height = 360;
    CHECK_OK(ctl.set_mode(w.hwnd, req));
    GetClientRect(w.hwnd, &c1);
    CHECK(c1.right - c1.left == 640 && c1.bottom - c1.top == 360);

    WindowPlan planned;
    CHECK(!ctl.plan(HWND(0x12345678), req, planned).ok);   // a bad handle is refused
}

static void test_attachment() {
    TEST_BEGIN("attach to an existing HWND");
    nuetest::HiddenWindow w(true);
    struct Seen {
        int focus_changes = 0, sizes = 0, dpi = 0, display = 0, suspend = 0, resume = 0, lock = 0, unlock = 0, device = 0, close_requests = 0;
        bool focused = true;
        uint32_t last_w = 0, last_h = 0, last_dpi = 0;
        bool minimized = false;
        bool allow_close = false;
    } seen;
    WindowCallbacks cb;
    cb.on_focus = [&](bool f) { seen.focused = f; ++seen.focus_changes; };
    cb.on_size = [&](uint32_t cw, uint32_t ch, bool min) { seen.last_w = cw; seen.last_h = ch; seen.minimized = min; ++seen.sizes; };
    cb.on_dpi_changed = [&](uint32_t d, const RECT&) { seen.last_dpi = d; ++seen.dpi; };
    cb.on_display_change = [&](uint32_t, uint32_t, uint32_t) { ++seen.display; };
    cb.on_suspend = [&] { ++seen.suspend; };
    cb.on_resume = [&] { ++seen.resume; };
    cb.on_session_lock = [&](bool locked) { locked ? ++seen.lock : ++seen.unlock; };
    cb.on_device_change = [&] { ++seen.device; };
    cb.on_close_request = [&] { ++seen.close_requests; return seen.allow_close; };

    Status st;
    auto a = WindowAttachment::attach(w.hwnd, cb, &st);
    CHECK_OK(st);
    CHECK(a && a->attached() && a->hwnd() == w.hwnd);
    Status again;
    CHECK(WindowAttachment::attach(w.hwnd, cb, &again) == nullptr && !again.ok && again.code == ERROR_ALREADY_EXISTS);   // one attachment per window

    SendMessageW(w.hwnd, WM_ACTIVATEAPP, TRUE, 0);
    CHECK(a->focused());
    const int before = seen.focus_changes;
    SendMessageW(w.hwnd, WM_ACTIVATEAPP, FALSE, 0);
    CHECK(!a->focused() && seen.focus_changes == before + 1 && !seen.focused);
    SendMessageW(w.hwnd, WM_ACTIVATEAPP, FALSE, 0);
    CHECK(seen.focus_changes == before + 1);   // no duplicate notifications
    SendMessageW(w.hwnd, WM_ACTIVATEAPP, TRUE, 0);

    SendMessageW(w.hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 720));
    CHECK(seen.last_w == 1280 && seen.last_h == 720 && !seen.minimized);
    SendMessageW(w.hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
    CHECK(seen.minimized);

    RECT suggested{10, 20, 1000, 800};
    SendMessageW(w.hwnd, WM_DPICHANGED, MAKEWPARAM(144, 144), LPARAM(&suggested));
    CHECK(seen.last_dpi == 144 && a->dpi() == 144);
    SendMessageW(w.hwnd, WM_DISPLAYCHANGE, 32, MAKELPARAM(1920, 1080));
    CHECK(seen.display == 1);

    SendMessageW(w.hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    SendMessageW(w.hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    SendMessageW(w.hwnd, WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);   // the second resume message is reported once
    CHECK(seen.suspend == 1 && seen.resume == 1);
    SendMessageW(w.hwnd, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);   // spurious resume without suspend
    CHECK(seen.resume == 1);

    SendMessageW(w.hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 1);
    SendMessageW(w.hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 1);
    CHECK(seen.lock == 1 && seen.unlock == 1);
    SendMessageW(w.hwnd, WM_DEVICECHANGE, 0x0007 /*DBT_DEVNODES_CHANGED*/, 0);
    CHECK(seen.device == 1);

    SendMessageW(w.hwnd, WM_CLOSE, 0, 0);
    CHECK(seen.close_requests == 1 && IsWindow(w.hwnd));   // the callback refused: the window stays

    // The subclass leaves every other message to the game's own procedure.
    CHECK(SendMessageW(w.hwnd, WM_GETTEXTLENGTH, 0, 0) == LRESULT(wcslen(L"nue test (hidden)")));

    // Cursor hiding answers WM_SETCURSOR in the client area only.
    a->set_cursor_visible(false);
    CHECK(!a->cursor_visible());
    CHECK(SendMessageW(w.hwnd, WM_SETCURSOR, WPARAM(w.hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE)) == TRUE);
    CHECK(GetCursor() == nullptr);
    CHECK(SendMessageW(w.hwnd, WM_SETCURSOR, WPARAM(w.hwnd), MAKELPARAM(HTCAPTION, WM_MOUSEMOVE)) == FALSE);   // title bar: default handling
    a->set_cursor_visible(true);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    seen.allow_close = true;
    a->detach();
    CHECK(!a->attached());
    // after detach nothing is reported any more
    const int sizes = seen.sizes;
    SendMessageW(w.hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(10, 10));
    CHECK(seen.sizes == sizes);
}

static void test_clip_and_destroy() {
    TEST_BEGIN("cursor clip lifecycle (hooked, the real pointer is never confined)");
    auto w = std::make_unique<nuetest::HiddenWindow>(true, 640, 480);
    auto a = WindowAttachment::attach(w->hwnd, {});
    CHECK(a != nullptr);
    std::vector<RECT> calls;
    std::vector<bool> released;
    a->set_clip_hook([&](const RECT* r) { if (r) { calls.push_back(*r); released.push_back(false); } else released.push_back(true); });
    SendMessageW(w->hwnd, WM_ACTIVATEAPP, TRUE, 0);
    CHECK_OK(a->set_cursor_clip(true));
    CHECK(calls.size() == 1);
    CHECK(calls[0].right - calls[0].left == 640 && calls[0].bottom - calls[0].top == 480);
    SendMessageW(w->hwnd, WM_ACTIVATEAPP, FALSE, 0);
    CHECK(released.back());                         // focus lost: the pointer is free again
    SendMessageW(w->hwnd, WM_ACTIVATEAPP, TRUE, 0);
    CHECK(calls.size() == 2 && !released.back());   // and confined again on return
    SendMessageW(w->hwnd, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    CHECK(released.back());                         // suspend releases it
    SendMessageW(w->hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 1);
    SendMessageW(w->hwnd, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 1);
    CHECK(calls.size() == 3);                       // unlock re-applies

    // relative mouse: raw input registered on the hidden window, pointer hidden + clipped (hooked)
    CHECK_OK(a->set_relative_mouse(true));
    CHECK(a->relative_mouse() && a->raw_input().registered() && !a->cursor_visible());
    int32_t dx = 1, dy = 1;
    CHECK(!a->take_relative_motion(dx, dy) && dx == 0 && dy == 0);
    CHECK_OK(a->set_relative_mouse(false));
    CHECK(!a->raw_input().registered() && a->cursor_visible());

    // Destroying the window while attached must not leave anything behind (no crash, state cleared).
    a->set_clip_hook(nullptr);   // the real ClipCursor(NULL) below is harmless
    DestroyWindow(w->hwnd);
    CHECK(!a->attached());
    a.reset();   // destructor after the window is gone
    ClipCursor(nullptr);
    RECT clip{}, screen = virtual_screen_rect();
    GetClipCursor(&clip);
    CHECK(clip.left == screen.left && clip.right == screen.right && clip.top == screen.top && clip.bottom == screen.bottom);
    emergency_restore_desktop();
    // The display counter is back to exactly 0 (visible, nothing owed): one more hide/show pair leaves -1/0.
    CHECK(ShowCursor(FALSE) == -1);
    CHECK(ShowCursor(TRUE) == 0);
}

static void test_attach_rules() {
    TEST_BEGIN("attach preconditions");
    Status st;
    CHECK(WindowAttachment::attach(nullptr, {}, &st) == nullptr && !st.ok);
    nuetest::HiddenWindow w(true);
    Status other;
    std::unique_ptr<WindowAttachment> from_thread;
    std::thread([&] { from_thread = WindowAttachment::attach(w.hwnd, {}, &other); }).join();   // wrong thread
    CHECK(from_thread == nullptr && !other.ok);
}

static RAWINPUT make_mouse(LONG dx, LONG dy, USHORT buttons, USHORT data = 0, USHORT flags = MOUSE_MOVE_RELATIVE) {
    RAWINPUT r{};
    r.header.dwType = RIM_TYPEMOUSE;
    r.header.dwSize = sizeof r;
    r.data.mouse.usFlags = flags;
    r.data.mouse.lLastX = dx;
    r.data.mouse.lLastY = dy;
    r.data.mouse.usButtonFlags = buttons;
    r.data.mouse.usButtonData = data;
    return r;
}
static RAWINPUT make_key(USHORT make, USHORT vkey, USHORT flags) {
    RAWINPUT r{};
    r.header.dwType = RIM_TYPEKEYBOARD;
    r.header.dwSize = sizeof r;
    r.data.keyboard.MakeCode = make;
    r.data.keyboard.VKey = vkey;
    r.data.keyboard.Flags = flags;
    return r;
}

static void test_raw_input() {
    TEST_BEGIN("raw input decoding (synthetic) and registration");
    RawInput ri;
    RawInputEvent e;
    CHECK(ri.decode(make_mouse(5, -3, RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_WHEEL, USHORT(-120)), e));
    CHECK(e.kind == RawInputEvent::Mouse && e.mouse.dx == 5 && e.mouse.dy == -3 && e.mouse.button_down == 1 && e.mouse.wheel == -120);
    CHECK(ri.decode(make_mouse(2, 2, RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_BUTTON_5_DOWN), e));
    CHECK(e.mouse.button_up == 1 && e.mouse.button_down == (1u << 4));
    int32_t dx, dy;
    ri.take_mouse_motion(dx, dy);
    CHECK(dx == 7 && dy == -1);
    ri.take_mouse_motion(dx, dy);
    CHECK(dx == 0 && dy == 0);
    // absolute devices (remote desktop): first event has no delta, then differences
    CHECK(ri.decode(make_mouse(1000, 1000, 0, 0, MOUSE_MOVE_ABSOLUTE | MOUSE_VIRTUAL_DESKTOP), e));
    CHECK(e.mouse.absolute && e.mouse.virtual_desktop && e.mouse.dx == 0);
    CHECK(ri.decode(make_mouse(1010, 995, 0, 0, MOUSE_MOVE_ABSOLUTE), e));
    CHECK(e.mouse.dx == 10 && e.mouse.dy == -5);

    CHECK(ri.decode(make_key(0x1E, 'A', 0), e));
    CHECK(e.kind == RawInputEvent::Key && e.key.vkey == 'A' && e.key.down && !e.key.repeat && e.key.scancode == 0x1E);
    CHECK(ri.decode(make_key(0x1E, 'A', 0), e) && e.key.repeat);          // already down: auto-repeat
    CHECK(ri.decode(make_key(0x1E, 'A', RI_KEY_BREAK), e) && !e.key.down);
    CHECK(ri.decode(make_key(0x1E, 'A', 0), e) && !e.key.repeat);
    CHECK(ri.decode(make_key(0x2A, VK_SHIFT, 0), e) && e.key.vkey == VK_LSHIFT);
    CHECK(ri.decode(make_key(0x1D, VK_CONTROL, RI_KEY_E0), e) && e.key.vkey == VK_RCONTROL && e.key.extended && e.key.scancode == 0xE01D);
    CHECK(ri.decode(make_key(0x38, VK_MENU, 0), e) && e.key.vkey == VK_LMENU);
    CHECK(!ri.decode(make_key(0x2A, 0xFF, 0), e) || e.kind == RawInputEvent::None);   // fake keys are dropped

    nuetest::HiddenWindow msg(false);
    RawInput live;
    RawInput::Options o;
    o.keyboard = true;
    o.mouse = true;
    CHECK_OK(live.register_devices(msg.hwnd, o));
    CHECK(live.registered());
    live.unregister_all();
    CHECK(!live.registered());

    std::vector<RawDeviceInfo> devs;
    CHECK(enumerate_raw_devices(devs));
    int mice = 0, keyboards = 0, pads = 0;
    for (const auto& d : devs) { mice += d.type == RawDeviceInfo::Mouse; keyboards += d.type == RawDeviceInfo::Keyboard; pads += d.is_gamepad_like(); }
    INFO("raw devices: %zu (mice %d, keyboards %d, gamepad-like HID %d)", devs.size(), mice, keyboards, pads);
}

static void test_xinput() {
    TEST_BEGIN("xinput");
    float x, y;
    apply_stick_deadzone(3000, 0, 7849, x, y);
    CHECK(x == 0 && y == 0);
    apply_stick_deadzone(32767, 0, 7849, x, y);
    CHECK(std::abs(x - 1.0f) < 1e-4f && y == 0);
    apply_stick_deadzone(-32768, 0, 7849, x, y);
    CHECK(x <= -1.0f + 1e-3f && x >= -1.0001f);
    apply_stick_deadzone(32767, 32767, 7849, x, y);   // diagonal never exceeds the unit circle
    CHECK(std::sqrt(x * x + y * y) <= 1.0001f);
    apply_stick_deadzone(7849 + 1, 0, 7849, x, y);
    CHECK(x > 0 && x < 0.001f);                        // continuous at the dead-zone edge
    CHECK(apply_trigger_threshold(10, 30) == 0);
    CHECK(std::abs(apply_trigger_threshold(255, 30) - 1.0f) < 1e-6f);
    CHECK(apply_trigger_threshold(142, 30) > 0.49f && apply_trigger_threshold(142, 30) < 0.51f);

    XInput xi;
    INFO("XInput available: %d (%s), Guide button via XInputGetStateEx: %d", int(xi.available()), xi.dll_name(), int(xi.uses_guide_button()));
    CHECK(xi.available());
    int connected = 0;
    for (int slot = 0; slot < XInput::MaxPads; ++slot) {
        GamepadState s;
        if (xi.poll(slot, s)) {
            ++connected;
            GamepadBattery b;
            xi.battery(slot, b);
            INFO("pad %d: packet %u buttons 0x%04X battery kind %d level %d", slot, s.packet, s.buttons, int(b.kind), int(b.level));
            CHECK_OK(xi.set_rumble(slot, 0.0f, 0.0f));   // zero strength only: the test never makes a pad vibrate
        }
    }
    CHECK(xi.set_rumble(7, 0, 0).ok == false);
    INFO("connected pads: %d", connected);
    GamepadState none;
    CHECK(!xi.poll(9, none) && !none.connected);

    const GameInputProbe gi = probe_game_input();
    INFO("GameInput: dll %d, create %d, redistributable %d -- %s", int(gi.dll_present), int(gi.create_ok), int(gi.redistributable_present), gi.note.c_str());
}

static void test_last_used() {
    TEST_BEGIN("last used device");
    LastUsedDevice d;
    CHECK(d.current() == InputDevice::None);
    CHECK(d.on_key(1000) && d.current() == InputDevice::KeyboardMouse);
    CHECK(!d.on_key(1010));
    GamepadState s;
    s.connected = true;
    s.packet = 1;
    s.any_input = false;
    CHECK(!d.on_gamepad(s, 2000));            // stick at rest: not a use
    s.packet = 2;
    s.any_input = true;
    CHECK(d.on_gamepad(s, 2000) && d.current() == InputDevice::Gamepad);
    CHECK(!d.on_mouse_motion(3, 2, 2050));     // jitter below the threshold
    CHECK(!d.on_mouse_motion(8, 8, 2100));     // over the threshold but inside the cooldown after the switch... 2100-2000 < 250
    CHECK(d.current() == InputDevice::Gamepad);
    CHECK(d.on_mouse_motion(20, 0, 2400) && d.current() == InputDevice::KeyboardMouse);
    s.packet = 2;   // same packet: repeated polling of an unchanged pad does not count
    CHECK(!d.on_gamepad(s, 3000));
    s.packet = 3;
    CHECK(d.on_gamepad(s, 3000) && d.current() == InputDevice::Gamepad);
}

int main() {
    enable_best_dpi_awareness();
    test_plan();
    test_apply_real_window();
    test_attachment();
    test_clip_and_destroy();
    test_attach_rules();
    test_raw_input();
    test_xinput();
    test_last_used();
    return finish("window_input_tests");
}
