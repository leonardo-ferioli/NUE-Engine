#include "nue_legacy/win32/window.hpp"
#include "nue_legacy/platform/storage_path.hpp"
#include <algorithm>
#include <cstring>

namespace nue::legacy::win32 {
namespace {

constexpr uint32_t kErrorCannotFindWndClass = 1407, kErrorBusy = 170, kCwUseDefault = 0x80000000u;
constexpr uint32_t kBroadcast = 0xFFFF;

struct ClassObject {
    WndClass desc;
    uint16_t atom = 0;
    int windows = 0;
};

struct WindowObject : Object {
    WindowObject() : Object(ObjectKind::other) {}
    std::shared_ptr<ClassObject> cls;
    uint32_t ex_style = 0, style = 0;
    Rect32 rect;                              // window rectangle in screen coordinates
    std::string title;
    Handle parent = 0;
    uint32_t thread = 0;                      // the thread that created it: it receives the window's messages
    uint32_t proc = 0;
    bool visible = false, minimized = false, destroyed = false, needs_paint = false;
    std::map<int32_t, int32_t> longs;         // GWL_USERDATA, GWL_ID, class extra...
    std::vector<Handle> children;
    uint32_t instance = 0;
};

struct Queue {
    std::deque<Msg> posted;
    bool quit = false;
    int32_t exit_code = 0;
};

// HID usage -> virtual key (US layout letters, digits, function keys, arrows, modifiers, the rest of the main block).
uint32_t virtual_key_of(uint32_t hid) {
    if (hid >= 4 && hid <= 29) return 'A' + (hid - 4);
    if (hid >= 30 && hid <= 38) return '1' + (hid - 30);
    if (hid == 39) return '0';
    if (hid >= 58 && hid <= 69) return 0x70 + (hid - 58);          // F1..F12
    if (hid >= 104 && hid <= 115) return 0x7C + (hid - 104);       // F13..F24
    switch (hid) {
    case 40: return 0x0D; case 41: return 0x1B; case 42: return 0x08; case 43: return 0x09; case 44: return 0x20;
    case 45: return 0xBD; case 46: return 0xBB; case 47: return 0xDB; case 48: return 0xDD; case 49: return 0xDC; case 51: return 0xBA; case 52: return 0xDE; case 53: return 0xC0; case 54: return 0xBC; case 55: return 0xBE; case 56: return 0xBF;
    case 57: return 0x14; case 70: return 0x2C; case 71: return 0x91; case 72: return 0x13; case 73: return 0x2D; case 74: return 0x24; case 75: return 0x21; case 76: return 0x2E; case 77: return 0x23; case 78: return 0x22;
    case 79: return 0x27; case 80: return 0x25; case 81: return 0x28; case 82: return 0x26; case 83: return 0x90;
    case 84: return 0x6F; case 85: return 0x6A; case 86: return 0x6D; case 87: return 0x6B; case 88: return 0x0D;
    case 89: case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97: return 0x61 + (hid - 89);
    case 98: return 0x60; case 99: return 0x6E;
    case 224: return 0xA2; case 225: return 0xA0; case 226: return 0xA4; case 227: return 0x5B; case 228: return 0xA3; case 229: return 0xA1; case 230: return 0xA5; case 231: return 0x5C;
    default: return 0;
    }
}
bool is_extended_key(uint32_t hid) { return hid == 228 || hid == 230 || (hid >= 73 && hid <= 82 && hid != 72 && hid != 71) || hid == 84 || hid == 88 || hid == 227 || hid == 231; }
// The AT scan code (set 1) of a HID usage, for the lParam of key messages.
uint32_t scan_code_of(uint32_t hid) {
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (hid >= 4 && hid <= 29) return letters[hid - 4];
    if (hid >= 30 && hid <= 38) return 0x02 + (hid - 30);
    if (hid == 39) return 0x0B;
    if (hid >= 58 && hid <= 67) return 0x3B + (hid - 58);
    switch (hid) { case 40: return 0x1C; case 41: return 0x01; case 42: return 0x0E; case 43: return 0x0F; case 44: return 0x39; case 224: return 0x1D; case 225: return 0x2A; case 226: return 0x38; case 229: return 0x36; case 68: return 0x57; case 69: return 0x58;
    case 82: return 0x48; case 81: return 0x50; case 80: return 0x4B; case 79: return 0x4D; default: return 0; }
}
// The character a key makes on a US keyboard (0 for none).
char32_t character_of(uint32_t vk, bool shift, bool caps) {
    if (vk >= 'A' && vk <= 'Z') { const bool upper = shift != caps; return upper ? vk : vk + 32; }
    if (vk >= '0' && vk <= '9') { static const char shifted[] = ")!@#$%^&*("; return shift ? shifted[vk - '0'] : vk; }
    switch (vk) {
    case 0x20: return ' '; case 0x0D: return '\r'; case 0x09: return '\t'; case 0x08: return '\b'; case 0x1B: return 0x1B;
    case 0xBA: return shift ? ':' : ';'; case 0xBB: return shift ? '+' : '='; case 0xBC: return shift ? '<' : ','; case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.'; case 0xBF: return shift ? '?' : '/'; case 0xC0: return shift ? '~' : '`'; case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\'; case 0xDD: return shift ? '}' : ']'; case 0xDE: return shift ? '"' : '\'';
    default: return 0;
    }
}

}  // namespace

uint32_t virtual_key_from_hid(uint32_t hid) { return virtual_key_of(hid); }

struct WindowSystem::Impl {
    Impl(platform::Clock& c, platform::Threads& t, HandleTable& h, std::function<uint32_t()> tid) : clock(c), threads(t), handles(h), current_thread(std::move(tid)) {
        mutex = threads.mutex_create(true).value;     // recursive: a window procedure called under the lock may call back into this object
        cond = threads.cond_create().value;
    }
    ~Impl() { threads.cond_destroy(cond); threads.mutex_destroy(mutex); }

    platform::Clock& clock;
    platform::Threads& threads;
    HandleTable& handles;
    std::function<uint32_t()> current_thread;
    platform::MutexHandle mutex;
    platform::CondHandle cond;
    struct Lock { Impl& i; explicit Lock(Impl& impl) : i(impl) { i.threads.mutex_lock(i.mutex); } ~Lock() { i.threads.mutex_unlock(i.mutex); } };

    WndProcInvoker invoker;
    platform::Window* window = nullptr;
    platform::Input* input = nullptr;
    platform::Display* display = nullptr;
    uint32_t input_listener = 0;
    std::vector<platform::InputEvent> input_events;     // collected by the listener, turned into messages by pump_platform
    std::map<std::string, std::shared_ptr<ClassObject>> classes;
    uint16_t next_atom = 0xC000;
    std::map<uint32_t, Queue> queues;
    Handle focus = 0, active = 0, foreground = 0, capture = 0;
    int32_t cursor_count = 0;
    uint32_t cursor = 0;
    bool clipped = false;
    Rect32 clip;
    uint64_t dispatched = 0;
    int32_t screen_w = 1920, screen_h = 1080;
    bool keys_down[platform::kScancodeCount] = {};
    bool shift_down = false, caps = false;               // as of the keys the platform has reported (used for the flags of new messages)
    bool retrieved_shift = false, retrieved_caps = false;   // as of the key messages the program has retrieved (what TranslateMessage sees, like GetKeyState)
    std::map<std::string, uint32_t> resources;
    uint32_t next_resource = 0x10001;

    std::shared_ptr<WindowObject> window_of(Handle h) {
        auto o = handles.get(h);
        return o && o->kind == ObjectKind::other ? std::dynamic_pointer_cast<WindowObject>(o) : nullptr;
    }
    Queue& queue_of(uint32_t thread) { return queues[thread]; }
    uint32_t now_ms() { return uint32_t(clock.time_ns() / 1000000); }

    int32_t call(const std::shared_ptr<WindowObject>& w, Handle h, uint32_t msg, uint32_t wp, int32_t lp) {
        ++dispatched;
        if (w && w->proc && invoker) return invoker(w->proc, h, msg, wp, lp);
        return def_proc(h, msg, wp, lp);
    }
    void post(Handle hwnd, uint32_t thread, uint32_t msg, uint32_t wp, int32_t lp) {
        Msg m; m.hwnd = hwnd; m.message = msg; m.wparam = wp; m.lparam = lp; m.time = now_ms();
        queue_of(thread).posted.push_back(m);
        threads.cond_broadcast(cond);
    }
    int32_t def_proc(Handle hwnd, uint32_t msg, uint32_t wp, int32_t lp) {
        (void)wp; (void)lp;
        switch (msg) {
        case wm::nccreate: return 1;
        case wm::close: destroy(hwnd); return 0;
        case wm::erasebkgnd: return 1;
        case wm::nchittest: return 1;                       // HTCLIENT
        case wm::setcursor: return 0;
        case wm::syscommand: if ((wp & 0xFFF0) == 0xF060) { post_to_window(hwnd, wm::close, 0, 0); } return 0;   // SC_CLOSE
        case wm::ncactivate: return 1;
        default: return 0;
        }
    }
    void post_to_window(Handle hwnd, uint32_t msg, uint32_t wp, int32_t lp) {
        auto w = window_of(hwnd);
        if (w) post(hwnd, w->thread, msg, wp, lp);
    }
    void destroy(Handle hwnd);
    void enqueue_focus_change(Handle gained, Handle lost);
};

void WindowSystem::Impl::destroy(Handle hwnd) {
    auto w = window_of(hwnd);
    if (!w || w->destroyed) return;
    // Children first, then the window itself: WM_DESTROY while it is still valid, WM_NCDESTROY at the end.
    const auto children = w->children;
    for (Handle c : children) destroy(c);
    if (focus == hwnd) { focus = 0; call(w, hwnd, wm::killfocus, 0, 0); }
    if (active == hwnd) active = 0;
    if (foreground == hwnd) foreground = 0;
    if (capture == hwnd) capture = 0;
    call(w, hwnd, wm::destroy, 0, 0);
    w->destroyed = true;
    call(w, hwnd, wm::ncdestroy, 0, 0);
    if (auto p = window_of(w->parent)) p->children.erase(std::remove(p->children.begin(), p->children.end(), hwnd), p->children.end());
    if (w->cls) --w->cls->windows;
    // Pending messages of the window are discarded.
    auto& q = queue_of(w->thread);
    q.posted.erase(std::remove_if(q.posted.begin(), q.posted.end(), [hwnd](const Msg& m) { return m.hwnd == hwnd; }), q.posted.end());
    handles.close(hwnd);
}

WindowSystem::WindowSystem(platform::Clock& clock, platform::Threads& threads, HandleTable& handles, std::function<uint32_t()> tid) : impl_(std::make_unique<Impl>(clock, threads, handles, std::move(tid))) {
    // The desktop window: a permanent window that owns nothing.
    auto desk = std::make_shared<WindowObject>();
    desk->visible = true;
    desk->rect = {0, 0, impl_->screen_w, impl_->screen_h};
    desk->title = "Desktop";
    desktop_ = handles.add(desk);
}

WindowSystem::~WindowSystem() {
    if (impl_->input && impl_->input_listener) impl_->input->remove_listener(impl_->input_listener);
}

void WindowSystem::set_wndproc_invoker(WndProcInvoker invoker) { impl_->invoker = std::move(invoker); }

void WindowSystem::attach_platform(platform::Window* window, platform::Input* input, platform::Display* display) {
    impl_->window = window;
    impl_->display = display;
    if (display) { auto all = display->list(false); for (auto& d : all) if (d.primary) { impl_->screen_w = int32_t(d.current.width); impl_->screen_h = int32_t(d.current.height); } }
    if (auto desk = impl_->window_of(desktop_)) desk->rect = {0, 0, impl_->screen_w, impl_->screen_h};
    impl_->input = input;
    if (input) {
        Impl* impl = impl_.get();
        impl_->input_listener = input->add_listener([impl](const platform::InputEvent& e) {
            Impl::Lock lock(*impl);
            impl->input_events.push_back(e);
        });
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ classes
Ret<uint16_t> WindowSystem::register_class(const WndClass& desc) {
    Impl::Lock lock(*impl_);
    if (desc.name.empty()) return fail<uint16_t>(err::invalid_parameter);
    const std::string key = platform::path::fold(desc.name);
    if (impl_->classes.count(key)) return fail<uint16_t>(err::class_already_exists);
    auto c = std::make_shared<ClassObject>();
    c->desc = desc;
    c->atom = impl_->next_atom++;
    impl_->classes[key] = c;
    return ok(c->atom);
}

Ret<bool> WindowSystem::unregister_class(std::string_view name) {
    Impl::Lock lock(*impl_);
    auto it = impl_->classes.find(platform::path::fold(name));
    if (it == impl_->classes.end()) return fail<bool>(err::class_does_not_exist);
    if (it->second->windows > 0) return fail<bool>(kErrorBusy);
    impl_->classes.erase(it);
    return ok(true);
}

// ------------------------------------------------------------------------------------------------------------------------------ windows
Ret<Handle> WindowSystem::create_window(const CreateParams& p) {
    std::shared_ptr<ClassObject> cls;
    {
        Impl::Lock lock(*impl_);
        auto it = impl_->classes.find(platform::path::fold(p.class_name));
        if (it == impl_->classes.end()) return fail<Handle>(kErrorCannotFindWndClass);
        cls = it->second;
        if (p.parent && !impl_->window_of(p.parent)) return fail<Handle>(err::invalid_window_handle);
    }
    auto w = std::make_shared<WindowObject>();
    w->cls = cls;
    w->ex_style = p.ex_style;
    w->style = p.style & ~ws::visible;
    w->title = p.title;
    w->parent = p.parent;
    w->thread = impl_->current_thread();
    w->proc = cls->desc.proc;
    w->instance = p.instance;
    const bool use_default_size = p.width == int32_t(kCwUseDefault) || p.height == int32_t(kCwUseDefault);
    const int32_t x = p.x == int32_t(kCwUseDefault) ? 0 : p.x, y = p.y == int32_t(kCwUseDefault) ? 0 : p.y;
    const int32_t width = use_default_size ? 640 : p.width, height = use_default_size ? 480 : p.height;
    w->rect = {x, y, x + width, y + height};
    const Handle h = impl_->handles.add(w);
    if (!h) return fail<Handle>(err::not_enough_memory);
    {
        Impl::Lock lock(*impl_);
        ++cls->windows;
        if (auto parent = impl_->window_of(p.parent)) parent->children.push_back(h);
    }
    // The creation messages. Windows sends WM_GETMINMAXINFO first for top-level windows; the lparam of NCCREATE and CREATE (a CREATESTRUCT in guest
    // memory) is the facade's to build, so it travels as the window parameter `p.param` here.
    impl_->call(w, h, 0x24, 0, 0);                                          // WM_GETMINMAXINFO
    if (impl_->call(w, h, wm::nccreate, 0, int32_t(p.param)) == 0) { impl_->destroy(h); return fail<Handle>(err::invalid_parameter); }
    impl_->call(w, h, wm::nccalcsize, 0, 0);
    if (impl_->call(w, h, wm::create, 0, int32_t(p.param)) == -1) { impl_->destroy(h); return fail<Handle>(err::invalid_parameter); }
    if (p.style & ws::visible) show_window(h, sw::shownormal);
    return ok(h);
}

Ret<bool> WindowSystem::destroy_window(Handle h) {
    auto w = impl_->window_of(h);
    if (!w || h == desktop_) return fail<bool>(err::invalid_window_handle);
    if (w->thread != impl_->current_thread()) return fail<bool>(5);          // a window can only be destroyed by its own thread
    impl_->destroy(h);
    return ok(true);
}

bool WindowSystem::is_window(Handle h) const { return impl_->window_of(h) != nullptr; }

Ret<bool> WindowSystem::show_window(Handle h, uint32_t command) {
    auto w = impl_->window_of(h);
    if (!w) return fail<bool>(err::invalid_window_handle);
    const bool was_visible = w->visible;
    const bool show = command != sw::hide;
    if (show != w->visible) {
        w->visible = show;
        impl_->call(w, h, wm::showwindow, show ? 1 : 0, 0);
        if (show) {
            impl_->call(w, h, wm::move, 0, (w->rect.top << 16) | (w->rect.left & 0xFFFF));
            const int32_t cw = w->rect.right - w->rect.left, ch = w->rect.bottom - w->rect.top;
            impl_->call(w, h, wm::size, 0, (ch << 16) | (cw & 0xFFFF));
            w->needs_paint = true;
        }
    }
    w->minimized = command == sw::showminimized || command == sw::minimize;
    // A top-level window that becomes visible takes the activation and the focus when nothing else has them.
    if (show && command != sw::shownoactivate && w->parent == 0 && !(w->style & ws::child)) {
        const Handle previous_active = impl_->active;
        if (previous_active != h) {
            impl_->active = h;
            impl_->foreground = h;
            if (previous_active == 0) impl_->call(w, h, wm::activateapp, 1, 0);
            impl_->call(w, h, wm::ncactivate, 1, 0);
            impl_->call(w, h, wm::activate, 1, 0);
        }
        if (impl_->focus != h) { impl_->focus = h; impl_->call(w, h, wm::setfocus, 0, 0); }
    }
    return ok(was_visible);
}

Ret<bool> WindowSystem::update_window(Handle h) {
    auto w = impl_->window_of(h);
    if (!w) return fail<bool>(err::invalid_window_handle);
    if (w->visible && w->needs_paint) { w->needs_paint = false; impl_->call(w, h, wm::paint, 0, 0); }
    return ok(true);
}

Ret<bool> WindowSystem::is_iconic(Handle h) {
    auto w = impl_->window_of(h);
    if (!w) return fail<bool>(err::invalid_window_handle);
    return ok(w->minimized);
}

Ret<Rect32> WindowSystem::get_window_rect(Handle h) {
    auto w = impl_->window_of(h);
    if (!w) return fail<Rect32>(err::invalid_window_handle);
    return ok(w->rect);
}

Rect32 WindowSystem::adjust_window_rect(Rect32 r, uint32_t style, bool menu, uint32_t ex_style) {
    // The non-client metrics of a classic Windows theme: 1-pixel border, 4-pixel sizing frame, 19-pixel caption, 19-pixel menu bar.
    int32_t border = 0, caption = 0;
    if ((style & ws::caption) == ws::caption) { caption = 19 + 1; border = (style & ws::thickframe) ? 4 + 1 : 1 + 1; }
    else if (style & ws::border) border = 1;
    else if (style & ws::thickframe) border = 4;
    if (ex_style & ws_ex::clientedge) border += 2;
    if (ex_style & ws_ex::windowedge) border += 2;
    r.left -= border; r.right += border; r.top -= border + caption + (menu ? 19 : 0); r.bottom += border;
    return r;
}

Ret<Rect32> WindowSystem::get_client_rect(Handle h) {
    auto w = impl_->window_of(h);
    if (!w) return fail<Rect32>(err::invalid_window_handle);
    // The client area: the window rectangle minus the frame; its origin is (0, 0).
    const Rect32 outer = adjust_window_rect({0, 0, 0, 0}, w->style, false, w->ex_style);
    Rect32 c;
    c.right = (w->rect.right - w->rect.left) + outer.left + outer.left * 0 - outer.right * 0 + (outer.left - outer.left);   // width minus both side borders
    c.right = (w->rect.right - w->rect.left) - (outer.right - outer.left);
    c.bottom = (w->rect.bottom - w->rect.top) - (outer.bottom - outer.top);
    return ok(c);
}

Ret<bool> WindowSystem::set_window_pos(Handle h, int32_t x, int32_t y, int32_t width, int32_t height) {
    auto w = impl_->window_of(h);
    if (!w) return fail<bool>(err::invalid_window_handle);
    const Rect32 before = w->rect;
    w->rect = {x, y, x + width, y + height};
    if (w->visible) {
        if (before.left != x || before.top != y) impl_->call(w, h, wm::move, 0, (y << 16) | (x & 0xFFFF));
        if (before.right - before.left != width || before.bottom - before.top != height) {
            const Rect32 outer = adjust_window_rect({0, 0, 0, 0}, w->style, false, w->ex_style);
            impl_->call(w, h, wm::size, 0, (((height - (outer.bottom - outer.top))) << 16) | ((width - (outer.right - outer.left)) & 0xFFFF));
        }
    }
    return ok(true);
}

Ret<int32_t> WindowSystem::get_window_long(Handle h, int32_t index) {
    auto w = impl_->window_of(h);
    if (!w) return fail<int32_t>(err::invalid_window_handle);
    switch (index) {
    case gwl::style: return ok(int32_t(w->style | (w->visible ? ws::visible : 0u)));
    case gwl::exstyle: return ok(int32_t(w->ex_style));
    case gwl::wndproc: return ok(int32_t(w->proc));
    case gwl::hinstance: return ok(int32_t(w->instance));
    case gwl::hwndparent: return ok(int32_t(w->parent));
    default: { auto it = w->longs.find(index); return ok(it == w->longs.end() ? 0 : it->second); }
    }
}

Ret<int32_t> WindowSystem::set_window_long(Handle h, int32_t index, int32_t value) {
    auto w = impl_->window_of(h);
    if (!w) return fail<int32_t>(err::invalid_window_handle);
    int32_t previous;
    switch (index) {
    case gwl::style: previous = int32_t(w->style | (w->visible ? ws::visible : 0u)); w->style = uint32_t(value) & ~ws::visible; break;
    case gwl::exstyle: previous = int32_t(w->ex_style); w->ex_style = uint32_t(value); break;
    case gwl::wndproc: previous = int32_t(w->proc); w->proc = uint32_t(value); break;
    case gwl::hinstance: previous = int32_t(w->instance); w->instance = uint32_t(value); break;
    case gwl::hwndparent: previous = int32_t(w->parent); w->parent = Handle(value); break;
    default: previous = w->longs.count(index) ? w->longs[index] : 0; w->longs[index] = value; break;
    }
    return ok(previous);
}

int32_t WindowSystem::get_system_metrics(int32_t index) const {
    switch (index) {
    case sm::cxscreen: case sm::cxvirtualscreen: return impl_->screen_w;
    case sm::cyscreen: case sm::cyvirtualscreen: return impl_->screen_h;
    case sm::cxvscroll: return 17;
    case sm::cyhscroll: return 17;
    case sm::cycaption: return 19;
    case sm::cxborder: case sm::cyborder: return 1;
    case sm::cxfixedframe: case sm::cyfixedframe: return 3;
    case sm::cxframe: case sm::cyframe: return 4;
    case sm::cmonitors: return 1;
    default: return 0;
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ focus
Handle WindowSystem::set_focus(Handle h) {
    Impl::Lock lock(*impl_);
    const Handle previous = impl_->focus;
    if (previous == h) return previous;
    auto old = impl_->window_of(previous);
    auto now = impl_->window_of(h);
    if (h && !now) return previous;
    impl_->focus = h;
    if (old) impl_->call(old, previous, wm::killfocus, h, 0);
    if (now) impl_->call(now, h, wm::setfocus, previous, 0);
    return previous;
}
Handle WindowSystem::get_focus() const { return impl_->focus; }

Ret<bool> WindowSystem::set_foreground_window(Handle h) {
    if (!impl_->window_of(h)) return fail<bool>(err::invalid_window_handle);
    impl_->foreground = h;
    set_active_window(h);
    return ok(true);
}
Handle WindowSystem::get_foreground_window() const { return impl_->foreground; }

Handle WindowSystem::set_active_window(Handle h) {
    const Handle previous = impl_->active;
    auto now = impl_->window_of(h);
    if (!now || previous == h) return previous;
    auto old = impl_->window_of(previous);
    impl_->active = h;
    if (old) { impl_->call(old, previous, wm::ncactivate, 0, 0); impl_->call(old, previous, wm::activate, 0, int32_t(h)); }
    impl_->call(now, h, wm::ncactivate, 1, 0);
    impl_->call(now, h, wm::activate, 1, int32_t(previous));
    if (impl_->focus != h) set_focus(h);
    return previous;
}
Handle WindowSystem::get_active_window() const { return impl_->active; }

// ------------------------------------------------------------------------------------------------------------------------------ messages
void WindowSystem::pump_platform() {
    std::vector<platform::InputEvent> events;
    {
        Impl::Lock lock(*impl_);
        events.swap(impl_->input_events);
    }
    if (impl_->window) {
        platform::WindowEvent buffer[16];
        for (;;) {
            const size_t n = impl_->window->poll_events(buffer);
            for (size_t i = 0; i < n; ++i) {
                const auto& e = buffer[i];
                Impl::Lock lock(*impl_);
                const Handle target = impl_->active ? impl_->active : impl_->foreground;
                auto w = impl_->window_of(target);
                if (!w) continue;
                switch (e.kind) {
                case platform::EventKind::close: impl_->post(target, w->thread, wm::close, 0, 0); break;
                case platform::EventKind::focus_gained: impl_->post(target, w->thread, wm::activate, 1, 0); impl_->post(target, w->thread, wm::setfocus, 0, 0); impl_->foreground = target; break;
                case platform::EventKind::focus_lost: impl_->post(target, w->thread, wm::activate, 0, 0); impl_->post(target, w->thread, wm::killfocus, 0, 0); break;
                case platform::EventKind::resized: impl_->post(target, w->thread, wm::size, 0, int32_t((e.height << 16) | (e.width & 0xFFFF))); break;
                default: break;
                }
            }
            if (n < 16) break;
        }
    }
    Impl::Lock lock(*impl_);
    const Handle target = impl_->focus ? impl_->focus : impl_->active;
    auto w = impl_->window_of(target);
    if (!w) return;
    const bool hold_capture = impl_->capture != 0;
    const Handle mouse_target = hold_capture ? impl_->capture : target;
    auto mw = impl_->window_of(mouse_target);
    for (const auto& e : events) {
        switch (e.kind) {
        case platform::InputEventKind::key: {
            if (e.code >= platform::kScancodeCount) break;
            const uint32_t vk = virtual_key_of(e.code);
            if (!vk) break;
            const bool down = e.value != 0, was_down = impl_->keys_down[e.code];
            impl_->keys_down[e.code] = down;
            if (e.code == 225 || e.code == 229) impl_->shift_down = impl_->keys_down[225] || impl_->keys_down[229];
            if (e.code == 57 && down && !was_down) impl_->caps = !impl_->caps;
            const bool alt = impl_->keys_down[226] || impl_->keys_down[230];
            uint32_t flags = (scan_code_of(e.code) << 16) | (is_extended_key(e.code) ? 1u << 24 : 0u) | (alt ? 1u << 29 : 0u);
            if (down) flags |= 1;                                                  // repeat count
            else flags |= (1u << 30) | (1u << 31);                                 // previous state down, transition to up
            if (down && was_down) flags |= 1u << 30;
            const bool system = alt && vk != 0x12 && vk != 0xA4 && vk != 0xA5 ? true : (vk == 0x12 || vk == 0xA4 || vk == 0xA5 ? true : false);
            impl_->post(target, w->thread, down ? (system ? wm::syskeydown : wm::keydown) : (system ? wm::syskeyup : wm::keyup), vk, int32_t(flags));
            break;
        }
        case platform::InputEventKind::mouse_button: {
            if (!mw) break;
            static const uint32_t down_msgs[] = {0, wm::lbuttondown, wm::mbuttondown, wm::rbuttondown, wm::xbuttondown, wm::xbuttondown};
            static const uint32_t up_msgs[] = {0, wm::lbuttonup, wm::mbuttonup, wm::rbuttonup, wm::xbuttonup, wm::xbuttonup};
            if (e.code < 1 || e.code > 5) break;
            float x = 0, y = 0;
            const uint32_t mask = impl_->input ? impl_->input->mouse_state(&x, &y) : 0;
            const uint32_t keys = (mask & 1 ? 1u : 0u) | (mask & 4 ? 2u : 0u) | (impl_->shift_down ? 4u : 0u) | (mask & 2 ? 0x10u : 0u);
            impl_->post(mouse_target, mw->thread, e.value ? down_msgs[e.code] : up_msgs[e.code], keys, int32_t((int32_t(y) << 16) | (int32_t(x) & 0xFFFF)));
            break;
        }
        case platform::InputEventKind::mouse_motion: {
            if (!mw) break;
            float x = 0, y = 0;
            const uint32_t mask = impl_->input ? impl_->input->mouse_state(&x, &y) : 0;
            const uint32_t keys = (mask & 1 ? 1u : 0u) | (mask & 4 ? 2u : 0u) | (impl_->shift_down ? 4u : 0u) | (mask & 2 ? 0x10u : 0u);
            impl_->post(mouse_target, mw->thread, wm::mousemove, keys, int32_t((int32_t(y) << 16) | (int32_t(x) & 0xFFFF)));
            break;
        }
        case platform::InputEventKind::mouse_wheel:
            if (mw) impl_->post(mouse_target, mw->thread, wm::mousewheel, uint32_t(uint16_t(e.value)) << 16, 0);
            break;
        default: break;
        }
    }
}

namespace {
bool matches(const Msg& m, Handle hwnd, uint32_t min, uint32_t max) {
    if (hwnd && m.hwnd != hwnd) return false;
    if (min == 0 && max == 0) return true;
    return m.message >= min && m.message <= max;
}
}  // namespace

Ret<bool> WindowSystem::peek_message(Msg& out, Handle hwnd, uint32_t min, uint32_t max, uint32_t flags) {
    pump_platform();
    Impl::Lock lock(*impl_);
    auto& q = impl_->queue_of(impl_->current_thread());
    for (auto it = q.posted.begin(); it != q.posted.end(); ++it) {
        if (!matches(*it, hwnd, min, max)) continue;
        out = *it;
        if (flags & pm::remove) {
            if (out.message == wm::keydown || out.message == wm::keyup || out.message == wm::syskeydown || out.message == wm::syskeyup) {
                const bool down = out.message == wm::keydown || out.message == wm::syskeydown;
                if (out.wparam == 0x10 || out.wparam == 0xA0 || out.wparam == 0xA1) impl_->retrieved_shift = down;
                if (out.wparam == 0x14 && down && !((uint32_t(out.lparam) >> 30) & 1)) impl_->retrieved_caps = !impl_->retrieved_caps;
            }
            q.posted.erase(it);
        }
        return ok(true);
    }
    // WM_PAINT is generated when a visible window has something to paint and no other message is waiting; WM_QUIT comes last.
    if (q.quit && (min == 0 && max == 0 ? true : (wm::quit >= min && wm::quit <= max))) {
        out = Msg{0, wm::quit, uint32_t(q.exit_code), 0, impl_->now_ms(), 0, 0};
        if (flags & pm::remove) q.quit = false;
        return ok(true);
    }
    return ok(false);
}

Ret<int32_t> WindowSystem::get_message(Msg& out, Handle hwnd, uint32_t min, uint32_t max) {
    for (;;) {
        auto r = peek_message(out, hwnd, min, max, pm::remove);
        if (r.value) return ok(out.message == wm::quit ? 0 : 1);
        Impl::Lock lock(*impl_);
        impl_->threads.cond_wait(impl_->cond, impl_->mutex, 20000000);   // wake at least every 20 ms to poll the platform
    }
}

Ret<bool> WindowSystem::post_message(Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam) {
    Impl::Lock lock(*impl_);
    if (hwnd == 0) { impl_->post(0, impl_->current_thread(), message, wparam, lparam); return ok(true); }
    if (hwnd == kBroadcast) {
        for (auto& [thread, q] : impl_->queues) { (void)q; impl_->post(kBroadcast, thread, message, wparam, lparam); }
        return ok(true);
    }
    auto w = impl_->window_of(hwnd);
    if (!w) return fail<bool>(err::invalid_window_handle);
    impl_->post(hwnd, w->thread, message, wparam, lparam);
    return ok(true);
}

Ret<bool> WindowSystem::post_thread_message(uint32_t thread_id, uint32_t message, uint32_t wparam, int32_t lparam) {
    Impl::Lock lock(*impl_);
    if (!impl_->queues.count(thread_id)) return fail<bool>(err::invalid_thread_id);   // a thread that has not created a queue yet has none
    impl_->post(0, thread_id, message, wparam, lparam);
    return ok(true);
}

void WindowSystem::post_quit_message(int32_t exit_code) {
    Impl::Lock lock(*impl_);
    auto& q = impl_->queue_of(impl_->current_thread());
    q.quit = true;
    q.exit_code = exit_code;
    impl_->threads.cond_broadcast(impl_->cond);
}

bool WindowSystem::translate_message(const Msg& m) {
    if (m.message != wm::keydown && m.message != wm::syskeydown) return false;
    const char32_t c = character_of(m.wparam, impl_->retrieved_shift, impl_->retrieved_caps);
    if (!c) return false;
    Impl::Lock lock(*impl_);
    auto w = impl_->window_of(m.hwnd);
    // The character goes to the front: it belongs to the key message just retrieved, so it comes out before any further input.
    Msg ch; ch.hwnd = m.hwnd; ch.message = m.message == wm::syskeydown ? wm::syschar : wm::char_; ch.wparam = uint32_t(c); ch.lparam = m.lparam; ch.time = impl_->now_ms();
    impl_->queue_of(w ? w->thread : impl_->current_thread()).posted.push_front(ch);
    impl_->threads.cond_broadcast(impl_->cond);
    return true;
}

int32_t WindowSystem::dispatch_message(const Msg& m) {
    if (!m.hwnd) return 0;
    auto w = impl_->window_of(m.hwnd);
    if (!w) return 0;
    return impl_->call(w, m.hwnd, m.message, m.wparam, m.lparam);
}

int32_t WindowSystem::send_message(Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam) {
    auto w = impl_->window_of(hwnd);
    if (!w) return 0;
    return impl_->call(w, hwnd, message, wparam, lparam);
}

int32_t WindowSystem::def_window_proc(Handle hwnd, uint32_t message, uint32_t wparam, int32_t lparam) { return impl_->def_proc(hwnd, message, wparam, lparam); }

size_t WindowSystem::pending_messages(uint32_t thread_id) const {
    Impl::Lock lock(*impl_);
    auto it = impl_->queues.find(thread_id);
    return it == impl_->queues.end() ? 0 : it->second.posted.size();
}
uint64_t WindowSystem::messages_dispatched() const { return impl_->dispatched; }

// ------------------------------------------------------------------------------------------------------------------------------ cursor
int32_t WindowSystem::show_cursor(bool show) {
    Impl::Lock lock(*impl_);
    impl_->cursor_count += show ? 1 : -1;
    if (impl_->window) impl_->window->show_cursor(impl_->cursor_count >= 0);
    return impl_->cursor_count;
}
uint32_t WindowSystem::set_cursor(uint32_t cursor) { const uint32_t previous = impl_->cursor; impl_->cursor = cursor; return previous; }
bool WindowSystem::get_cursor_pos(int32_t& x, int32_t& y) {
    float fx = 0, fy = 0;
    if (impl_->input) impl_->input->mouse_state(&fx, &fy);
    x = int32_t(fx); y = int32_t(fy);
    return true;
}
Handle WindowSystem::set_capture(Handle h) {
    Impl::Lock lock(*impl_);
    const Handle previous = impl_->capture;
    if (impl_->window_of(h)) impl_->capture = h;
    return previous;
}
bool WindowSystem::release_capture() { Impl::Lock lock(*impl_); impl_->capture = 0; return true; }
Handle WindowSystem::get_capture() const { return impl_->capture; }
bool WindowSystem::clip_cursor(const Rect32* rect) {
    Impl::Lock lock(*impl_);
    impl_->clipped = rect != nullptr;
    if (rect) impl_->clip = *rect;
    else if (impl_->window) impl_->window->release_cursor_clip();
    return true;
}
uint32_t WindowSystem::load_cursor(uint32_t instance, std::string_view name) {
    Impl::Lock lock(*impl_);
    const std::string key = "c" + std::to_string(instance) + ":" + std::string(name);
    auto it = impl_->resources.find(key);
    if (it != impl_->resources.end()) return it->second;
    return impl_->resources[key] = impl_->next_resource += 4;
}
uint32_t WindowSystem::load_icon(uint32_t instance, std::string_view name) {
    Impl::Lock lock(*impl_);
    const std::string key = "i" + std::to_string(instance) + ":" + std::string(name);
    auto it = impl_->resources.find(key);
    if (it != impl_->resources.end()) return it->second;
    return impl_->resources[key] = impl_->next_resource += 4;
}

}  // namespace nue::legacy::win32
