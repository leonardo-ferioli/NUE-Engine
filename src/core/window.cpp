// Displays, windows and cursor: bookkeeping and validation in the core, devices in the backend.
#include "internal/core_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

struct nue_window_s {
    nue_window_id id = 0;
    void* impl = nullptr;
    nue_window_snapshot snap;
    bool attached = false;
};

namespace {

struct WindowTable {
    std::mutex mutex;
    std::map<nue_window_id, nue_window_s*> by_id;
    nue_window_id next_id = 1;
    nue_window focused = nullptr;
    std::vector<void (*)(nue_window)> destroy_hooks;
    bool cursor_visible = true;
};

WindowTable& wt() {
    static WindowTable* t = new WindowTable();
    return *t;
}

const nue_backend_video* video() {
    const nue_backend* b = nuei::backend();
    return b ? b->video : nullptr;
}

nue_window_s* find_locked(WindowTable& t, nue_window w) {
    for (auto& kv : t.by_id) if (kv.second == w) return kv.second;
    return nullptr;
}

#define NO_VIDEO_OR_RETURN(v)                                                                                       \
    const nue_backend_video* v = video();                                                                          \
    if (!v) return nuei::fail(NUE_ERR_UNSUPPORTED, "this backend has no video support")

// Copies a snapshot of `w` if the handle is alive.
bool get_snapshot(nue_window w, nue_window_snapshot* out, void** impl = nullptr) {
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    nue_window_s* s = find_locked(t, w);
    if (!s) return false;
    if (out) *out = s->snap;
    if (impl) *impl = s->impl;
    return true;
}

void refresh_one(nue_window w) {
    const nue_backend_video* v = video();
    if (!v || !v->window_refresh) return;
    void* impl = nullptr;
    if (!get_snapshot(w, nullptr, &impl)) return;
    nue_window_snapshot snap;
    NUE_STRUCT_INIT(&snap);
    if (NUE_FAILED(v->window_refresh(impl, &snap))) return;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->window_safe_area) { nue_rect_i r; if (NUE_SUCCEEDED(p->window_safe_area(&snap.native, &r))) snap.safe_area = r; }
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    if (nue_window_s* s = find_locked(t, w)) s->snap = snap;
}

}  // namespace

namespace nuei {

void windows_after_pump() {
    std::vector<nue_window> all;
    { WindowTable& t = wt(); std::lock_guard<std::mutex> lock(t.mutex); for (auto& kv : t.by_id) all.push_back(kv.second); }
    for (nue_window w : all) refresh_one(w);
}

void* window_impl_or_null(nue_window w) {
    void* impl = nullptr;
    return get_snapshot(w, nullptr, &impl) ? impl : nullptr;
}

void windows_add_destroy_hook(void (*fn)(nue_window)) {
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    t.destroy_hooks.push_back(fn);
}

void windows_shutdown() {
    std::vector<nue_window> all;
    { WindowTable& t = wt(); std::lock_guard<std::mutex> lock(t.mutex); for (auto& kv : t.by_id) all.push_back(kv.second); }
    if (!all.empty()) NUE_LOGW("window", "%zu window(s) were still open at shutdown and are being closed", all.size());
    for (nue_window w : all) nue_window_destroy(w);
    // cursor state must never leak out of the process: show it again
    const nue_backend_video* v = video();
    if (v && v->cursor_set_visible && !wt().cursor_visible) v->cursor_set_visible(NUE_TRUE);
    wt().cursor_visible = true;
}

}  // namespace nuei

extern "C" {

// ---- displays -------------------------------------------------------------------------------------------------------------------

static std::vector<nue_display_info> enum_displays() {
    std::vector<nue_display_info> v;
    const nue_backend_video* vid = video();
    if (!vid || !vid->display_enum) return v;
    uint32_t count = 0;
    nue_display_info probe[1];
    NUE_STRUCT_INIT(&probe[0]);
    if (NUE_FAILED(vid->display_enum(probe, 0, &count)) || count == 0) return v;
    v.resize(count);
    for (auto& d : v) NUE_STRUCT_INIT(&d);
    uint32_t got = 0;
    if (NUE_FAILED(vid->display_enum(v.data(), count, &got))) { v.clear(); return v; }
    v.resize(std::min(got, count));
    return v;
}

NUE_API nue_result NUE_CALL nue_display_list(nue_display_id* ids, uint32_t capacity, uint32_t* out_count) {
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    NO_VIDEO_OR_RETURN(vid);
    (void)vid;
    std::vector<nue_display_info> v = enum_displays();
    if (out_count) *out_count = uint32_t(v.size());
    for (uint32_t i = 0; ids && i < capacity && i < v.size(); ++i) ids[i] = v[i].id;
    return NUE_OK;
}

NUE_API nue_display_id NUE_CALL nue_display_primary(void) {
    if (!nuei::initialized()) return 0;
    std::vector<nue_display_info> v = enum_displays();
    for (const auto& d : v) if (d.is_primary) return d.id;
    return v.empty() ? 0 : v[0].id;
}

NUE_API nue_result NUE_CALL nue_display_get_info(nue_display_id id, nue_display_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    NO_VIDEO_OR_RETURN(vid);
    (void)vid;
    for (const auto& d : enum_displays()) {
        if (d.id != id) continue;
        uint32_t keep = out->struct_size;
        std::memcpy(out, &d, keep < sizeof(d) ? keep : sizeof(d));
        out->struct_size = keep;
        return NUE_OK;
    }
    return nuei::fail(NUE_ERR_NOT_FOUND, "display %u does not exist", id);
}

NUE_API nue_result NUE_CALL nue_display_list_modes(nue_display_id id, nue_display_mode* modes, uint32_t capacity, uint32_t* out_count) {
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    NO_VIDEO_OR_RETURN(vid);
    if (!vid->display_modes) return NUE_ERR_UNSUPPORTED;
    uint32_t count = 0;
    nue_display_mode probe;
    NUE_STRUCT_INIT(&probe);
    nue_result r = vid->display_modes(id, &probe, 0, &count);
    if (NUE_FAILED(r)) return r;
    std::vector<nue_display_mode> v(count);
    for (auto& m : v) NUE_STRUCT_INIT(&m);
    uint32_t got = 0;
    if (count) { r = vid->display_modes(id, v.data(), count, &got); if (NUE_FAILED(r)) return r; v.resize(std::min(got, count)); }
    std::stable_sort(v.begin(), v.end(), [](const nue_display_mode& a, const nue_display_mode& b) {
        if (a.width != b.width) return a.width > b.width;
        if (a.height != b.height) return a.height > b.height;
        return a.refresh_rate_hz > b.refresh_rate_hz;
    });
    if (out_count) *out_count = uint32_t(v.size());
    for (uint32_t i = 0; modes && i < capacity && i < v.size(); ++i) modes[i] = v[i];
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_display_find_mode(nue_display_id id, int32_t width, int32_t height, float refresh, nue_display_mode* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    uint32_t count = 0;
    nue_result r = nue_display_list_modes(id, nullptr, 0, &count);
    if (NUE_FAILED(r)) return r;
    if (count == 0) return nuei::fail(NUE_ERR_NOT_FOUND, "display %u reports no modes", id);
    std::vector<nue_display_mode> v(count);
    for (auto& m : v) NUE_STRUCT_INIT(&m);
    r = nue_display_list_modes(id, v.data(), count, &count);
    if (NUE_FAILED(r)) return r;
    double best = 1e30;
    const nue_display_mode* pick = &v[0];
    for (uint32_t i = 0; i < count; ++i) {
        double dw = double(v[i].width) - width, dh = double(v[i].height) - height;
        double score = dw * dw + dh * dh;
        if (refresh > 0) { double dr = double(v[i].refresh_rate_hz) - refresh; score += dr * dr * 1000.0; }
        else score -= double(v[i].refresh_rate_hz) * 0.001;   // prefer the higher rate among equals
        if (score < best) { best = score; pick = &v[i]; }
    }
    *out = *pick;
    return NUE_OK;
}

NUE_API nue_display_id NUE_CALL nue_display_from_point(nue_point_i p) {
    for (const auto& d : enum_displays())
        if (p.x >= d.bounds.x && p.y >= d.bounds.y && p.x < d.bounds.x + d.bounds.w && p.y < d.bounds.y + d.bounds.h) return d.id;
    return 0;
}

// ---- windows --------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_window_create(const nue_window_desc* desc, nue_window* out_window) {
    if (!desc || !out_window) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_window_create: desc and out_window are required");
    *out_window = nullptr;
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    if (!nuei::subsystem_enabled(NUE_SUBSYSTEM_VIDEO)) return nuei::fail(NUE_ERR_BAD_STATE, "the video subsystem was not requested at nue_init");
    NO_VIDEO_OR_RETURN(vid);
    WindowTable& t = wt();
    nue_window_s* w = new (std::nothrow) nue_window_s();
    if (!w) return NUE_ERR_OUT_OF_MEMORY;
    { std::lock_guard<std::mutex> lock(t.mutex); w->id = t.next_id++; }
    NUE_STRUCT_INIT(&w->snap);
    w->attached = desc->attach && desc->attach->kind != NUE_NATIVE_NONE;
    nue_result r = vid->window_create(desc, w->id, &w->impl, &w->snap);
    if (NUE_FAILED(r)) { delete w; return r; }
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        t.by_id[w->id] = w;
    }
    *out_window = w;
    refresh_one(w);
    NUE_LOGI("window", "window %u created (%dx%d px, %s)", w->id, w->snap.pixel_width, w->snap.pixel_height, w->attached ? "attached" : "owned");
    return NUE_OK;
}

NUE_API void NUE_CALL nue_window_destroy(nue_window window) {
    WindowTable& t = wt();
    std::vector<void (*)(nue_window)> hooks;
    void* impl = nullptr;
    nue_window_id id = 0;
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        nue_window_s* s = find_locked(t, window);
        if (!s) return;
        impl = s->impl;
        id = s->id;
        hooks = t.destroy_hooks;
        if (t.focused == window) t.focused = nullptr;
    }
    for (auto h : hooks) h(window);   // gfx contexts bound to this window let go first
    const nue_backend_video* vid = video();
    if (vid && vid->window_destroy) vid->window_destroy(impl);
    {
        std::lock_guard<std::mutex> lock(t.mutex);
        t.by_id.erase(id);
    }
    delete window;
}

NUE_API nue_window_id NUE_CALL nue_window_get_id(nue_window window) {
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    nue_window_s* s = find_locked(t, window);
    return s ? s->id : 0;
}

NUE_API nue_window NUE_CALL nue_window_from_id(nue_window_id id) {
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto it = t.by_id.find(id);
    return it == t.by_id.end() ? nullptr : it->second;
}

NUE_API nue_window NUE_CALL nue_window_get_focused(void) {
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    for (auto& kv : t.by_id) if (kv.second->snap.focused) return kv.second;
    return nullptr;
}

// Runs a backend op on a live window, then refreshes the snapshot.
#define WINDOW_OP(window, opname, ...)                                                                              \
    do {                                                                                                          \
        NO_VIDEO_OR_RETURN(vid_);                                                                                  \
        void* impl_ = nullptr;                                                                                     \
        if (!get_snapshot((window), nullptr, &impl_)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle"); \
        if (!vid_->opname) return NUE_ERR_UNSUPPORTED;                                                             \
        nue_result r_ = vid_->opname(impl_, ##__VA_ARGS__);                                                        \
        if (NUE_SUCCEEDED(r_)) refresh_one(window);                                                                \
        return r_;                                                                                                 \
    } while (0)

NUE_API nue_result NUE_CALL nue_window_set_title(nue_window w, const char* title) { WINDOW_OP(w, window_set_title, title ? title : ""); }

NUE_API nue_result NUE_CALL nue_window_get_title(nue_window w, char* buffer, size_t capacity, size_t* out_size) {
    NO_VIDEO_OR_RETURN(vid);
    void* impl = nullptr;
    if (!get_snapshot(w, nullptr, &impl)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    if (!vid->window_get_title) return NUE_ERR_UNSUPPORTED;
    return vid->window_get_title(impl, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_window_set_size(nue_window w, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "window size must be positive");
    WINDOW_OP(w, window_set_size, width, height);
}
NUE_API nue_result NUE_CALL nue_window_set_position(nue_window w, int32_t x, int32_t y) { WINDOW_OP(w, window_set_position, x, y); }
NUE_API nue_result NUE_CALL nue_window_set_size_limits(nue_window w, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { WINDOW_OP(w, window_set_size_limits, a, b, c, d); }
NUE_API nue_result NUE_CALL nue_window_set_resizable(nue_window w, nue_bool v) { WINDOW_OP(w, window_set_flag, NUE_WINDOW_RESIZABLE, v); }
NUE_API nue_result NUE_CALL nue_window_set_bordered(nue_window w, nue_bool v) { WINDOW_OP(w, window_set_flag, NUE_WINDOW_BORDERLESS, v ? NUE_FALSE : NUE_TRUE); }
NUE_API nue_result NUE_CALL nue_window_set_always_on_top(nue_window w, nue_bool v) { WINDOW_OP(w, window_set_flag, NUE_WINDOW_ALWAYS_ON_TOP, v); }
NUE_API nue_result NUE_CALL nue_window_show(nue_window w) { WINDOW_OP(w, window_show, NUE_TRUE); }
NUE_API nue_result NUE_CALL nue_window_hide(nue_window w) { WINDOW_OP(w, window_show, NUE_FALSE); }
NUE_API nue_result NUE_CALL nue_window_raise(nue_window w) { WINDOW_OP(w, window_raise); }
NUE_API nue_result NUE_CALL nue_window_minimize(nue_window w) { WINDOW_OP(w, window_minimize); }
NUE_API nue_result NUE_CALL nue_window_maximize(nue_window w) { WINDOW_OP(w, window_maximize); }
NUE_API nue_result NUE_CALL nue_window_restore(nue_window w) { WINDOW_OP(w, window_restore); }
NUE_API nue_result NUE_CALL nue_window_flash(nue_window w, nue_bool until_focused) { WINDOW_OP(w, window_flash, until_focused); }
NUE_API nue_result NUE_CALL nue_window_set_mode(nue_window w, nue_window_mode mode, const nue_display_mode* m) { WINDOW_OP(w, window_set_mode, mode, m); }
NUE_API nue_result NUE_CALL nue_window_move_to_display(nue_window w, nue_display_id d) { WINDOW_OP(w, window_move_to_display, d); }
NUE_API nue_result NUE_CALL nue_window_set_cursor_clip(nue_window w, const nue_rect_i* r, nue_bool enabled) { WINDOW_OP(w, window_set_cursor_clip, r, enabled); }
NUE_API nue_result NUE_CALL nue_window_set_relative_mouse(nue_window w, nue_bool enabled) { WINDOW_OP(w, window_set_relative_mouse, enabled); }
NUE_API nue_result NUE_CALL nue_window_warp_mouse(nue_window w, float x, float y) { WINDOW_OP(w, window_warp_mouse, x, y); }

NUE_API nue_bool NUE_CALL nue_window_get_relative_mouse(nue_window w) {
    nue_window_snapshot s;
    return get_snapshot(w, &s) && s.relative_mouse ? NUE_TRUE : NUE_FALSE;
}

NUE_API nue_window_mode NUE_CALL nue_window_get_mode(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) ? s.mode : NUE_WINDOW_MODE_WINDOWED; }
NUE_API nue_window_flags NUE_CALL nue_window_get_flags(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) ? s.flags : 0; }

NUE_API nue_result NUE_CALL nue_window_get_size(nue_window w, int32_t* width, int32_t* height) {
    nue_window_snapshot s;
    if (!get_snapshot(w, &s)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    if (width) *width = s.width;
    if (height) *height = s.height;
    return NUE_OK;
}
NUE_API nue_result NUE_CALL nue_window_get_pixel_size(nue_window w, int32_t* width, int32_t* height) {
    nue_window_snapshot s;
    if (!get_snapshot(w, &s)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    if (width) *width = s.pixel_width;
    if (height) *height = s.pixel_height;
    return NUE_OK;
}
NUE_API nue_result NUE_CALL nue_window_get_position(nue_window w, int32_t* x, int32_t* y) {
    nue_window_snapshot s;
    if (!get_snapshot(w, &s)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    if (x) *x = s.x;
    if (y) *y = s.y;
    return NUE_OK;
}
NUE_API float NUE_CALL nue_window_get_content_scale(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) ? s.content_scale : 1.0f; }
NUE_API float NUE_CALL nue_window_get_pixel_density(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) ? s.pixel_density : 1.0f; }
NUE_API nue_display_id NUE_CALL nue_window_get_display(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) ? s.display : 0; }
NUE_API float NUE_CALL nue_window_get_refresh_rate(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) ? s.refresh_rate_hz : 0.0f; }
NUE_API nue_bool NUE_CALL nue_window_has_focus(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) && s.focused ? NUE_TRUE : NUE_FALSE; }
NUE_API nue_bool NUE_CALL nue_window_is_visible(nue_window w) { nue_window_snapshot s; return get_snapshot(w, &s) && s.visible ? NUE_TRUE : NUE_FALSE; }

NUE_API nue_result NUE_CALL nue_window_get_safe_area(nue_window w, nue_rect_i* out) {
    nue_window_snapshot s;
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    if (!get_snapshot(w, &s)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    *out = s.safe_area;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_window_get_native(nue_window w, nue_native_window* out) {
    nue_window_snapshot s;
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    if (!get_snapshot(w, &s)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    uint32_t keep = out->struct_size;
    std::memcpy(out, &s.native, keep < sizeof(s.native) ? keep : sizeof(s.native));
    out->struct_size = keep;
    return NUE_OK;
}

// ---- cursor ---------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_cursor_set_visible(nue_bool visible) {
    NO_VIDEO_OR_RETURN(vid);
    if (!vid->cursor_set_visible) return NUE_ERR_UNSUPPORTED;
    nue_result r = vid->cursor_set_visible(visible);
    if (NUE_SUCCEEDED(r)) { WindowTable& t = wt(); std::lock_guard<std::mutex> lock(t.mutex); t.cursor_visible = visible != 0; }
    return r;
}
NUE_API nue_bool NUE_CALL nue_cursor_is_visible(void) { WindowTable& t = wt(); std::lock_guard<std::mutex> lock(t.mutex); return t.cursor_visible ? NUE_TRUE : NUE_FALSE; }
NUE_API nue_result NUE_CALL nue_cursor_set_kind(nue_cursor_kind kind) {
    NO_VIDEO_OR_RETURN(vid);
    return vid->cursor_set_kind ? vid->cursor_set_kind(kind) : NUE_ERR_UNSUPPORTED;
}

}  // extern "C"

// the owner table is also needed by the backend glue
nue_window_id nue_internal_window_id_from_impl(void* impl) {
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    for (auto& kv : t.by_id) if (kv.second->impl == impl) return kv.first;
    return 0;
}

void nue_internal_window_changed(nue_window_id id, const nue_window_snapshot* snapshot) {
    if (!snapshot) return;
    WindowTable& t = wt();
    std::lock_guard<std::mutex> lock(t.mutex);
    auto it = t.by_id.find(id);
    if (it != t.by_id.end()) it->second->snap = *snapshot;
}
