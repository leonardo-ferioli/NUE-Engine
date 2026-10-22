// The NULL (headless) backend: one virtual display, bookkeeping windows, virtual audio devices consumed by a (virtual) clock, scripted input.
// Everything in the public API works against it, so the whole engine is testable without a window or a sound card.
#include "internal/core_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ---- state --------------------------------------------------------------------------------------------------------------------
struct NullWindow {
    nue_window_id id = 0;
    nue_window_snapshot snap;
    std::string title;
    bool attached = false;
    bool clip = false;
    uint32_t min_w = 0, min_h = 0, max_w = 0, max_h = 0;
    bool text_input = false;
};

struct NullPad {
    nue_gamepad_info info;
    nue_gamepad_state state;
    uint16_t rumble_low = 0, rumble_high = 0;
    std::string mapping;
};

struct NullDevice {
    nue_backend_device_open open;
    nue_audio_spec spec;
    uint32_t buffer_frames = 480;
    bool paused = false;
    uint64_t frames_played = 0;
    double frac = 0;
    int64_t clock_ns = 0;               // virtual time of the last consumed sample
    std::vector<uint8_t> scratch;
    std::vector<uint8_t> capture;       // ring of the last capture_frames frames (device format)
    uint32_t capture_frames = 0;
    uint64_t capture_total = 0;
    std::mutex run_mutex;               // held while the callback runs (close waits on it)
    std::thread thread;                 // real-time mode only
    std::atomic<bool> stop{false};
};

struct Null {
    std::mutex mutex;
    bool deterministic = false;
    bool headless = true;
    std::atomic<int64_t> virtual_ns{1000000000};   // starts at 1 s so timestamps are never 0; atomic: read while the state lock is held
    int64_t epoch_real_ns = 0;
    nue_display_info display;
    std::map<nue_window_id, std::unique_ptr<NullWindow>> windows;
    nue_window_id focused = 0;
    std::vector<std::unique_ptr<NullPad>> pads;
    uint32_t next_pad = 1;
    std::vector<std::unique_ptr<NullDevice>> devices;
    nue_keymod mods = 0;
    float mouse_x = 0, mouse_y = 0;
    uint32_t mouse_buttons = 0;
    bool active = false;
};

Null& N() {
    static Null* n = new Null();
    return *n;
}

const nue_native_kind kNativeKind =
#if defined(NUE_PLATFORM_WINDOWS)
    NUE_NATIVE_WIN32_HWND;
#elif defined(NUE_PLATFORM_ANDROID)
    NUE_NATIVE_ANDROID_WINDOW;
#elif defined(NUE_PLATFORM_IOS)
    NUE_NATIVE_UIKIT_WINDOW;
#elif defined(NUE_PLATFORM_MACOS)
    NUE_NATIVE_COCOA_WINDOW;
#else
    NUE_NATIVE_X11_WINDOW;
#endif

void push(nue_event_type type, nue_window_id win, const nue_event* payload_src = nullptr) {
    nue_event e;
    if (payload_src) e = *payload_src; else std::memset(&e, 0, sizeof(e));
    e.type = type;
    e.window_id = win;
    nue_internal_push_event(&e);
}

void push_window(nue_event_type type, NullWindow& w, int32_t x = 0, int32_t y = 0, float scale = 0) {
    nue_internal_window_changed(w.id, &w.snap);   // the cached snapshot is never older than the events that announce it
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.window.x = x;
    e.payload.window.y = y;
    e.payload.window.scale = scale;
    push(type, w.id, &e);
}

void apply_pixel_size(NullWindow& w, const Null& n) {
    float scale = (w.snap.flags & NUE_WINDOW_HIGH_DPI) ? n.display.content_scale : 1.0f;
    w.snap.pixel_density = scale;
    w.snap.pixel_width = int32_t(std::lround(w.snap.width * scale));
    w.snap.pixel_height = int32_t(std::lround(w.snap.height * scale));
    w.snap.content_scale = n.display.content_scale;
}

void announce_size(NullWindow& w) {
    push_window(NUE_EVENT_WINDOW_RESIZED, w, w.snap.width, w.snap.height);
    push_window(NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED, w, w.snap.pixel_width, w.snap.pixel_height, w.snap.pixel_density);
}

void set_focus(Null& n, NullWindow* target) {
    nue_window_id old = n.focused;
    nue_window_id nid = target ? target->id : 0;
    if (old == nid) return;
    if (old) {
        auto it = n.windows.find(old);
        if (it != n.windows.end()) { it->second->snap.focused = NUE_FALSE; push_window(NUE_EVENT_WINDOW_FOCUS_LOST, *it->second); }
    }
    n.focused = nid;
    if (target) { target->snap.focused = NUE_TRUE; push_window(NUE_EVENT_WINDOW_FOCUS_GAINED, *target); }
}

NullWindow* win_of(void* impl) { return static_cast<NullWindow*>(impl); }

uint32_t bytes_per_frame(const nue_audio_spec& s) { return nue_audio_frame_size(&s); }

// ---- audio consumption --------------------------------------------------------------------------------------------------------
// Runs the device callback for `frames` frames (in chunks of buffer_frames) and records what was "played".
void consume(NullDevice& d, uint64_t frames) {
    std::lock_guard<std::mutex> lock(d.run_mutex);
    const uint32_t fsz = bytes_per_frame(d.spec);
    while (frames > 0 && !d.paused) {
        uint32_t chunk = uint32_t(std::min<uint64_t>(frames, d.buffer_frames));
        d.scratch.assign(size_t(chunk) * fsz, 0);
        if (d.open.callback) d.open.callback(d.open.userdata, d.scratch.data(), chunk);
        if (d.capture_frames) {
            if (d.capture.size() != size_t(d.capture_frames) * fsz) d.capture.assign(size_t(d.capture_frames) * fsz, 0);
            for (uint32_t i = 0; i < chunk; ++i) {
                size_t slot = size_t((d.capture_total + i) % d.capture_frames);
                std::memcpy(&d.capture[slot * fsz], &d.scratch[size_t(i) * fsz], fsz);
            }
            d.capture_total += chunk;
        }
        d.frames_played += chunk;
        frames -= chunk;
    }
}

void advance_devices(Null& n, int64_t ns) {
    std::vector<NullDevice*> devs;
    { std::lock_guard<std::mutex> lock(n.mutex); for (auto& d : n.devices) devs.push_back(d.get()); }
    for (NullDevice* d : devs) {
        if (d->paused) continue;
        d->frac += double(ns) * d->spec.sample_rate / 1e9;
        uint64_t whole = uint64_t(d->frac);
        d->frac -= double(whole);
        // keep the callback rhythm in whole buffers; leftover frames stay in `frac` for the next advance
        consume(*d, whole);
        d->clock_ns += ns;
    }
}

// ---- backend entry points -----------------------------------------------------------------------------------------------------

nue_result b_init(const nue_backend_init* init) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    n.deterministic = init->deterministic != 0;
    n.headless = init->headless != 0;
    n.virtual_ns = 1000000000;
    n.epoch_real_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    NUE_STRUCT_INIT(&n.display);
    n.display.id = 1;
    std::snprintf(n.display.name, sizeof(n.display.name), "NUE virtual display");
    n.display.bounds = {0, 0, 1920, 1080};
    n.display.usable_bounds = {0, 0, 1920, 1040};
    NUE_STRUCT_INIT(&n.display.current_mode);
    n.display.current_mode.width = 1920;
    n.display.current_mode.height = 1080;
    n.display.current_mode.refresh_rate_hz = 60.0f;
    n.display.current_mode.pixel_density = 1.0f;
    n.display.desktop_mode = n.display.current_mode;
    n.display.content_scale = 1.0f;
    n.display.orientation = n.display.natural_orientation = NUE_ORIENTATION_LANDSCAPE;
    n.display.is_primary = NUE_TRUE;
    n.windows.clear();
    n.pads.clear();
    n.devices.clear();
    n.focused = 0;
    n.mods = 0;
    n.mouse_x = n.mouse_y = 0;
    n.mouse_buttons = 0;
    n.next_pad = 1;
    n.active = true;
    return NUE_OK;
}

void b_shutdown(void) {
    Null& n = N();
    std::vector<std::unique_ptr<NullDevice>> devs;
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        n.active = false;
        devs.swap(n.devices);
        n.windows.clear();
        n.pads.clear();
    }
    for (auto& d : devs) { d->stop = true; if (d->thread.joinable()) d->thread.join(); }
}

int32_t b_pump(void) { return 0; }

int64_t b_time_ns(void) {
    Null& n = N();
    if (n.deterministic) return n.virtual_ns.load();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// video
nue_result v_display_enum(nue_display_info* infos, uint32_t capacity, uint32_t* out_count) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    *out_count = 1;
    if (infos && capacity >= 1) infos[0] = n.display;
    return NUE_OK;
}

nue_result v_display_modes(nue_display_id id, nue_display_mode* modes, uint32_t capacity, uint32_t* out_count) {
    if (id != 1) return NUE_ERR_NOT_FOUND;
    static const int sizes[][2] = {{1920, 1080}, {1600, 900}, {1366, 768}, {1280, 720}, {1024, 768}, {800, 600}, {640, 480}};
    static const float rates[] = {60.0f, 59.94f, 30.0f};
    uint32_t total = 0;
    for (auto& s : sizes)
        for (float r : rates) {
            if (modes && total < capacity) {
                NUE_STRUCT_INIT(&modes[total]);
                modes[total].width = s[0];
                modes[total].height = s[1];
                modes[total].refresh_rate_hz = r;
                modes[total].pixel_density = 1.0f;
            }
            ++total;
        }
    *out_count = total;
    return NUE_OK;
}

nue_result v_window_create(const nue_window_desc* desc, nue_window_id id, void** out_impl, nue_window_snapshot* out_snapshot) {
    Null& n = N();
    std::unique_ptr<NullWindow> w(new NullWindow());
    w->id = id;
    w->title = desc->title ? desc->title : "";
    NUE_STRUCT_INIT(&w->snap);
    NullWindow& W = *w;
    std::lock_guard<std::mutex> lock(n.mutex);
    W.attached = desc->attach && desc->attach->kind != NUE_NATIVE_NONE;
    W.snap.flags = desc->flags;
    W.snap.mode = desc->mode;
    W.snap.display = desc->display ? desc->display : 1;
    W.snap.refresh_rate_hz = n.display.current_mode.refresh_rate_hz;
    int32_t width = desc->width > 0 ? desc->width : 1280, height = desc->height > 0 ? desc->height : 720;
    if (desc->mode != NUE_WINDOW_MODE_WINDOWED) {
        width = desc->mode == NUE_WINDOW_MODE_EXCLUSIVE && desc->fullscreen_mode.width > 0 ? desc->fullscreen_mode.width : n.display.bounds.w;
        height = desc->mode == NUE_WINDOW_MODE_EXCLUSIVE && desc->fullscreen_mode.height > 0 ? desc->fullscreen_mode.height : n.display.bounds.h;
    }
    W.snap.width = width;
    W.snap.height = height;
    W.snap.x = desc->x == NUE_WINDOW_POS_CENTERED || desc->x == NUE_WINDOW_POS_UNDEFINED ? (n.display.bounds.w - width) / 2 : desc->x;
    W.snap.y = desc->y == NUE_WINDOW_POS_CENTERED || desc->y == NUE_WINDOW_POS_UNDEFINED ? (n.display.bounds.h - height) / 2 : desc->y;
    W.min_w = desc->min_width; W.min_h = desc->min_height; W.max_w = desc->max_width; W.max_h = desc->max_height;
    apply_pixel_size(W, n);
    NUE_STRUCT_INIT(&W.snap.native);
    if (W.attached) W.snap.native = *desc->attach;
    else { W.snap.native.kind = kNativeKind; W.snap.native.window = 0x10000u + id; W.snap.native.display = 1; }
    W.snap.visible = (desc->flags & NUE_WINDOW_HIDDEN) ? NUE_FALSE : NUE_TRUE;
    *out_snapshot = W.snap;
    *out_impl = w.get();
    NullWindow* raw = w.get();
    n.windows[id] = std::move(w);
    push_window(NUE_EVENT_SURFACE_CREATED, *raw);
    if (raw->snap.visible) {
        push_window(NUE_EVENT_WINDOW_SHOWN, *raw);
        if (!(desc->flags & NUE_WINDOW_NOT_FOCUSABLE)) set_focus(n, raw);
    }
    return NUE_OK;
}

void v_window_destroy(void* impl) {
    Null& n = N();
    NullWindow* w = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    if (n.focused == w->id) n.focused = 0;
    n.windows.erase(w->id);   // frees w
}

nue_result v_window_refresh(void* impl, nue_window_snapshot* out) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    *out = win_of(impl)->snap;
    return NUE_OK;
}

nue_result v_set_title(void* impl, const char* t) { Null& n = N(); std::lock_guard<std::mutex> lock(n.mutex); win_of(impl)->title = t; return NUE_OK; }

nue_result v_get_title(void* impl, char* buffer, size_t cap, size_t* out_size) {
    Null& n = N();
    std::string t;
    { std::lock_guard<std::mutex> lock(n.mutex); t = win_of(impl)->title; }
    return nuei::copy_out_string(t, buffer, cap, out_size);
}

nue_result v_set_size(void* impl, int32_t w, int32_t h) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    if (W->attached) return NUE_ERR_UNSUPPORTED;   // the host owns an attached window's size
    if (W->min_w) w = std::max<int32_t>(w, int32_t(W->min_w));
    if (W->min_h) h = std::max<int32_t>(h, int32_t(W->min_h));
    if (W->max_w) w = std::min<int32_t>(w, int32_t(W->max_w));
    if (W->max_h) h = std::min<int32_t>(h, int32_t(W->max_h));
    W->snap.width = w; W->snap.height = h;
    apply_pixel_size(*W, n);
    announce_size(*W);
    return NUE_OK;
}

nue_result v_set_position(void* impl, int32_t x, int32_t y) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    W->snap.x = x; W->snap.y = y;
    push_window(NUE_EVENT_WINDOW_MOVED, *W, x, y);
    return NUE_OK;
}

nue_result v_set_limits(void* impl, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    W->min_w = a; W->min_h = b; W->max_w = c; W->max_h = d;
    return NUE_OK;
}

nue_result v_set_flag(void* impl, nue_window_flags flag, nue_bool value) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    if (value) W->snap.flags |= flag; else W->snap.flags &= ~flag;
    return NUE_OK;
}

nue_result v_show(void* impl, nue_bool show) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    if (bool(W->snap.visible) == bool(show)) return NUE_OK_NOOP;
    W->snap.visible = show;
    if (show) W->snap.flags &= ~NUE_WINDOW_HIDDEN; else W->snap.flags |= NUE_WINDOW_HIDDEN;
    push_window(show ? NUE_EVENT_WINDOW_SHOWN : NUE_EVENT_WINDOW_HIDDEN, *W);
    if (!show && n.focused == W->id) set_focus(n, nullptr);
    return NUE_OK;
}

nue_result v_raise(void* impl) { Null& n = N(); std::lock_guard<std::mutex> lock(n.mutex); set_focus(n, win_of(impl)); return NUE_OK; }

nue_result v_minimize(void* impl) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    W->snap.flags |= NUE_WINDOW_MINIMIZED; W->snap.flags &= ~NUE_WINDOW_MAXIMIZED;
    push_window(NUE_EVENT_WINDOW_MINIMIZED, *W);
    if (n.focused == W->id) set_focus(n, nullptr);
    return NUE_OK;
}
nue_result v_maximize(void* impl) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    W->snap.flags |= NUE_WINDOW_MAXIMIZED; W->snap.flags &= ~NUE_WINDOW_MINIMIZED;
    W->snap.width = n.display.usable_bounds.w; W->snap.height = n.display.usable_bounds.h; W->snap.x = 0; W->snap.y = 0;
    apply_pixel_size(*W, n);
    push_window(NUE_EVENT_WINDOW_MAXIMIZED, *W);
    announce_size(*W);
    return NUE_OK;
}
nue_result v_restore(void* impl) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    W->snap.flags &= ~(NUE_WINDOW_MAXIMIZED | NUE_WINDOW_MINIMIZED);
    push_window(NUE_EVENT_WINDOW_RESTORED, *W);
    return NUE_OK;
}
nue_result v_flash(void*, nue_bool) { return NUE_OK; }

nue_result v_set_mode(void* impl, nue_window_mode mode, const nue_display_mode* m) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    if (W->attached) return NUE_ERR_UNSUPPORTED;
    nue_window_mode old = W->snap.mode;
    W->snap.mode = mode;
    if (mode == NUE_WINDOW_MODE_WINDOWED) {
        W->snap.width = 1280; W->snap.height = 720;
        if (old != NUE_WINDOW_MODE_WINDOWED) push_window(NUE_EVENT_WINDOW_LEAVE_FULLSCREEN, *W);
    } else {
        W->snap.width = mode == NUE_WINDOW_MODE_EXCLUSIVE && m && m->width > 0 ? m->width : n.display.bounds.w;
        W->snap.height = mode == NUE_WINDOW_MODE_EXCLUSIVE && m && m->height > 0 ? m->height : n.display.bounds.h;
        W->snap.x = W->snap.y = 0;
        push_window(NUE_EVENT_WINDOW_ENTER_FULLSCREEN, *W);
    }
    apply_pixel_size(*W, n);
    announce_size(*W);
    return NUE_OK;
}

nue_result v_move_display(void* impl, nue_display_id d) {
    Null& n = N();
    if (d != 1) return NUE_ERR_NOT_FOUND;
    NullWindow* W = win_of(impl);
    std::lock_guard<std::mutex> lock(n.mutex);
    W->snap.display = d;
    push_window(NUE_EVENT_WINDOW_DISPLAY_CHANGED, *W, int32_t(d), 0);
    return NUE_OK;
}

nue_result v_clip(void* impl, const nue_rect_i*, nue_bool enabled) { Null& n = N(); std::lock_guard<std::mutex> lock(n.mutex); win_of(impl)->clip = enabled != 0; return NUE_OK; }
nue_result v_relative(void* impl, nue_bool enabled) { Null& n = N(); std::lock_guard<std::mutex> lock(n.mutex); win_of(impl)->snap.relative_mouse = enabled; return NUE_OK; }

nue_result v_warp(void* impl, float x, float y) {
    Null& n = N();
    NullWindow* W = win_of(impl);
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        e.payload.motion.dx = x - n.mouse_x; e.payload.motion.dy = y - n.mouse_y;
        n.mouse_x = x; n.mouse_y = y;
        e.payload.motion.x = x; e.payload.motion.y = y; e.payload.motion.buttons = n.mouse_buttons;
    }
    push(NUE_EVENT_MOUSE_MOTION, W->id, &e);
    return NUE_OK;
}

nue_result v_text_input(void* impl, nue_bool start, const nue_rect_i*, int32_t) { Null& n = N(); std::lock_guard<std::mutex> lock(n.mutex); win_of(impl)->text_input = start != 0; return NUE_OK; }
nue_result v_cursor_visible(nue_bool) { return NUE_OK; }
nue_result v_cursor_kind(nue_cursor_kind) { return NUE_OK; }

const nue_backend_video g_video = {
    v_display_enum, v_display_modes, v_window_create, v_window_destroy, v_window_refresh, v_set_title, v_get_title, v_set_size, v_set_position,
    v_set_limits, v_set_flag, v_show, v_raise, v_minimize, v_maximize, v_restore, v_flash, v_set_mode, v_move_display, v_clip, v_relative, v_warp,
    v_text_input, v_cursor_visible, v_cursor_kind};

// input
NullPad* find_pad(Null& n, nue_gamepad_id id) { for (auto& p : n.pads) if (p->info.id == id) return p.get(); return nullptr; }

nue_result i_gamepad_enum(nue_gamepad_info* infos, uint32_t capacity, uint32_t* out_count) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    *out_count = uint32_t(n.pads.size());
    for (uint32_t i = 0; infos && i < capacity && i < n.pads.size(); ++i) infos[i] = n.pads[i]->info;
    return NUE_OK;
}
nue_result i_gamepad_state(nue_gamepad_id id, nue_gamepad_state* out) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    NullPad* p = find_pad(n, id);
    if (!p) return NUE_ERR_NOT_FOUND;
    *out = p->state;
    out->timestamp_ns = n.deterministic ? n.virtual_ns.load() : 0;
    return NUE_OK;
}
nue_result i_rumble(nue_gamepad_id id, uint16_t low, uint16_t high, uint32_t) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    NullPad* p = find_pad(n, id);
    if (!p) return NUE_ERR_NOT_FOUND;
    p->rumble_low = low; p->rumble_high = high;
    return NUE_OK;
}
nue_result i_add_mapping(const char* line) { return std::strchr(line, ',') ? NUE_OK : NUE_ERR_INVALID_ARGUMENT; }
nue_result i_get_mapping(nue_gamepad_id id, char* buffer, size_t cap, size_t* out_size) {
    Null& n = N();
    std::string m;
    { std::lock_guard<std::mutex> lock(n.mutex); NullPad* p = find_pad(n, id); if (!p) return NUE_ERR_NOT_FOUND; m = p->mapping; }
    return nuei::copy_out_string(m, buffer, cap, out_size);
}
nue_result i_set_player(nue_gamepad_id id, int32_t index) {
    Null& n = N();
    std::lock_guard<std::mutex> lock(n.mutex);
    NullPad* p = find_pad(n, id);
    if (!p) return NUE_ERR_NOT_FOUND;
    p->info.player_index = index;
    return NUE_OK;
}
nue_bool i_true(void) { return NUE_TRUE; }
nue_bool i_false(void) { return NUE_FALSE; }

const nue_backend_input g_input = {
    i_gamepad_enum, i_gamepad_state, i_rumble, nullptr, nullptr, nullptr, nullptr, i_add_mapping, i_get_mapping, i_set_player,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, i_false, nullptr, i_false, i_true, i_false};

// audio
nue_result a_list(nue_bool capture, nue_audio_device_info* infos, uint32_t capacity, uint32_t* out_count) {
    if (capture) { *out_count = 0; return NUE_OK; }
    *out_count = 2;
    for (uint32_t i = 0; infos && i < capacity && i < 2; ++i) {
        NUE_STRUCT_INIT(&infos[i]);
        infos[i].id = i + 1;
        std::snprintf(infos[i].name, sizeof(infos[i].name), i == 0 ? "NUE null output" : "NUE null output (second)");
        infos[i].is_default = i == 0 ? NUE_TRUE : NUE_FALSE;
        infos[i].preferred = {NUE_AUDIO_F32, 2, 48000};
        infos[i].preferred_buffer_frames = 480;
    }
    return NUE_OK;
}

void device_thread(NullDevice* d) {
    // real-time mode: consume one buffer per buffer duration
    const int64_t period_ns = int64_t(d->buffer_frames) * 1000000000ll / d->spec.sample_rate;
    int64_t next = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + period_ns;
    while (!d->stop) {
        int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now < next) { std::this_thread::sleep_for(std::chrono::nanoseconds(next - now)); }
        next += period_ns;
        if (!d->paused) { consume(*d, d->buffer_frames); d->clock_ns += period_ns; }
    }
}

nue_result a_open(const nue_backend_device_open* open, void** out_device, nue_backend_device_info* info) {
    if (open->capture) return nuei::fail(NUE_ERR_UNSUPPORTED, "the null backend has no capture devices");
    if (open->device > 2) return NUE_ERR_NOT_FOUND;
    Null& n = N();
    std::unique_ptr<NullDevice> d(new NullDevice());
    d->open = *open;
    d->spec = open->spec;   // the null device accepts any format exactly
    d->buffer_frames = open->buffer_frames ? open->buffer_frames : (open->low_latency ? 128 : 480);
    NullDevice* raw = d.get();
    bool det;
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        det = n.deterministic;
        n.devices.push_back(std::move(d));
    }
    if (!det) raw->thread = std::thread(device_thread, raw);
    *out_device = raw;
    info->actual = raw->spec;
    info->buffer_frames = raw->buffer_frames;
    info->latency_ns = uint64_t(raw->buffer_frames) * 1000000000ull / raw->spec.sample_rate;
    return NUE_OK;
}

void a_close(void* dev) {
    Null& n = N();
    NullDevice* d = static_cast<NullDevice*>(dev);
    d->stop = true;
    if (d->thread.joinable()) d->thread.join();
    { std::lock_guard<std::mutex> run(d->run_mutex); }   // wait for a callback in flight (deterministic mode)
    std::lock_guard<std::mutex> lock(n.mutex);
    n.devices.erase(std::remove_if(n.devices.begin(), n.devices.end(), [&](const std::unique_ptr<NullDevice>& u) { return u.get() == d; }), n.devices.end());
}

nue_result a_pause(void* dev, nue_bool paused) { static_cast<NullDevice*>(dev)->paused = paused != 0; return NUE_OK; }

nue_result a_info(void* dev, nue_backend_device_info* info) {
    NullDevice* d = static_cast<NullDevice*>(dev);
    info->actual = d->spec;
    info->buffer_frames = d->buffer_frames;
    info->latency_ns = uint64_t(d->buffer_frames) * 1000000000ull / d->spec.sample_rate;
    return NUE_OK;
}

nue_result a_clock(void* dev, uint64_t* frames, int64_t* time_ns) {
    NullDevice* d = static_cast<NullDevice*>(dev);
    *frames = d->frames_played;
    *time_ns = b_time_ns();
    return NUE_OK;
}

const nue_backend_audio g_audio = {a_list, a_open, a_close, a_pause, a_info, a_clock};

nue_backend make_backend() {
    nue_backend b;
    std::memset(&b, 0, sizeof(b));
    b.struct_size = sizeof(b);
    b.name = "null";
    b.init = b_init;
    b.shutdown = b_shutdown;
    b.pump = b_pump;
    b.time_ns = b_time_ns;
    b.video = &g_video;
    b.input = &g_input;
    b.audio = &g_audio;
    const nue_capability caps[] = {NUE_CAP_MULTIPLE_WINDOWS, NUE_CAP_EXCLUSIVE_FULLSCREEN, NUE_CAP_DISPLAY_MODE_CHANGE, NUE_CAP_ATTACH_NATIVE_WINDOW,
                                   NUE_CAP_HIGH_DPI, NUE_CAP_CURSOR_CLIP, NUE_CAP_RELATIVE_MOUSE, NUE_CAP_KEYBOARD, NUE_CAP_MOUSE, NUE_CAP_GAMEPAD,
                                   NUE_CAP_GAMEPAD_RUMBLE, NUE_CAP_AUDIO_OUTPUT, NUE_CAP_AUDIO_DEVICE_SELECTION};
    for (nue_capability c : caps) b.capabilities[c / 64] |= 1ull << (c % 64);
    return b;
}

const nue_backend g_backend = make_backend();

}  // namespace

const nue_backend* nue_backend_null_get(void) { return &g_backend; }

// ---- cross-module hooks --------------------------------------------------------------------------------------------------------

namespace nuei {
void* audio_stream_backend_device(nue_audio_stream stream);   // audio.cpp
void power_null_set_thermal(nue_thermal_state state);         // power.cpp
void power_null_set_power(nue_power_state state, int32_t percent);
}  // namespace nuei

extern "C" {

int32_t nue_null_advance_time_internal(int64_t ns) {
    Null& n = N();
    if (ns <= 0) return 0;
    // time moves in steps no larger than one audio buffer so callbacks see a monotonic clock
    int64_t left = ns;
    while (left > 0) {
        int64_t step = std::min<int64_t>(left, 10000000);   // 10 ms
        n.virtual_ns += step;
        advance_devices(n, step);
        nuei::timers_run_due(n.virtual_ns.load());
        left -= step;
    }
    return 0;
}

#define NULL_ONLY_OR_RETURN()                                                                                   \
    do {                                                                                                        \
        if (nuei::backend() != nue_backend_null_get()) return nuei::fail(NUE_ERR_UNSUPPORTED, "the null backend is not active"); \
    } while (0)

NUE_API nue_result NUE_CALL nue_null_advance_time(int64_t ns) {
    NULL_ONLY_OR_RETURN();
    if (!nuei::deterministic()) return nuei::fail(NUE_ERR_BAD_STATE, "nue_null_advance_time needs NUE_INIT_DETERMINISTIC");
    nue_null_advance_time_internal(ns);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_inject_event(const nue_event* event) {
    NULL_ONLY_OR_RETURN();
    if (!event) return NUE_ERR_INVALID_ARGUMENT;
    nue_internal_push_event(event);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_press_key(nue_scancode sc, nue_bool down) {
    NULL_ONLY_OR_RETURN();
    Null& n = N();
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    nue_window_id focus;
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        nue_keymod bit = 0;
        switch (sc) {
            case NUE_SC_LSHIFT: bit = NUE_KMOD_LSHIFT; break; case NUE_SC_RSHIFT: bit = NUE_KMOD_RSHIFT; break;
            case NUE_SC_LCTRL: bit = NUE_KMOD_LCTRL; break; case NUE_SC_RCTRL: bit = NUE_KMOD_RCTRL; break;
            case NUE_SC_LALT: bit = NUE_KMOD_LALT; break; case NUE_SC_RALT: bit = NUE_KMOD_RALT; break;
            case NUE_SC_LGUI: bit = NUE_KMOD_LGUI; break; case NUE_SC_RGUI: bit = NUE_KMOD_RGUI; break;
            default: break;
        }
        if (down) n.mods |= bit; else n.mods &= nue_keymod(~bit);
        e.payload.key.mods = n.mods;
        focus = n.focused;
    }
    e.payload.key.scancode = sc;
    e.payload.key.keycode = nue_key_from_scancode(sc);
    e.payload.key.down = down ? 1 : 0;
    push(down ? NUE_EVENT_KEY_DOWN : NUE_EVENT_KEY_UP, focus, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_move_mouse(float x, float y) {
    NULL_ONLY_OR_RETURN();
    Null& n = N();
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    nue_window_id focus;
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        e.payload.motion.dx = x - n.mouse_x; e.payload.motion.dy = y - n.mouse_y;
        n.mouse_x = x; n.mouse_y = y;
        e.payload.motion.x = x; e.payload.motion.y = y; e.payload.motion.buttons = n.mouse_buttons;
        focus = n.focused;
    }
    push(NUE_EVENT_MOUSE_MOTION, focus, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_press_mouse(nue_mouse_button button, nue_bool down) {
    NULL_ONLY_OR_RETURN();
    Null& n = N();
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    nue_window_id focus;
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        uint32_t bit = NUE_MOUSE_BUTTON_MASK(button);
        if (down) n.mouse_buttons |= bit; else n.mouse_buttons &= ~bit;
        e.payload.button.x = n.mouse_x; e.payload.button.y = n.mouse_y;
        focus = n.focused;
    }
    e.payload.button.button = uint8_t(button);
    e.payload.button.down = down ? 1 : 0;
    e.payload.button.clicks = 1;
    push(down ? NUE_EVENT_MOUSE_BUTTON_DOWN : NUE_EVENT_MOUSE_BUTTON_UP, focus, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_add_gamepad(const char* name, nue_gamepad_type type, nue_gamepad_id* out_id) {
    NULL_ONLY_OR_RETURN();
    Null& n = N();
    std::unique_ptr<NullPad> p(new NullPad());
    NUE_STRUCT_INIT(&p->info);
    NUE_STRUCT_INIT(&p->state);
    std::snprintf(p->info.name, sizeof(p->info.name), "%s", name ? name : "NUE null gamepad");
    p->info.type = type;
    switch (type) {
        case NUE_PAD_TYPE_XBOX360: case NUE_PAD_TYPE_XBOXONE: p->info.glyphs = NUE_GLYPHS_XBOX; break;
        case NUE_PAD_TYPE_PS3: case NUE_PAD_TYPE_PS4: case NUE_PAD_TYPE_PS5: p->info.glyphs = NUE_GLYPHS_PLAYSTATION; break;
        case NUE_PAD_TYPE_SWITCH_PRO: case NUE_PAD_TYPE_SWITCH_JOYCON_LEFT: case NUE_PAD_TYPE_SWITCH_JOYCON_RIGHT: case NUE_PAD_TYPE_SWITCH_JOYCON_PAIR:
            p->info.glyphs = NUE_GLYPHS_NINTENDO; break;
        default: p->info.glyphs = NUE_GLYPHS_GENERIC_GAMEPAD; break;
    }
    p->info.caps = NUE_PADCAP_RUMBLE | NUE_PADCAP_HAS_MAPPING;
    p->info.player_index = -1;
    p->info.battery_percent = -1;
    p->info.joystick_axes = NUE_PAD_AXIS_COUNT;
    p->info.joystick_buttons = NUE_PAD_BUTTON_COUNT;
    p->info.connected = NUE_TRUE;
    p->mapping = "00000000000000000000000000000000," + std::string(p->info.name) + ",a:b0,b:b1,x:b2,y:b3,";
    nue_gamepad_id id;
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        id = n.next_pad++;
        p->info.id = id;
        n.pads.push_back(std::move(p));
    }
    if (out_id) *out_id = id;
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.gamepad.gamepad_id = id;
    push(NUE_EVENT_GAMEPAD_ADDED, 0, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_remove_gamepad(nue_gamepad_id id) {
    NULL_ONLY_OR_RETURN();
    Null& n = N();
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        auto it = std::find_if(n.pads.begin(), n.pads.end(), [&](const std::unique_ptr<NullPad>& p) { return p->info.id == id; });
        if (it == n.pads.end()) return NUE_ERR_NOT_FOUND;
        n.pads.erase(it);
    }
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.gamepad.gamepad_id = id;
    push(NUE_EVENT_GAMEPAD_REMOVED, 0, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_set_gamepad_button(nue_gamepad_id id, nue_gamepad_button button, nue_bool down) {
    NULL_ONLY_OR_RETURN();
    if (button < 0 || button >= NUE_PAD_BUTTON_COUNT) return NUE_ERR_INVALID_ARGUMENT;
    Null& n = N();
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        NullPad* p = find_pad(n, id);
        if (!p) return NUE_ERR_NOT_FOUND;
        if (down) p->state.buttons |= 1u << button; else p->state.buttons &= ~(1u << button);
    }
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.gamepad.gamepad_id = id;
    e.payload.gamepad.control = button;
    e.payload.gamepad.value = down ? 1 : 0;
    push(down ? NUE_EVENT_GAMEPAD_BUTTON_DOWN : NUE_EVENT_GAMEPAD_BUTTON_UP, 0, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_set_gamepad_axis(nue_gamepad_id id, nue_gamepad_axis axis, int16_t value) {
    NULL_ONLY_OR_RETURN();
    if (axis < 0 || axis >= NUE_PAD_AXIS_COUNT) return NUE_ERR_INVALID_ARGUMENT;
    Null& n = N();
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        NullPad* p = find_pad(n, id);
        if (!p) return NUE_ERR_NOT_FOUND;
        p->state.axes[axis] = value;
    }
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.gamepad.gamepad_id = id;
    e.payload.gamepad.control = axis;
    e.payload.gamepad.value = value;
    push(NUE_EVENT_GAMEPAD_AXIS, 0, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_touch(uint64_t finger_id, float x, float y, int32_t phase) {
    NULL_ONLY_OR_RETURN();
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.finger.touch_id = 1;
    e.payload.finger.finger_id = finger_id;
    e.payload.finger.x = x;
    e.payload.finger.y = y;
    e.payload.finger.pressure = phase == 2 ? 0.0f : 1.0f;
    push(phase == 0 ? NUE_EVENT_FINGER_DOWN : phase == 1 ? NUE_EVENT_FINGER_MOTION : NUE_EVENT_FINGER_UP, N().focused, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_set_app_state(nue_app_state state) {
    NULL_ONLY_OR_RETURN();
    nue_internal_set_app_state(state);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_set_thermal_state(nue_thermal_state state) { NULL_ONLY_OR_RETURN(); nuei::power_null_set_thermal(state); return NUE_OK; }
NUE_API nue_result NUE_CALL nue_null_set_power(nue_power_state state, int32_t percent) { NULL_ONLY_OR_RETURN(); nuei::power_null_set_power(state, percent); return NUE_OK; }

NUE_API nue_result NUE_CALL nue_null_set_memory(const nue_memory_info* info) {
    NULL_ONLY_OR_RETURN();
    nuei::memory_null_set(info);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_signal_memory_pressure(nue_memory_pressure level, uint64_t available_bytes) {
    NULL_ONLY_OR_RETURN();
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.memory.level = uint32_t(level);
    e.payload.memory.available_bytes = available_bytes;
    push(NUE_EVENT_MEMORY_PRESSURE, 0, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_system_suspend(nue_bool suspending) {
    NULL_ONLY_OR_RETURN();
    push(suspending ? NUE_EVENT_SYSTEM_SUSPENDING : NUE_EVENT_SYSTEM_RESUMED, 0);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_audio_interruption(nue_bool begin) {
    NULL_ONLY_OR_RETURN();
    push(begin ? NUE_EVENT_AUDIO_INTERRUPTED : NUE_EVENT_AUDIO_RESUMED, 0);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_set_display(uint32_t width, uint32_t height, float refresh_hz, float content_scale) {
    NULL_ONLY_OR_RETURN();
    if (!width || !height || refresh_hz <= 0 || content_scale <= 0) return NUE_ERR_INVALID_ARGUMENT;
    Null& n = N();
    {
        std::lock_guard<std::mutex> lock(n.mutex);
        n.display.bounds = {0, 0, int32_t(width), int32_t(height)};
        n.display.usable_bounds = {0, 0, int32_t(width), int32_t(height) - 40};
        n.display.current_mode.width = int32_t(width);
        n.display.current_mode.height = int32_t(height);
        n.display.current_mode.refresh_rate_hz = refresh_hz;
        n.display.desktop_mode = n.display.current_mode;
        n.display.content_scale = content_scale;
        n.display.orientation = n.display.natural_orientation = width >= height ? NUE_ORIENTATION_LANDSCAPE : NUE_ORIENTATION_PORTRAIT;
        for (auto& kv : n.windows) {
            NullWindow& w = *kv.second;
            w.snap.refresh_rate_hz = refresh_hz;
            w.snap.content_scale = content_scale;
            if (w.snap.mode != NUE_WINDOW_MODE_WINDOWED) { w.snap.width = int32_t(width); w.snap.height = int32_t(height); }
            apply_pixel_size(w, n);
            push_window(NUE_EVENT_WINDOW_CONTENT_SCALE_CHANGED, w, 0, 0, content_scale);
            announce_size(w);
        }
    }
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.payload.display.display_id = 1;
    e.payload.display.data1 = int32_t(width);
    e.payload.display.data2 = int32_t(height);
    push(NUE_EVENT_DISPLAY_CHANGED, 0, &e);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_resize_window(nue_window window, int32_t width, int32_t height) {
    NULL_ONLY_OR_RETURN();
    Null& n = N();
    nue_window_id id = nue_window_get_id(window);
    std::lock_guard<std::mutex> lock(n.mutex);
    auto it = n.windows.find(id);
    if (it == n.windows.end()) return NUE_ERR_INVALID_HANDLE;
    NullWindow& w = *it->second;
    w.snap.width = width; w.snap.height = height;
    apply_pixel_size(w, n);
    announce_size(w);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_request_close(nue_window window) {
    NULL_ONLY_OR_RETURN();
    nue_window_id id = nue_window_get_id(window);
    if (!id) return NUE_ERR_INVALID_HANDLE;
    push(NUE_EVENT_WINDOW_CLOSE_REQUESTED, id);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_audio_set_capture(nue_audio_stream stream, uint32_t capture_frames) {
    NULL_ONLY_OR_RETURN();
    NullDevice* d = static_cast<NullDevice*>(nuei::audio_stream_backend_device(stream));
    if (!d) return NUE_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(d->run_mutex);
    d->capture_frames = capture_frames;
    d->capture.clear();
    d->capture_total = 0;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_null_audio_read_played(nue_audio_stream stream, void* out, uint32_t max_frames, uint32_t* out_frames) {
    NULL_ONLY_OR_RETURN();
    NullDevice* d = static_cast<NullDevice*>(nuei::audio_stream_backend_device(stream));
    if (!d) return NUE_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(d->run_mutex);
    const uint32_t fsz = bytes_per_frame(d->spec);
    uint64_t have = std::min<uint64_t>(d->capture_total, d->capture_frames);
    uint32_t n = uint32_t(std::min<uint64_t>(have, max_frames));
    uint64_t first = d->capture_total - have;   // oldest retained frame
    for (uint32_t i = 0; i < n; ++i) {
        size_t slot = size_t((first + i) % d->capture_frames);
        std::memcpy(static_cast<uint8_t*>(out) + size_t(i) * fsz, &d->capture[slot * fsz], fsz);
    }
    if (out_frames) *out_frames = n;
    return NUE_OK;
}

}  // extern "C"
