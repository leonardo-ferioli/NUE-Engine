// Hidden window helper for tests: never shown, never activated, destroyed at scope end.
#pragma once
#include "nue_win/common.hpp"

namespace nuetest {

inline LRESULT CALLBACK default_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

struct HiddenWindow {
    HWND hwnd = nullptr;
    // `overlapped`: a normal captioned window (hidden); otherwise a message-only window.
    explicit HiddenWindow(bool overlapped = true, int client_w = 800, int client_h = 600) {
        static const wchar_t* kClass = L"nue_win_test_window";
        static bool registered = [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = default_wndproc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kClass;
            return RegisterClassExW(&wc) != 0;
        }();
        (void)registered;
        if (overlapped) {
            RECT r{0, 0, client_w, client_h};
            AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
            // No WS_VISIBLE: the window exists for the API but never appears on the desktop.
            hwnd = CreateWindowExW(WS_EX_NOACTIVATE, kClass, L"nue test (hidden)", WS_OVERLAPPEDWINDOW, 100, 100, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        } else {
            hwnd = CreateWindowExW(0, kClass, L"nue test (message only)", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        }
    }
    ~HiddenWindow() { if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd); }
    HiddenWindow(const HiddenWindow&) = delete;
    HiddenWindow& operator=(const HiddenWindow&) = delete;
};

}  // namespace nuetest
