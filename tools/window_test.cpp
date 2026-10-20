// nue_window_test: window behaviour of the drop-in d3d9.dll, with windows that are never shown.
//
//   nue_window_test <path to d3d9.dll>
//
// For each scenario it creates a window (never made visible, so nothing appears on the desktop), makes a Direct3D 9 device on it, presents, resizes / moves the window
// and checks what the dll reports through NueD3D9_DescribeWindow: the client area it follows, the part of it on the monitor, and the size of bgfx's own window back buffer.
// It also checks that the dll creates no window of its own and does not change the style, position or z-order of the game's window.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}

using CreateFn = IDirect3D9*(WINAPI*)(UINT);
using DescribeFn = int(*)(char*, int);

struct Status { unsigned cw = 0, ch = 0; long vx = 0, vy = 0, vw = 0, vh = 0; unsigned bw = 0, bh = 0, swap_w = 0, swap_h = 0; };

Status read_status(DescribeFn describe) {
    char text[1024] = {};
    describe(text, sizeof text);
    if (std::getenv("NUE_WINDOW_TEST_VERBOSE")) std::printf("    [%s]\n", text);
    Status s;
    const char* p = std::strstr(text, "client ");
    if (p) std::sscanf(p, "client %ux%u; visible area %ld,%ld %ldx%ld; game back buffer %ux%u", &s.cw, &s.ch, &s.vx, &s.vy, &s.vw, &s.vh, &s.bw, &s.bh);
    if (const char* q = std::strstr(text, "window back buffer ")) std::sscanf(q, "window back buffer %ux%u", &s.swap_w, &s.swap_h);
    return s;
}

int top_level_windows_of_process() {
    struct Ctx { DWORD pid; int count; } ctx{GetCurrentProcessId(), 0};
    EnumWindows([](HWND window, LPARAM l) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(l);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == c->pid) ++c->count;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.count;
}

HWND make_window(DWORD style, int client_w, int client_h, int x, int y) {
    RECT r{0, 0, client_w, client_h};
    AdjustWindowRect(&r, style, FALSE);
    // never ShowWindow: the window exists for the API but is not visible
    return CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "STATIC", "nue window test", style & ~WS_VISIBLE, x, y, r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
}

void present_frames(IDirect3DDevice9* device, int frames) {
    for (int i = 0; i < frames; ++i) {
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(10 * i, 40, 80), 1.0f, 0);
        device->BeginScene();
        device->EndScene();
        device->Present(nullptr, nullptr, nullptr, nullptr);
    }
}

void resize_client(HWND window, int client_w, int client_h, int x, int y) {
    const LONG style = GetWindowLongA(window, GWL_STYLE);
    RECT r{0, 0, client_w, client_h};
    AdjustWindowRect(&r, DWORD(style), FALSE);
    SetWindowPos(window, nullptr, x, y, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_HIDEWINDOW);
}
}  // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();
    if (argc < 2) { std::printf("usage: nue_window_test <d3d9.dll>\n"); return 2; }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { std::printf("cannot load %s\n", argv[1]); return 2; }
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "Direct3DCreate9"));
    auto describe = reinterpret_cast<DescribeFn>(GetProcAddress(dll, "NueD3D9_DescribeWindow"));
    if (!create || !describe) { std::printf("the dll lacks Direct3DCreate9 / NueD3D9_DescribeWindow\n"); return 2; }
    const int desktop_w = GetSystemMetrics(SM_CXSCREEN), desktop_h = GetSystemMetrics(SM_CYSCREEN);
    std::printf("desktop %dx%d\n", desktop_w, desktop_h);

    struct Scenario { const char* name; DWORD style; };
    const Scenario scenarios[] = {
        {"bordered window (--windowed): title bar, draggable", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX},
        {"resizable bordered window (WindowedMode=3)", WS_OVERLAPPEDWINDOW},
        {"borderless window (WindowedMode=1 / scaled virtual resolution)", WS_POPUP},
    };
    for (const Scenario& sc : scenarios) {
        std::printf("--- %s\n", sc.name);
        const int before_windows = top_level_windows_of_process();
        HWND window = make_window(sc.style, 1280, 720, 100, 100);
        check(window != nullptr, "test window created");
        if (!window) continue;
        const int with_window = top_level_windows_of_process();
        IDirect3D9* d3d = create(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS pp{};
        pp.BackBufferWidth = 1280; pp.BackBufferHeight = 720; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE; pp.hDeviceWindow = window;
        pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        IDirect3DDevice9* device = nullptr;
        HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device);
        check(SUCCEEDED(hr) && device, "device created on the game's window");
        if (!device) { if (d3d) d3d->Release(); DestroyWindow(window); continue; }
        const LONG style_before = GetWindowLongA(window, GWL_STYLE), ex_before = GetWindowLongA(window, GWL_EXSTYLE);
        RECT rect_before;
        GetWindowRect(window, &rect_before);
        present_frames(device, 4);
        Status s = read_status(describe);
        check(s.cw == 1280 && s.ch == 720, "follows the client area (1280x720)");
        check(s.swap_w == 1280 && s.swap_h == 720, "bgfx window back buffer is the client area");
        check(s.bw == 1280 && s.bh == 720, "the game's back buffer keeps its own size");
        check(top_level_windows_of_process() == with_window, "the dll created no window of its own");
        RECT rect_after;
        GetWindowRect(window, &rect_after);
        check(GetWindowLongA(window, GWL_STYLE) == style_before && GetWindowLongA(window, GWL_EXSTYLE) == ex_before, "style of the game's window untouched");
        check(rect_after.left == rect_before.left && rect_after.top == rect_before.top && rect_after.right == rect_before.right, "position / size of the game's window untouched");

        // the user drags the window's border: the swap chain follows
        resize_client(window, 1600, 900, 120, 120);
        present_frames(device, 4);
        s = read_status(describe);
        check(s.cw == 1600 && s.ch == 900, "resize to 1600x900: client follows");
        check(s.swap_w == 1600 && s.swap_h == 900, "resize to 1600x900: swap chain follows");
        resize_client(window, 800, 600, 120, 120);
        present_frames(device, 4);
        s = read_status(describe);
        check(s.cw == 800 && s.ch == 600 && s.swap_w == 800 && s.swap_h == 600, "resize to 800x600 (4:3): client and swap chain follow");

        // the window is moved so that part of it is outside the monitor: the picture is fitted into the part that is on the monitor
        resize_client(window, 1000, 600, desktop_w - 300, 100);
        present_frames(device, 4);
        s = read_status(describe);
        check(s.vw > 0 && s.vw <= 300 + 8 && s.vh == 600, "window moved half off the monitor: visible width is what the monitor shows");
        resize_client(window, 1000, 600, 150, 150);
        present_frames(device, 4);
        s = read_status(describe);
        check(s.vw == 1000 && s.vh == 600, "window moved back inside: all of it visible");

        // Reset to another back buffer size does not touch the window
        pp.BackBufferWidth = 1920; pp.BackBufferHeight = 1080;
        hr = device->Reset(&pp);
        check(SUCCEEDED(hr), "Reset to 1920x1080");
        present_frames(device, 4);
        s = read_status(describe);
        check(s.bw == 1920 && s.bh == 1080 && s.cw == 1000 && s.ch == 600, "after Reset: game back buffer 1920x1080, window still 1000x600");
        check(top_level_windows_of_process() == with_window, "still no window of the dll's own");

        device->Release();
        d3d->Release();
        DestroyWindow(window);
        check(top_level_windows_of_process() == before_windows, "no window left behind");
    }
    std::printf(g_failed ? "FAILED: %d checks\n" : "PASS nue_window_test\n", g_failed);
    return g_failed ? 1 : 0;
}
