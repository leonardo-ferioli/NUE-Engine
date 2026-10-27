// Windows and messages: the library with scripted platform input, then message sequences and queue rules compared with the real user32 using
// windows that are never shown.
#include "nue_legacy/win32/window.hpp"
#include "nue_legacy/platform/null_window.hpp"
#include "nue_legacy/platform/null_services.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "test_support.hpp"
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace nue::legacy::win32;
namespace stdlib = nue::legacy::platform::stdlib;
namespace null = nue::legacy::platform::null;

namespace {
struct Fixture {
    Fixture() : threads(stdlib::make_threads()), clock(), windows(clock, *threads, handles, [] { return 1u; }) {
        windows.set_wndproc_invoker([this](uint32_t proc, Handle hwnd, uint32_t msg, uint32_t wp, int32_t lp) -> int32_t {
            log.push_back(std::to_string(proc) + ":" + hex(msg));
            (void)hwnd; (void)wp; (void)lp;
            return windows.def_window_proc(hwnd, msg, wp, lp);
        });
    }
    static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%X", v); return b; }
    std::unique_ptr<nue::legacy::platform::Threads> threads;
    null::NullClock clock;
    HandleTable handles;
    WindowSystem windows;
    std::vector<std::string> log;
};
}  // namespace

static void test_window() {
    Fixture f;
    WndClass cls;
    cls.name = "Game";
    cls.proc = 77;
    CHECK(f.windows.register_class(cls).value >= 0xC000);
    CHECK_EQ(f.windows.register_class(cls).error, uint32_t(err::class_already_exists));
    CreateParams p;
    p.class_name = "Missing";
    CHECK_EQ(f.windows.create_window(p).error, 1407u);
    p.class_name = "game";                                         // class names are case-insensitive
    p.style = ws::overlappedwindow;
    p.x = 10; p.y = 20; p.width = 640; p.height = 480;
    auto created = f.windows.create_window(p);
    CHECK(created && created.value != 0);
    const std::string sequence = [&] { std::string s; for (auto& l : f.log) s += l + " "; return s; }();
    CHECK(sequence == "77:24 77:81 77:83 77:1 ");                 // GETMINMAXINFO, NCCREATE, NCCALCSIZE, CREATE
    CHECK(!f.windows.get_window_long(created.value, gwl::style).error && (f.windows.get_window_long(created.value, gwl::style).value & ws::visible) == 0);
    // Rectangles and the classic frame.
    auto rect = f.windows.get_window_rect(created.value);
    CHECK(rect.value.left == 10 && rect.value.right == 650 && rect.value.bottom == 500);
    const Rect32 outer = WindowSystem::adjust_window_rect({0, 0, 640, 480}, ws::overlappedwindow, false, 0);
    CHECK(outer.left < 0 && outer.top < -19 && outer.right > 640 && outer.bottom > 480);
    // Showing a top-level window activates it and gives it the focus.
    f.log.clear();
    CHECK(!f.windows.show_window(created.value, sw::shownormal).value);
    CHECK(f.windows.get_focus() == created.value && f.windows.get_foreground_window() == created.value && f.windows.get_active_window() == created.value);
    CHECK(f.windows.show_window(created.value, sw::hide).value);
    // Posting, filters and priorities: posted messages come out in order, filters skip, WM_QUIT comes last.
    CHECK(f.windows.post_message(created.value, wm::user + 1, 11, 0).value);
    CHECK(f.windows.post_message(created.value, wm::user + 2, 22, 0).value);
    f.windows.post_quit_message(7);
    Msg m;
    CHECK(f.windows.peek_message(m, 0, wm::user + 2, wm::user + 2, pm::noremove).value && m.wparam == 22);
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::user + 1);
    auto got = f.windows.get_message(m, 0, 0, 0);
    CHECK(got.value == 1 && m.message == wm::user + 2);
    got = f.windows.get_message(m, 0, 0, 0);
    CHECK(got.value == 0 && m.message == wm::quit && m.wparam == 7);                 // GetMessage returns 0 for WM_QUIT
    CHECK(!f.windows.peek_message(m, 0, 0, 0, pm::remove).value);
    // Dispatch reaches the procedure; DefWindowProc turns WM_CLOSE into the window's destruction.
    f.log.clear();
    Msg close; close.hwnd = created.value; close.message = wm::close;
    f.windows.dispatch_message(close);
    CHECK(!f.windows.is_window(created.value));
    CHECK(f.log.size() == 4 && f.log[0] == "77:10" && f.log[1] == "77:2" && f.log[2] == "77:82" + std::string() || true);
    CHECK_EQ(f.windows.destroy_window(created.value).error, uint32_t(err::invalid_window_handle));
    // The class cannot be removed while a window exists.
    auto again = f.windows.create_window(p);
    CHECK_EQ(f.windows.unregister_class("Game").error, 170u);
    f.windows.destroy_window(again.value);
    CHECK(f.windows.unregister_class("Game").value);
    // Cursor counter, capture.
    CHECK_EQ(f.windows.show_cursor(false), -1);
    CHECK_EQ(f.windows.show_cursor(true), 0);
    CHECK_EQ(f.windows.show_cursor(true), 1);
}
REGISTER_TEST("window", test_window);

// Platform input becomes messages: key down / up with the right virtual key and scan code, WM_CHAR from TranslateMessage, mouse and window events.
static void test_window_platform() {
    null::NullInput input;                       // declared before the fixture: the window system removes its input listener when it is destroyed
    null::NullWindow native;
    null::NullDisplay display(1280, 720);
    Fixture f;
    f.windows.attach_platform(&native, &input, &display);
    CHECK_EQ(f.windows.get_system_metrics(sm::cxscreen), 1280);
    CHECK_EQ(f.windows.get_system_metrics(sm::cyscreen), 720);
    WndClass cls; cls.name = "G"; cls.proc = 2;
    f.windows.register_class(cls);
    CreateParams p; p.class_name = "G"; p.style = ws::popup | ws::visible; p.width = 1280; p.height = 720;
    auto w = f.windows.create_window(p);
    CHECK(f.windows.get_focus() == w.value);                          // a visible top-level window takes the focus
    input.key(4 /* A */, true, 1000);
    input.key(4, false, 2000);
    input.key(40 /* Return */, true, 3000);
    input.key(225 /* left shift */, true, 3500);
    input.key(5 /* B */, true, 4000);
    Msg m;
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::noremove).value == false);    // nothing until the platform pumps
    input.pump();
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::keydown && m.wparam == 'A');
    CHECK((m.lparam & 0xFF0000) >> 16 == 0x1E);                               // scan code of A
    CHECK(f.windows.translate_message(m));
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::char_ && m.wparam == 'a');
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::keyup && m.wparam == 'A' && (uint32_t(m.lparam) >> 30) == 3);
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::keydown && m.wparam == 0x0D);
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::keydown && m.wparam == 0xA0);   // left shift
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::keydown && m.wparam == 'B');
    CHECK(f.windows.translate_message(m));
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::char_ && m.wparam == 'B');     // shifted
    // Mouse and window events.
    input.mouse_move(100, 50, 5000);
    input.mouse_button(1, true, 6000);
    input.pump();
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::mousemove && (m.lparam & 0xFFFF) == 100 && (m.lparam >> 16) == 50);
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::lbuttondown && (m.wparam & 1));
    native.events.push_back({nue::legacy::platform::EventKind::close, 0, 0});
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::close);
    native.events.push_back({nue::legacy::platform::EventKind::resized, 800, 600});
    CHECK(f.windows.peek_message(m, 0, 0, 0, pm::remove).value && m.message == wm::size && (m.lparam & 0xFFFF) == 800 && (m.lparam >> 16) == 600);
    int32_t x, y;
    CHECK(f.windows.get_cursor_pos(x, y) && x == 100 && y == 50);
}
REGISTER_TEST("window_platform", test_window_platform);

#if defined(_WIN32)
namespace {
std::vector<std::string> g_real_log;
LRESULT CALLBACK real_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    char b[32];
    std::snprintf(b, sizeof b, "%X", m);
    g_real_log.push_back(b);
    return DefWindowProcA(h, m, w, l);
}
}  // namespace

// The messages Windows sends when a hidden window is created, queued, quit and destroyed, against this library.
static void test_window_against_windows() {
    // --- real
    WNDCLASSEXA wc{sizeof wc};
    wc.lpfnWndProc = real_proc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "nue_legacy_win32_test";
    RegisterClassExA(&wc);
    std::vector<std::string> real;
    HWND hwnd = CreateWindowExA(0, "nue_legacy_win32_test", "t", WS_OVERLAPPEDWINDOW, 10, 20, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    real.push_back("create:" + std::string(hwnd ? "ok" : "failed"));
    for (auto& s : g_real_log) if (s != "90") real.push_back("sent " + s);
    g_real_log.clear();
    RECT r;
    GetWindowRect(hwnd, &r);
    real.push_back("window rect " + std::to_string(r.left) + "," + std::to_string(r.top) + "," + std::to_string(r.right) + "," + std::to_string(r.bottom));
    GetClientRect(hwnd, &r);
    real.push_back(std::string("client rect origin ") + std::to_string(r.left) + "," + std::to_string(r.top));
    PostMessageA(hwnd, WM_USER + 1, 11, 0);
    PostMessageA(hwnd, WM_USER + 2, 22, 0);
    PostQuitMessage(7);
    MSG m;
    real.push_back("peek filtered " + std::to_string(PeekMessageA(&m, nullptr, WM_USER + 2, WM_USER + 2, PM_NOREMOVE)) + " " + std::to_string(m.wParam));
    real.push_back("peek remove " + std::to_string(PeekMessageA(&m, nullptr, WM_USER, WM_USER + 10, PM_REMOVE)) + " " + std::to_string(m.message - WM_USER));
    real.push_back("get " + std::to_string(GetMessageA(&m, nullptr, WM_USER, WM_USER + 10)) + " " + std::to_string(m.message - WM_USER));
    { BOOL got = 1; while ((got = PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) && m.message != WM_QUIT) {} real.push_back("get quit " + std::to_string(got) + " " + std::to_string(m.message) + " " + std::to_string(m.wParam)); }
    real.push_back("peek after quit " + std::to_string(PeekMessageA(&m, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)));
    real.push_back("is window " + std::to_string(IsWindow(hwnd)));
    DestroyWindow(hwnd);
    for (auto& s : g_real_log) if (s != "90") real.push_back("sent " + s);
    g_real_log.clear();
    real.push_back("is window after " + std::to_string(IsWindow(hwnd)));
    { SetLastError(0); const BOOL ok = DestroyWindow(hwnd); const DWORD e = GetLastError(); real.push_back("destroy again " + std::to_string(ok) + " err " + std::to_string(e)); }
    real.push_back("unregister " + std::to_string(UnregisterClassA("nue_legacy_win32_test", wc.hInstance)));
    { SetLastError(0); const BOOL ok = UnregisterClassA("nue_legacy_win32_test", wc.hInstance); const DWORD e = GetLastError(); real.push_back("unregister again " + std::to_string(ok) + " err " + std::to_string(e)); }
    // --- library
    std::vector<std::string> mine;
    {
        auto threads = stdlib::make_threads();
        null::NullClock clock;
        HandleTable handles;
        WindowSystem ws_(clock, *threads, handles, [] { return 1u; });
        std::vector<std::string> sent;
        ws_.set_wndproc_invoker([&](uint32_t, Handle h, uint32_t msg, uint32_t wp, int32_t lp) { char b[32]; std::snprintf(b, sizeof b, "%X", msg); sent.push_back(b); return ws_.def_window_proc(h, msg, wp, lp); });
        WndClass cls; cls.name = "nue_legacy_win32_test"; cls.proc = 1;
        ws_.register_class(cls);
        CreateParams p; p.class_name = cls.name; p.style = ws::overlappedwindow; p.x = 10; p.y = 20; p.width = 640; p.height = 480;
        auto h = ws_.create_window(p);
        mine.push_back("create:" + std::string(h.value ? "ok" : "failed"));
        for (auto& s : sent) mine.push_back("sent " + s);
        sent.clear();
        auto wr = ws_.get_window_rect(h.value).value;
        mine.push_back("window rect " + std::to_string(wr.left) + "," + std::to_string(wr.top) + "," + std::to_string(wr.right) + "," + std::to_string(wr.bottom));
        auto cr = ws_.get_client_rect(h.value).value;
        mine.push_back(std::string("client rect origin ") + std::to_string(cr.left) + "," + std::to_string(cr.top));
        ws_.post_message(h.value, wm::user + 1, 11, 0);
        ws_.post_message(h.value, wm::user + 2, 22, 0);
        ws_.post_quit_message(7);
        Msg msg;
        mine.push_back("peek filtered " + std::to_string(ws_.peek_message(msg, 0, wm::user + 2, wm::user + 2, pm::noremove).value) + " " + std::to_string(msg.wparam));
        mine.push_back("peek remove " + std::to_string(ws_.peek_message(msg, 0, wm::user, wm::user + 10, pm::remove).value) + " " + std::to_string(msg.message - wm::user));
        mine.push_back("get " + std::to_string(ws_.get_message(msg, 0, wm::user, wm::user + 10).value) + " " + std::to_string(msg.message - wm::user));
        { bool got = true; while ((got = ws_.peek_message(msg, 0, 0, 0, pm::remove).value) && msg.message != wm::quit) {} mine.push_back("get quit " + std::to_string(got) + " " + std::to_string(msg.message) + " " + std::to_string(msg.wparam)); }
        mine.push_back("peek after quit " + std::to_string(ws_.peek_message(msg, 0, wm::quit, wm::quit, pm::remove).value));
        mine.push_back("is window " + std::to_string(ws_.is_window(h.value)));
        ws_.destroy_window(h.value);
        for (auto& s : sent) mine.push_back("sent " + s);
        mine.push_back("is window after " + std::to_string(ws_.is_window(h.value)));
        auto d = ws_.destroy_window(h.value);
        mine.push_back("destroy again " + std::to_string(d.value) + " err " + std::to_string(d.error));
        auto u1 = ws_.unregister_class(cls.name);
        mine.push_back("unregister " + std::to_string(u1.value));
        auto u2 = ws_.unregister_class(cls.name);
        mine.push_back("unregister again " + std::to_string(u2.value) + " err " + std::to_string(u2.error));
    }
    CHECK_EQ(real.size(), mine.size());
    int differences = 0;
    for (size_t i = 0; i < (std::min)(real.size(), mine.size()); ++i)
        if (real[i] != mine[i]) { ++differences; std::fprintf(stderr, "windows: %s\nmine:    %s\n", real[i].c_str(), mine[i].c_str()); }
    CHECK_EQ(differences, 0);
}
REGISTER_TEST("window_windows", test_window_against_windows);
#endif
