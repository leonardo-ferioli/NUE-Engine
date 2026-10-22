#include "nue_win/window.hpp"

#include <commctrl.h>
#include <wtsapi32.h>

#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace nue::win {

const char* window_mode_name(WindowMode m) {
    switch (m) {
        case WindowMode::Windowed: return "windowed";
        case WindowMode::Borderless: return "borderless";
        default: return "exclusive";
    }
}

DWORD borderless_style(DWORD s) {
    return (s & ~DWORD(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME | WS_CHILD)) | WS_POPUP;
}
DWORD borderless_ex_style(DWORD e) {
    return e & ~DWORD(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_TOOLWINDOW);
}

namespace {

SIZE frame_overhead(DWORD style, DWORD ex_style, bool menu, uint32_t dpi) {
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const Fn adjust = load_proc<Fn>(L"user32.dll", "AdjustWindowRectExForDpi");
    RECT r{0, 0, 100, 100};
    if (adjust) adjust(&r, style, menu, ex_style, dpi);
    else AdjustWindowRectEx(&r, style, menu, ex_style);
    return SIZE{(r.right - r.left) - 100, (r.bottom - r.top) - 100};
}

WindowSnapshot snapshot_of(HWND hwnd) {
    WindowSnapshot s;
    s.style = DWORD(GetWindowLongPtrW(hwnd, GWL_STYLE));
    s.ex_style = DWORD(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    s.has_menu = GetMenu(hwnd) != nullptr && !(s.style & WS_CHILD);
    GetWindowRect(hwnd, &s.window_rect);
    RECT c{};
    GetClientRect(hwnd, &c);
    s.client_size = SIZE{c.right - c.left, c.bottom - c.top};
    s.visible = IsWindowVisible(hwnd) != FALSE;
    return s;
}

MonitorGeometry geometry_for(HWND hwnd, HMONITOR monitor) {
    MonitorGeometry g;
    HMONITOR h = monitor ? monitor : MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (h && GetMonitorInfoW(h, &mi)) { g.monitor = mi.rcMonitor; g.work = mi.rcWork; g.dpi = monitor_dpi(h); }
    else { g.monitor = RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)}; g.work = g.monitor; }
    return g;
}

}  // namespace

WindowPlan plan_window_mode(const WindowSnapshot& original, const WindowSnapshot& current, const MonitorGeometry& mon, const WindowModeRequest& req) {
    WindowPlan p;
    p.mode = req.mode;
    const DWORD visible = current.style & WS_VISIBLE;
    const int mon_w = mon.monitor.right - mon.monitor.left, mon_h = mon.monitor.bottom - mon.monitor.top;

    if (req.mode != WindowMode::Windowed) {
        p.style = (borderless_style(original.style) & ~DWORD(WS_VISIBLE)) | visible;
        p.ex_style = borderless_ex_style(original.ex_style);
        p.window_rect = mon.monitor;
        p.client_size = SIZE{mon_w, mon_h};
        p.topmost = req.mode == WindowMode::Exclusive;
        p.covers_monitor = true;
    } else {
        p.style = (original.style & ~DWORD(WS_VISIBLE)) | visible;
        p.ex_style = original.ex_style;
        const SIZE frame = frame_overhead(p.style, p.ex_style, original.has_menu, mon.dpi);
        const int work_w = mon.work.right - mon.work.left, work_h = mon.work.bottom - mon.work.top;
        const int avail_w = std::max(1, work_w - int(frame.cx)), avail_h = std::max(1, work_h - int(frame.cy));

        uint32_t cw = req.client_width ? req.client_width : uint32_t(std::max<LONG>(1, original.client_size.cx));
        uint32_t ch = req.client_height ? req.client_height : uint32_t(std::max<LONG>(1, original.client_size.cy));
        if (req.fit_to_work_area) {
            const uint32_t aw = req.content_width ? req.content_width : cw, ah = req.content_height ? req.content_height : ch;
            const FitRect f = fit_aspect(aw, ah, 0, 0, uint32_t(avail_w), uint32_t(avail_h));
            cw = f.width;
            ch = f.height;
        } else if (req.clamp_to_work_area && (cw > uint32_t(avail_w) || ch > uint32_t(avail_h))) {
            const FitRect f = fit_aspect(cw, ch, 0, 0, uint32_t(avail_w), uint32_t(avail_h));
            cw = f.width;
            ch = f.height;
        }
        p.client_size = SIZE{LONG(cw), LONG(ch)};
        const int outer_w = int(cw) + frame.cx, outer_h = int(ch) + frame.cy;
        int x, y;
        if (req.center) {
            x = mon.work.left + (work_w - outer_w) / 2;
            y = mon.work.top + (work_h - outer_h) / 2;
        } else {
            x = std::clamp<int>(original.window_rect.left, mon.work.left, std::max<int>(mon.work.left, mon.work.right - outer_w));
            y = std::clamp<int>(original.window_rect.top, mon.work.top, std::max<int>(mon.work.top, mon.work.bottom - outer_h));
        }
        p.window_rect = RECT{x, y, x + outer_w, y + outer_h};
        p.topmost = false;
        p.covers_monitor = false;
    }
    if (req.content_width && req.content_height) p.content = fit_aspect(req.content_width, req.content_height, 0, 0, uint32_t(p.client_size.cx), uint32_t(p.client_size.cy));
    return p;
}

Status apply_window_plan(HWND hwnd, const WindowPlan& p) {
    if (!IsWindow(hwnd)) return Status::fail("not a window", ERROR_INVALID_WINDOW_HANDLE);
    if (IsWindowVisible(hwnd) && (IsIconic(hwnd) || IsZoomed(hwnd))) ShowWindow(hwnd, SW_RESTORE);
    SetLastError(0);
    SetWindowLongPtrW(hwnd, GWL_STYLE, LONG_PTR(p.style));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, LONG_PTR(p.ex_style));
    if (!SetWindowPos(hwnd, p.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, p.window_rect.left, p.window_rect.top, p.window_rect.right - p.window_rect.left,
                      p.window_rect.bottom - p.window_rect.top, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER))
        return Status::last_error("SetWindowPos");
    return Status::success();
}

Status WindowModeController::plan(HWND hwnd, const WindowModeRequest& req, WindowPlan& out) const {
    if (!IsWindow(hwnd)) return Status::fail("not a window", ERROR_INVALID_WINDOW_HANDLE);
    const WindowSnapshot current = snapshot_of(hwnd);
    const WindowSnapshot& original = (mode_ != WindowMode::Windowed && have_windowed_) ? windowed_ : current;
    out = plan_window_mode(original, current, geometry_for(hwnd, req.monitor), req);
    return Status::success();
}

Status WindowModeController::set_mode(HWND hwnd, const WindowModeRequest& req, WindowPlan* applied) {
    WindowPlan p;
    if (Status s = plan(hwnd, req, p); !s.ok) return s;
    if (mode_ == WindowMode::Windowed && req.mode != WindowMode::Windowed) { windowed_ = snapshot_of(hwnd); have_windowed_ = true; }
    if (Status s = apply_window_plan(hwnd, p); !s.ok) return s;
    mode_ = req.mode;
    if (req.mode == WindowMode::Windowed) have_windowed_ = false;
    if (applied) *applied = p;
    return Status::success();
}

// ---- WindowAttachment ------------------------------------------------------------------------------------------------------------------

namespace {
constexpr UINT_PTR kSubclassId = 0x4E554557;   // "NUEW"
}

std::unique_ptr<WindowAttachment> WindowAttachment::attach(HWND hwnd, WindowCallbacks cb, Status* status) {
    auto fail = [&](Status s) -> std::unique_ptr<WindowAttachment> { if (status) *status = s; return nullptr; };
    if (!IsWindow(hwnd)) return fail(Status::fail("not a window", ERROR_INVALID_WINDOW_HANDLE));
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return fail(Status::fail("attach must run on the window's thread", ERROR_ACCESS_DENIED));
    DWORD_PTR existing = 0;
    if (GetWindowSubclass(hwnd, subclass_proc, kSubclassId, &existing)) return fail(Status::fail("window already has an attachment", ERROR_ALREADY_EXISTS));
    std::unique_ptr<WindowAttachment> a(new WindowAttachment);
    a->hwnd_ = hwnd;
    a->cb_ = std::move(cb);
    a->dpi_ = window_dpi(hwnd);
    a->focused_ = GetForegroundWindow() == hwnd || GetActiveWindow() == hwnd;
    if (!SetWindowSubclass(hwnd, subclass_proc, kSubclassId, reinterpret_cast<DWORD_PTR>(a.get()))) return fail(Status::last_error("SetWindowSubclass"));
    a->session_registered_ = WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (status) *status = Status::success();
    return a;
}

WindowAttachment::~WindowAttachment() { detach(); }

void WindowAttachment::detach() {
    if (!hwnd_) return;
    HWND h = hwnd_;
    release_clip();
    raw_.unregister_all();
    hwnd_ = nullptr;
    if (IsWindow(h)) {
        if (session_registered_) WTSUnRegisterSessionNotification(h);
        RemoveWindowSubclass(h, subclass_proc, kSubclassId);
    }
    session_registered_ = false;
}

void WindowAttachment::set_cursor_visible(bool v) {
    cursor_visible_ = v;
    // Apply immediately if the pointer is over our client area (otherwise the next WM_SETCURSOR does it).
    if (hwnd_ && !v) {
        POINT pt;
        if (GetCursorPos(&pt) && WindowFromPoint(pt) == hwnd_) SendMessageW(hwnd_, WM_SETCURSOR, WPARAM(hwnd_), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
}

void WindowAttachment::apply_clip() {
    if (!hwnd_ || !clip_wanted_ || !focused_ || IsIconic(hwnd_)) { release_clip(); return; }
    RECT c;
    if (!GetClientRect(hwnd_, &c)) return;
    POINT tl{c.left, c.top}, br{c.right, c.bottom};
    ClientToScreen(hwnd_, &tl);
    ClientToScreen(hwnd_, &br);
    RECT r{tl.x, tl.y, br.x, br.y};
    if (clip_hook_) clip_hook_(&r); else ClipCursor(&r);
    clip_active_ = true;
}

void WindowAttachment::release_clip() {
    if (!clip_active_) return;
    if (clip_hook_) clip_hook_(nullptr); else ClipCursor(nullptr);
    clip_active_ = false;
}

Status WindowAttachment::set_cursor_clip(bool clip) {
    if (!hwnd_) return Status::fail("not attached", ERROR_INVALID_STATE);
    clip_wanted_ = clip;
    if (clip) apply_clip(); else release_clip();
    return Status::success();
}

Status WindowAttachment::set_relative_mouse(bool enabled) {
    if (!hwnd_) return Status::fail("not attached", ERROR_INVALID_STATE);
    if (enabled == relative_) return Status::success();
    if (enabled) {
        RawInput::Options o;
        o.mouse = true;
        o.keyboard = false;
        if (Status s = raw_.register_devices(hwnd_, o); !s.ok) return s;
        relative_ = true;
        set_cursor_visible(false);
        return set_cursor_clip(true);
    }
    relative_ = false;
    raw_.unregister_all();
    set_cursor_visible(true);
    return set_cursor_clip(false);
}

bool WindowAttachment::take_relative_motion(int32_t& dx, int32_t& dy) {
    raw_.take_mouse_motion(dx, dy);
    return dx != 0 || dy != 0;
}

LRESULT CALLBACK WindowAttachment::subclass_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    auto* self = reinterpret_cast<WindowAttachment*>(ref);
    if (id == kSubclassId && self && self->hwnd_ == hwnd) {
        bool pass_on = true;
        LRESULT r = self->handle(hwnd, msg, wp, lp, pass_on);
        if (!pass_on) return r;
    }
    if (msg == WM_NCDESTROY) {
        if (self && self->hwnd_ == hwnd) { self->release_clip(); self->raw_.unregister_all(); self->hwnd_ = nullptr; }
        RemoveWindowSubclass(hwnd, subclass_proc, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT WindowAttachment::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& pass_on) {
    switch (msg) {
        case WM_SETCURSOR:
            if (!cursor_visible_ && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); pass_on = false; return TRUE; }
            break;
        case WM_ACTIVATEAPP: case WM_ACTIVATE: case WM_SETFOCUS: case WM_KILLFOCUS: {
            bool now;
            if (msg == WM_ACTIVATEAPP) now = wp != FALSE;
            else if (msg == WM_ACTIVATE) now = LOWORD(wp) != WA_INACTIVE;
            else now = msg == WM_SETFOCUS;
            if (msg == WM_ACTIVATE && HIWORD(wp) != 0) now = false;   // minimised
            if (now != focused_) {
                focused_ = now;
                if (focused_) apply_clip(); else release_clip();
                if (cb_.on_focus) cb_.on_focus(focused_);
            }
            break;
        }
        case WM_SIZE:
            apply_clip();
            if (cb_.on_size) cb_.on_size(LOWORD(lp), HIWORD(lp), wp == SIZE_MINIMIZED);
            break;
        case WM_MOVE: apply_clip(); break;
        case WM_DPICHANGED:
            dpi_ = HIWORD(wp);
            if (cb_.on_dpi_changed && lp) cb_.on_dpi_changed(dpi_, *reinterpret_cast<const RECT*>(lp));
            break;
        case WM_DISPLAYCHANGE:
            apply_clip();
            if (cb_.on_display_change) cb_.on_display_change(LOWORD(lp), HIWORD(lp), uint32_t(wp));
            break;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMSUSPEND) { resumed_pending_ = true; release_clip(); if (cb_.on_suspend) cb_.on_suspend(); }
            else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                // Windows sends RESUMEAUTOMATIC first and RESUMESUSPEND only if the user was present: report once per suspend.
                if (resumed_pending_) { resumed_pending_ = false; if (cb_.on_resume) cb_.on_resume(); }
            }
            break;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK) { release_clip(); if (cb_.on_session_lock) cb_.on_session_lock(true); }
            else if (wp == WTS_SESSION_UNLOCK) { apply_clip(); if (cb_.on_session_lock) cb_.on_session_lock(false); }
            break;
        case WM_DEVICECHANGE:
            if (cb_.on_device_change) cb_.on_device_change();
            break;
        case WM_INPUT:
            if (raw_.registered()) {
                RawInputEvent e;
                if (raw_.handle_wm_input(lp, e) && cb_.on_raw_input) cb_.on_raw_input(e);
            }
            break;
        case WM_CLOSE:
            if (cb_.on_close_request && !cb_.on_close_request()) { pass_on = false; return 0; }
            break;
        default: break;
    }
    (void)hwnd;
    return 0;
}

void emergency_restore_desktop() {
    ClipCursor(nullptr);
    CURSORINFO ci{};
    ci.cbSize = sizeof ci;
    for (int i = 0; i < 32 && GetCursorInfo(&ci) && !(ci.flags & CURSOR_SHOWING); ++i) {
        ShowCursor(TRUE);
        ci.cbSize = sizeof ci;
    }
    restore_all_changed_display_modes();
}

}  // namespace nue::win
