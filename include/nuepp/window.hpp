// NUE C++ wrapper: events, windows, graphics context, input helpers.
#pragma once

#include "nuepp/core.hpp"

#include <optional>

namespace nuepp {

// ---- events ------------------------------------------------------------------------------------------------------------------------------
namespace events {
inline int pump() { return nue_pump_events(); }
inline std::optional<nue_event> poll() { nue_event e; if (nue_poll_event(&e)) return e; return std::nullopt; }
inline std::optional<nue_event> wait(int32_t timeout_ms) { nue_event e; if (nue_wait_event(&e, timeout_ms)) return e; return std::nullopt; }
// Drains the queue calling fn(const nue_event&) for each event; fn returns false to stop early. Returns the number handled.
template <class F>
int drain(F&& fn) {
    int n = 0;
    nue_event e;
    while (nue_poll_event(&e)) { ++n; if (!fn(static_cast<const nue_event&>(e))) break; }
    return n;
}
inline Result post_user(int32_t code, uint64_t a = 0, uint64_t b = 0) { return nue_push_user_event(code, a, b); }
}  // namespace events

// ---- windows -----------------------------------------------------------------------------------------------------------------------------
class Window {
public:
    Window() = default;
    explicit Window(const nue_window_desc& desc) { create(desc); }
    Window(Window&& o) noexcept : w_(o.w_), result_(o.result_) { o.w_ = nullptr; }
    Window& operator=(Window&& o) noexcept { if (this != &o) { reset(); w_ = o.w_; result_ = o.result_; o.w_ = nullptr; } return *this; }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    ~Window() { reset(); }

    Result create(const nue_window_desc& desc) { reset(); result_ = Result(nue_window_create(&desc, &w_)); return result_; }
    // Wraps a native window the host already owns (never destroyed by NUE).
    static Window attach(const nue_native_window& native, nue_window_desc desc = init<nue_window_desc>()) {
        desc.attach = &native;
        return Window(desc);
    }
    void reset() { if (w_) { nue_window_destroy(w_); w_ = nullptr; } }

    bool ok() const { return w_ != nullptr; }
    const Result& result() const { return result_; }
    nue_window handle() const { return w_; }
    nue_window_id id() const { return nue_window_get_id(w_); }

    Result set_title(const char* t) { return nue_window_set_title(w_, t); }
    std::string title() const {
        size_t need = 0;
        nue_window_get_title(w_, nullptr, 0, &need);
        std::string s(need ? need : 1, '\0');
        nue_window_get_title(w_, &s[0], s.size(), &need);
        s.resize(need ? need - 1 : 0);
        return s;
    }
    nue_size_i size() const { nue_size_i s{0, 0}; nue_window_get_size(w_, &s.w, &s.h); return s; }
    nue_size_i pixel_size() const { nue_size_i s{0, 0}; nue_window_get_pixel_size(w_, &s.w, &s.h); return s; }
    Result set_size(int w, int h) { return nue_window_set_size(w_, w, h); }
    Result set_mode(nue_window_mode m, const nue_display_mode* mode = nullptr) { return nue_window_set_mode(w_, m, mode); }
    float content_scale() const { return nue_window_get_content_scale(w_); }
    float refresh_rate() const { return nue_window_get_refresh_rate(w_); }
    bool has_focus() const { return nue_window_has_focus(w_) != 0; }
    nue_rect_i safe_area() const { nue_rect_i r{0, 0, 0, 0}; nue_window_get_safe_area(w_, &r); return r; }
    nue_native_window native() const { nue_native_window n = init<nue_native_window>(); nue_window_get_native(w_, &n); return n; }
    Result show() { return nue_window_show(w_); }
    Result hide() { return nue_window_hide(w_); }
    Result set_relative_mouse(bool on) { return nue_window_set_relative_mouse(w_, on ? NUE_TRUE : NUE_FALSE); }
    Result clip_cursor(bool on, const nue_rect_i* rect = nullptr) { return nue_window_set_cursor_clip(w_, rect, on ? NUE_TRUE : NUE_FALSE); }

private:
    nue_window w_ = nullptr;
    Result result_;
};

// ---- graphics context --------------------------------------------------------------------------------------------------------------------
class Gfx {
public:
    Gfx() = default;
    Gfx(const Window& window, nue_gfx_flags flags = NUE_GFX_PREPARE_ONLY | NUE_GFX_VSYNC, nue_renderer renderer = NUE_RENDERER_AUTO) {
        nue_gfx_desc d = init<nue_gfx_desc>();
        d.window = window.handle(); d.flags = flags; d.renderer = renderer;
        result_ = Result(nue_gfx_create(&d, &g_));
    }
    Gfx(Gfx&& o) noexcept : g_(o.g_), result_(o.result_) { o.g_ = nullptr; }
    Gfx& operator=(Gfx&& o) noexcept { if (this != &o) { reset(); g_ = o.g_; result_ = o.result_; o.g_ = nullptr; } return *this; }
    Gfx(const Gfx&) = delete;
    Gfx& operator=(const Gfx&) = delete;
    ~Gfx() { reset(); }
    void reset() { if (g_) { nue_gfx_destroy(g_); g_ = nullptr; } }
    bool ok() const { return g_ != nullptr; }
    const Result& result() const { return result_; }
    nue_gfx handle() const { return g_; }
    nue_gfx_native native() const { nue_gfx_native n = init<nue_gfx_native>(); nue_gfx_get_native(g_, &n); return n; }
    Result set_auto_events(bool on) { return nue_gfx_set_auto_events(g_, on ? NUE_TRUE : NUE_FALSE); }
    Result resize(int w = 0, int h = 0) { return nue_gfx_resize(g_, w, h); }
    nue_gpu_caps gpu_caps() const { nue_gpu_caps c = init<nue_gpu_caps>(); nue_gfx_get_gpu_caps(g_, &c); return c; }
private:
    nue_gfx g_ = nullptr;
    Result result_;
};

// ---- input -------------------------------------------------------------------------------------------------------------------------------
namespace input {
inline bool key_down(nue_scancode sc) { return nue_key_is_down(sc) != 0; }
inline nue_last_input last_device() { nue_last_input l = init<nue_last_input>(); nue_input_get_last_device(&l); return l; }
inline std::vector<nue_gamepad_id> gamepads() {
    uint32_t n = 0;
    nue_gamepad_list(nullptr, 0, &n);
    std::vector<nue_gamepad_id> v(n);
    if (n) { nue_gamepad_list(v.data(), n, &n); v.resize(n); }
    return v;
}
inline nue_gamepad_state gamepad_state(nue_gamepad_id id) { nue_gamepad_state s = init<nue_gamepad_state>(); nue_gamepad_get_state(id, &s); return s; }
}  // namespace input

}  // namespace nuepp
