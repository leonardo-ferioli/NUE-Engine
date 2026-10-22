// Graphics context lifecycle (nue_gfx): renderer selection, resize / suspend handling, listeners, native handles.
// The bgfx calls themselves (bgfx::init / reset / frame in MANAGED mode) arrive with the NUE_WITH_GFX module; PREPARE mode and the NOOP
// renderer are complete here, so the Direct3D 9 layer can already take its surface from nue_gfx_get_native().
#include "internal/core_internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

struct nue_gfx_s {
    nue_window window = nullptr;
    nue_window_id window_id = 0;
    nue_renderer renderer = NUE_RENDERER_NOOP;
    nue_gfx_flags flags = 0;
    bool follow_window = true;
    int32_t width = 0, height = 0;
    bool suspended = false;
    bool orphaned = false;          // the window was destroyed under us
    bool bgfx_initialized = false;
    bool auto_events = false;
    bool vsync = false;
    uint32_t frame = 0;
    std::vector<std::pair<nue_gfx_listener_fn, void*>> listeners;
};

namespace {

struct GfxRegistry {
    std::mutex mutex;
    std::vector<nue_gfx> all;
    bool hook_installed = false;
    bool watcher_installed = false;
};
GfxRegistry& G() { static GfxRegistry* g = new GfxRegistry(); return *g; }

bool known(nue_gfx g) { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); return std::find(r.all.begin(), r.all.end(), g) != r.all.end(); }

void notify(nue_gfx g, nue_gfx_notification what, int32_t a, int32_t b) {
    std::vector<std::pair<nue_gfx_listener_fn, void*>> ls;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); ls = g->listeners; }
    for (auto& l : ls) l.first(l.second, g, what, a, b);
}

void window_destroying(nue_window w) {
    std::vector<nue_gfx> affected;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); for (nue_gfx g : r.all) if (g->window == w) affected.push_back(g); }
    for (nue_gfx g : affected) { notify(g, NUE_GFX_NOTIFY_DESTROYING, 0, 0); g->orphaned = true; g->window = nullptr; }
}

nue_bool NUE_CALL gfx_watcher(void*, const nue_event* e) {
    std::vector<nue_gfx> targets;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); for (nue_gfx g : r.all) if (g->auto_events) targets.push_back(g); }
    for (nue_gfx g : targets) nue_gfx_handle_event(g, e);
    return NUE_TRUE;
}

const nue_renderer* platform_order(size_t* n) {
#if defined(NUE_PLATFORM_WINDOWS)
    static const nue_renderer o[] = {NUE_RENDERER_D3D11, NUE_RENDERER_VULKAN, NUE_RENDERER_D3D12, NUE_RENDERER_OPENGL};
#elif defined(NUE_PLATFORM_LINUX)
    static const nue_renderer o[] = {NUE_RENDERER_VULKAN, NUE_RENDERER_OPENGL, NUE_RENDERER_GLES};
#elif defined(NUE_PLATFORM_ANDROID)
    static const nue_renderer o[] = {NUE_RENDERER_VULKAN, NUE_RENDERER_GLES};
#else
    static const nue_renderer o[] = {NUE_RENDERER_METAL};
#endif
    *n = sizeof(o) / sizeof(o[0]);
    return o;
}

}  // namespace

namespace nuei {
void gfx_shutdown_all() {
    std::vector<nue_gfx> left;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); left = r.all; r.hook_installed = false; r.watcher_installed = false; }
    if (!left.empty()) NUE_LOGW("gfx", "%zu graphics context(s) were still open at shutdown and are being destroyed", left.size());
    for (nue_gfx g : left) nue_gfx_destroy(g);
}
}  // namespace nuei

extern "C" {

NUE_API const char* NUE_CALL nue_renderer_name(nue_renderer r) {
    switch (r) {
        case NUE_RENDERER_AUTO: return "Auto";
        case NUE_RENDERER_NOOP: return "Noop";
        case NUE_RENDERER_D3D11: return "Direct3D 11";
        case NUE_RENDERER_D3D12: return "Direct3D 12";
        case NUE_RENDERER_VULKAN: return "Vulkan";
        case NUE_RENDERER_METAL: return "Metal";
        case NUE_RENDERER_OPENGL: return "OpenGL";
        case NUE_RENDERER_GLES: return "OpenGL ES";
    }
    return "Unknown";
}

NUE_API nue_result NUE_CALL nue_gfx_list_renderers(nue_renderer* out, uint32_t capacity, uint32_t* out_count) {
    size_t n = 0;
    const nue_renderer* order = platform_order(&n);
    if (out_count) *out_count = uint32_t(n + 1);
    for (uint32_t i = 0; out && i < capacity && i <= n; ++i) out[i] = i < n ? order[i] : NUE_RENDERER_NOOP;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_create(const nue_gfx_desc* desc, nue_gfx* out) {
    if (!desc || !out) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: desc and out are required");
    *out = nullptr;
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    nue_renderer renderer = desc->renderer;
    if (renderer == NUE_RENDERER_AUTO) {
        std::string cfg(32, '\0');
        size_t need = 0;
        if (NUE_SUCCEEDED(nue_config_get_string("gfx.renderer", "", &cfg[0], cfg.size(), &need)) && need > 1) {
            cfg.resize(need - 1);
            for (int r = NUE_RENDERER_NOOP; r <= NUE_RENDERER_GLES; ++r) {
                std::string name = nue_renderer_name(nue_renderer(r));
                std::string lower; for (char c : name) lower += char(c >= 'A' && c <= 'Z' ? c + 32 : c);
                std::string want; for (char c : cfg) want += char(c >= 'A' && c <= 'Z' ? c + 32 : c);
                if (lower == want) renderer = nue_renderer(r);
            }
        }
    }
    if (renderer == NUE_RENDERER_AUTO) { size_t n; renderer = platform_order(&n)[0]; }
    if (renderer != NUE_RENDERER_NOOP && !desc->window) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: a window is required unless the renderer is NOOP");
    if (renderer != NUE_RENDERER_NOOP && !(desc->flags & NUE_GFX_PREPARE_ONLY))
        return nuei::fail(NUE_ERR_UNSUPPORTED, "MANAGED mode needs the bgfx module (NUE_WITH_GFX); use NUE_GFX_PREPARE_ONLY and initialise bgfx from nue_gfx_get_native()");

    nue_gfx g = new (std::nothrow) nue_gfx_s();
    if (!g) return NUE_ERR_OUT_OF_MEMORY;
    g->window = desc->window;
    g->window_id = desc->window ? nue_window_get_id(desc->window) : 0;
    if (desc->window && !g->window_id) { delete g; return nuei::fail(NUE_ERR_INVALID_HANDLE, "nue_gfx_create: invalid window handle"); }
    g->renderer = renderer;
    g->flags = desc->flags;
    g->vsync = (desc->flags & NUE_GFX_VSYNC) != 0;
    g->follow_window = desc->width <= 0 || desc->height <= 0;
    if (!g->follow_window) { g->width = desc->width; g->height = desc->height; }
    else if (desc->window) nue_window_get_pixel_size(desc->window, &g->width, &g->height);
    else { g->width = 1280; g->height = 720; }
    {
        GfxRegistry& r = G();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.all.push_back(g);
        if (!r.hook_installed) { nuei::windows_add_destroy_hook(window_destroying); r.hook_installed = true; }
    }
    NUE_LOGI("gfx", "context created: renderer %s, %dx%d, %s", nue_renderer_name(renderer), g->width, g->height, (desc->flags & NUE_GFX_PREPARE_ONLY) ? "prepare-only" : "managed");
    *out = g;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_gfx_destroy(nue_gfx g) {
    if (!g) return;
    {
        GfxRegistry& r = G();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = std::find(r.all.begin(), r.all.end(), g);
        if (it == r.all.end()) return;
    }
    if (!g->orphaned) notify(g, NUE_GFX_NOTIFY_DESTROYING, 0, 0);
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); r.all.erase(std::find(r.all.begin(), r.all.end(), g)); }
    delete g;
}

#define GFX_OR_RETURN(g) \
    if (!known(g)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid gfx handle")

NUE_API nue_result NUE_CALL nue_gfx_get_native(nue_gfx g, nue_gfx_native* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    GFX_OR_RETURN(g);
    nue_gfx_native n;
    NUE_STRUCT_INIT(&n);
    n.renderer = g->renderer;
    NUE_STRUCT_INIT(&n.window);
    if (g->window) nue_window_get_native(g->window, &n.window);
    n.pixel_width = g->width;
    n.pixel_height = g->height;
    n.bgfx_initialized = g->bgfx_initialized ? NUE_TRUE : NUE_FALSE;
    n.single_threaded = (g->flags & NUE_GFX_SINGLE_THREADED) ? NUE_TRUE : NUE_FALSE;
    uint32_t keep = out->struct_size;
    std::memcpy(out, &n, keep < sizeof(n) ? keep : sizeof(n));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_get_caps(nue_gfx g, nue_gfx_caps*) {
    GFX_OR_RETURN(g);
    return nuei::fail(NUE_ERR_BAD_STATE, "bgfx is not initialised (capabilities come from the renderer)");
}

NUE_API nue_renderer NUE_CALL nue_gfx_get_renderer(nue_gfx g) { return known(g) ? g->renderer : NUE_RENDERER_NOOP; }

NUE_API nue_result NUE_CALL nue_gfx_notify_initialized(nue_gfx g, nue_bool initialized) {
    GFX_OR_RETURN(g);
    g->bgfx_initialized = initialized != 0;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_resize(nue_gfx g, int32_t w, int32_t h) {
    GFX_OR_RETURN(g);
    if (g->orphaned) return nuei::fail(NUE_ERR_BAD_STATE, "the window of this gfx context was destroyed");
    if ((w == 0) != (h == 0) || w < 0 || h < 0) return NUE_ERR_INVALID_ARGUMENT;
    if (w == 0) {
        g->follow_window = true;
        if (g->window) nue_window_get_pixel_size(g->window, &w, &h); else { w = g->width; h = g->height; }
    } else g->follow_window = false;
    if (w == g->width && h == g->height) return NUE_OK_NOOP;
    g->width = w; g->height = h;
    notify(g, NUE_GFX_NOTIFY_RESIZED, w, h);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_set_vsync(nue_gfx g, nue_bool vsync) {
    GFX_OR_RETURN(g);
    if ((g->vsync != 0) == (vsync != 0)) return NUE_OK_NOOP;
    g->vsync = vsync != 0;
    notify(g, NUE_GFX_NOTIFY_VSYNC_CHANGED, g->vsync ? 1 : 0, 0);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_suspend(nue_gfx g) {
    GFX_OR_RETURN(g);
    if (g->suspended) return NUE_OK_NOOP;
    g->suspended = true;
    notify(g, NUE_GFX_NOTIFY_SUSPENDED, 0, 0);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_resume(nue_gfx g) {
    GFX_OR_RETURN(g);
    if (!g->suspended) return NUE_OK_NOOP;
    g->suspended = false;
    if (g->follow_window && g->window) nue_window_get_pixel_size(g->window, &g->width, &g->height);
    notify(g, NUE_GFX_NOTIFY_RESUMED, g->width, g->height);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_handle_event(nue_gfx g, const nue_event* e) {
    GFX_OR_RETURN(g);
    if (!e) return NUE_ERR_INVALID_ARGUMENT;
    switch (e->type) {
        case NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (g->follow_window && e->window_id == g->window_id) return nue_gfx_resize(g, 0, 0);
            return NUE_OK_NOOP;
        case NUE_EVENT_SURFACE_LOST: if (e->window_id == g->window_id) return nue_gfx_suspend(g); return NUE_OK_NOOP;
        case NUE_EVENT_SURFACE_CREATED: if (e->window_id == g->window_id && g->suspended) return nue_gfx_resume(g); return NUE_OK_NOOP;
        case NUE_EVENT_APP_PAUSED: return nue_gfx_suspend(g);
        case NUE_EVENT_APP_RESUMED: return nue_gfx_resume(g);
        default: return NUE_OK_NOOP;
    }
}

NUE_API nue_result NUE_CALL nue_gfx_set_auto_events(nue_gfx g, nue_bool enabled) {
    GFX_OR_RETURN(g);
    g->auto_events = enabled != 0;
    GfxRegistry& r = G();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (enabled && !r.watcher_installed) { nue_event_add_watcher(gfx_watcher, nullptr); r.watcher_installed = true; }
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_present(nue_gfx g, uint32_t* out_frame) {
    GFX_OR_RETURN(g);
    if (g->flags & NUE_GFX_PREPARE_ONLY) return nuei::fail(NUE_ERR_BAD_STATE, "prepare-only context: the bgfx owner calls bgfx::frame");
    if (g->suspended) return NUE_OK_NOOP;
    ++g->frame;
    if (out_frame) *out_frame = g->frame;
    return NUE_OK;   // NOOP renderer: nothing to submit
}

NUE_API nue_result NUE_CALL nue_gfx_add_listener(nue_gfx g, nue_gfx_listener_fn fn, void* ud) {
    GFX_OR_RETURN(g);
    if (!fn) return NUE_ERR_INVALID_ARGUMENT;
    GfxRegistry& r = G();
    std::lock_guard<std::mutex> lock(r.mutex);
    g->listeners.push_back({fn, ud});
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_remove_listener(nue_gfx g, nue_gfx_listener_fn fn, void* ud) {
    GFX_OR_RETURN(g);
    GfxRegistry& r = G();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (size_t i = 0; i < g->listeners.size(); ++i)
        if (g->listeners[i].first == fn && g->listeners[i].second == ud) { g->listeners.erase(g->listeners.begin() + long(i)); return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

}  // extern "C"
