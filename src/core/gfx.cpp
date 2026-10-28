// Graphics context lifecycle (nue_gfx): renderer selection with config overrides, PREPARE and MANAGED modes, resize / suspend / device-lost state machine,
// listeners, native handles, GPU memory budget. The bgfx calls themselves live in src/gfx/gfx_bgfx.cpp (compiled with NUE_WITH_GFX) behind GfxBackend;
// this file is always compiled, so PREPARE mode and the NOOP renderer work in every build. Ownership: the gfx session (docs/GFX.md).
#include "internal/core_internal.h"
#include "../gfx/gfx_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

using nuei::gfxi::GfxBackend;

struct GfxRegistry {
    std::mutex mutex;
    std::vector<nue_gfx> all;
    bool hook_installed = false;
    bool watcher_installed = false;
    nue_gfx bgfx_owner = nullptr;           // bgfx is a process-wide singleton: one MANAGED context owns it
};
GfxRegistry& G() { static GfxRegistry* g = new GfxRegistry(); return *g; }

bool known(nue_gfx g) { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); return g && std::find(r.all.begin(), r.all.end(), g) != r.all.end(); }

std::string lower(std::string s) { for (char& c : s) c = char(std::tolower((unsigned char)c)); return s; }

void notify(nue_gfx g, nue_gfx_notification what, int32_t a, int32_t b) {
    std::vector<std::pair<nue_gfx_listener_fn, void*>> ls;
    { std::lock_guard<std::mutex> lock(g->m); ls = g->listeners; }
    for (auto& l : ls) l.first(l.second, g, what, a, b);
}

bool on_render_thread(nue_gfx g) { return g->render_thread != 0 && g->render_thread == nuei::current_thread_id(); }
bool live(nue_gfx g) { return g->impl != nullptr; }    // a bgfx instance is behind it (MANAGED and not bookkeeping-only)

void window_destroying(nue_window w) {
    std::vector<nue_gfx> affected;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); for (nue_gfx g : r.all) if (g->window == w) affected.push_back(g); }
    for (nue_gfx g : affected) {
        notify(g, NUE_GFX_NOTIFY_DESTROYING, 0, 0);
        std::lock_guard<std::mutex> lock(g->m);
        g->orphaned = true;
        g->window = nullptr;
    }
}

nue_bool NUE_CALL gfx_watcher(void*, const nue_event* e) {
    std::vector<nue_gfx> targets;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); for (nue_gfx g : r.all) if (g->auto_events) targets.push_back(g); }
    for (nue_gfx g : targets) nue_gfx_handle_event(g, e);
    return NUE_TRUE;
}

// ---- renderer names / selection ---------------------------------------------------------------------------------------------------------

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

nue_renderer parse_renderer(const std::string& text) {
    const std::string t = lower(text);
    struct Alias { const char* name; nue_renderer r; };
    static const Alias aliases[] = {
        {"auto", NUE_RENDERER_AUTO}, {"noop", NUE_RENDERER_NOOP}, {"none", NUE_RENDERER_NOOP}, {"null", NUE_RENDERER_NOOP},
        {"d3d11", NUE_RENDERER_D3D11}, {"dx11", NUE_RENDERER_D3D11}, {"direct3d 11", NUE_RENDERER_D3D11}, {"direct3d11", NUE_RENDERER_D3D11},
        {"d3d12", NUE_RENDERER_D3D12}, {"dx12", NUE_RENDERER_D3D12}, {"direct3d 12", NUE_RENDERER_D3D12}, {"direct3d12", NUE_RENDERER_D3D12},
        {"vulkan", NUE_RENDERER_VULKAN}, {"vk", NUE_RENDERER_VULKAN}, {"metal", NUE_RENDERER_METAL},
        {"opengl", NUE_RENDERER_OPENGL}, {"gl", NUE_RENDERER_OPENGL}, {"opengl es", NUE_RENDERER_GLES}, {"gles", NUE_RENDERER_GLES}, {"opengles", NUE_RENDERER_GLES}};
    for (const Alias& a : aliases) if (t == a.name) return a.r;
    return NUE_RENDERER_AUTO;
}

std::string config_string(const char* key) {
    std::string s(64, '\0');
    size_t need = 0;
    if (NUE_FAILED(nue_config_get_string(key, "", &s[0], s.size(), &need)) || need <= 1) return std::string();
    if (need > s.size()) { s.assign(need, '\0'); nue_config_get_string(key, "", &s[0], s.size(), &need); }
    s.resize(need - 1);
    return s;
}

// config "gfx.renderer = vulkan,d3d11": the ordered list to try (empty = nothing configured)
std::vector<nue_renderer> configured_renderers() {
    std::vector<nue_renderer> out;
    const std::string cfg = config_string("gfx.renderer");
    size_t pos = 0;
    while (pos <= cfg.size()) {
        size_t comma = cfg.find(',', pos);
        if (comma == std::string::npos) comma = cfg.size();
        std::string item = cfg.substr(pos, comma - pos);
        while (!item.empty() && std::isspace((unsigned char)item.front())) item.erase(item.begin());
        while (!item.empty() && std::isspace((unsigned char)item.back())) item.pop_back();
        if (!item.empty()) {
            const nue_renderer r = parse_renderer(item);
            if (r != NUE_RENDERER_AUTO) out.push_back(r);
            else if (lower(item) != "auto") NUE_LOGW("gfx", "config gfx.renderer: unknown renderer '%s' ignored", item.c_str());
        }
        pos = comma + 1;
    }
    return out;
}

// config overrides of the flags: gfx.vsync, gfx.debug, gfx.hdr10, gfx.srgb, gfx.single_threaded, gfx.max_anisotropy, gfx.capture
nue_gfx_flags apply_flag_overrides(nue_gfx_flags f) {
    struct K { const char* key; nue_gfx_flags bit; };
    static const K keys[] = {{"gfx.vsync", NUE_GFX_VSYNC}, {"gfx.debug", NUE_GFX_DEBUG}, {"gfx.hdr10", NUE_GFX_HDR10}, {"gfx.srgb", NUE_GFX_SRGB_BACKBUFFER},
                             {"gfx.single_threaded", NUE_GFX_SINGLE_THREADED}, {"gfx.max_anisotropy", NUE_GFX_MAXANISOTROPY}, {"gfx.capture", NUE_GFX_CAPTURE}};
    for (const K& k : keys) if (nue_config_has(k.key)) f = nue_config_get_bool(k.key, (f & k.bit) != 0) ? (f | k.bit) : (f & ~k.bit);
    return f;
}

// ---- native handles ---------------------------------------------------------------------------------------------------------------------

void platform_handles(const nue_native_window& nw, uint64_t* ndt, uint64_t* nwh) {
    *ndt = 0; *nwh = 0;
    switch (nw.kind) {
        case NUE_NATIVE_WIN32_HWND: *nwh = nw.window; break;
        case NUE_NATIVE_X11_WINDOW: *ndt = nw.display; *nwh = nw.window; break;
        case NUE_NATIVE_WAYLAND_SURFACE: *ndt = nw.display; *nwh = nw.window; break;                     // bgfx makes its own wl_egl_window from the surface (PlatformData.type = Wayland)
        case NUE_NATIVE_ANDROID_WINDOW: *nwh = nw.window; break;
        case NUE_NATIVE_UIKIT_WINDOW: case NUE_NATIVE_COCOA_WINDOW: *nwh = nw.extra ? nw.extra : nw.window; break;   // extra = CAMetalLayer*
        default: break;
    }
}

}  // namespace

namespace nuei {
namespace gfxi {

void on_device_lost(nue_gfx g) {
    if (!known(g)) return;
    { std::lock_guard<std::mutex> lock(g->m); if (g->device_lost) return; g->device_lost = true; }
    NUE_LOGE("gfx", "device lost");
    notify(g, NUE_GFX_NOTIFY_DEVICE_LOST, 0, 0);
}

std::string shader_cache_file(nue_gfx g, const char* name) {
    if (g->shader_cache_native.empty()) return std::string();
    return g->shader_cache_native + "/" + name;
}

bool window_native(nue_gfx g, nue_native_window* out) {
    NUE_STRUCT_INIT(out);
    if (g->has_adopted) { *out = g->adopted; return true; }
    // the null backend invents handles (0x10000 + id) so that tests can ask for them: nothing real is behind them, a renderer must not be created on one
    const nue_backend* be = nuei::backend();
    if (be && be->name && std::strcmp(be->name, "null") == 0) return false;
    if (g->window) return NUE_SUCCEEDED(nue_window_get_native(g->window, out)) && out->kind != NUE_NATIVE_NONE;
    return false;
}

void native_handles(const nue_native_window& nw, uint64_t* ndt, uint64_t* nwh) { platform_handles(nw, ndt, nwh); }

const GfxBackend* backend() {
#if defined(NUE_HAS_GFX)
    return bgfx_backend();
#else
    return nullptr;
#endif
}

}  // namespace gfxi

void gfx_shutdown_all() {
    std::vector<nue_gfx> left;
    { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); left = r.all; r.hook_installed = false; r.watcher_installed = false; }
    if (!left.empty()) NUE_LOGW("gfx", "%zu graphics context(s) were still open at shutdown and are being destroyed", left.size());
    for (nue_gfx g : left) nue_gfx_destroy(g);
}
}  // namespace nuei

namespace {

// ---- GPU memory budget -----------------------------------------------------------------------------------------------------------------
// The GPU shares the engine's "texture" budget (memory.h). If the texture module registered it first, its eviction hook stays; otherwise the first context
// registers it and eviction is dispatched to the evictors of every context.
uint64_t NUE_CALL budget_evict(void*, nue_budget_id, uint64_t needed, nue_memory_pressure level) {
    std::vector<std::pair<nue_gfx, std::pair<nue_gfx_evict_fn, void*>>> hooks;
    {
        GfxRegistry& r = G();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (nue_gfx g : r.all) { std::lock_guard<std::mutex> l2(g->m); for (auto& e : g->evictors) hooks.push_back({g, e}); }
    }
    uint64_t freed = 0;
    for (auto& h : hooks) {
        const uint64_t want = needed ? (needed > freed ? needed - freed : 0) : 0;
        if (needed && !want) break;
        freed += h.second.first(h.second.second, h.first, want, uint32_t(level));
    }
    return freed;
}

nue_budget_id texture_budget() {
    nue_budget_id id = 0;
    if (NUE_SUCCEEDED(nue_budget_find("texture", &id)) && id) return id;
    nue_budget_desc d;
    NUE_STRUCT_INIT(&d);
    d.name = "texture";
    d.kind = NUE_BUDGET_TEXTURE;
    d.evict = budget_evict;
    return NUE_SUCCEEDED(nue_budget_register(&d, &id)) ? id : 0;
}

// ---- state machine ---------------------------------------------------------------------------------------------------------------------

nue_gfx_state state_locked(const nue_gfx_s* g) {
    if (g->orphaned) return NUE_GFX_STATE_ORPHANED;
    if (g->device_lost) return g->suspended ? NUE_GFX_STATE_DEVICE_LOST : NUE_GFX_STATE_NEEDS_RESET;
    return g->suspended ? NUE_GFX_STATE_SUSPENDED : NUE_GFX_STATE_OK;
}

// [render] bring the live renderer in line with what other threads asked for
nue_result apply_pending(nue_gfx g) {
    const GfxBackend* be = nuei::gfxi::backend();
    bool size, vsync, surface_back, suspend_now = false;
    {
        std::lock_guard<std::mutex> lock(g->m);
        const bool changed_suspend = g->suspended != g->applied_suspended;
        size = g->dirty_size || g->width != g->applied_width || g->height != g->applied_height;
        vsync = g->dirty_vsync || g->vsync != g->applied_vsync;
        g->dirty_size = g->dirty_vsync = g->dirty_surface = false;
        if (!changed_suspend && !size && !vsync) return NUE_OK_NOOP;
        suspend_now = g->suspended;
        surface_back = changed_suspend && !g->suspended;
        g->applied_suspended = g->suspended;
        if (!suspend_now) { g->applied_width = g->width; g->applied_height = g->height; g->applied_vsync = g->vsync; }
    }
    if (!live(g) || !be || suspend_now) return NUE_OK;        // suspended: the new size / vsync are applied on resume (the surface may not exist)
    if (!size && !vsync && !surface_back) return NUE_OK;
    uint32_t before;
    { std::lock_guard<std::mutex> lock(g->m); before = g->generation; }
    const nue_result r = be->reset(g, surface_back);
    uint32_t after;
    { std::lock_guard<std::mutex> lock(g->m); after = g->generation; }
    if (after != before) notify(g, NUE_GFX_NOTIFY_DEVICE_RESET, int32_t(after), 0);   // the backend had to rebuild the device (new native window)
    return r;
}

nue_result recreate_backend(nue_gfx g, const GfxBackend* be) {
    be->shutdown(g);
    std::string err;
    {
        std::lock_guard<std::mutex> lock(g->m);
        g->applied_width = g->width; g->applied_height = g->height; g->applied_vsync = g->vsync; g->applied_suspended = false;
        g->dirty_size = g->dirty_vsync = g->dirty_surface = false;
    }
    const nue_result r = be->init(g, g->renderer, err);
    if (NUE_FAILED(r)) return nuei::fail(r, "nue_gfx_reset_device: %s", err.c_str());
    { std::lock_guard<std::mutex> lock(g->m); g->bgfx_initialized = true; }
    return NUE_OK;
}

}  // namespace

#define GFX_OR_RETURN(g) \
    if (!known(g)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid gfx handle")

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
    const GfxBackend* be = nuei::gfxi::backend();
    std::vector<nue_renderer> list;
    for (size_t i = 0; i < n; ++i) if (!be || be->renderer_compiled(order[i])) list.push_back(order[i]);
    list.push_back(NUE_RENDERER_NOOP);
    if (out_count) *out_count = uint32_t(list.size());
    for (uint32_t i = 0; out && i < capacity && i < list.size(); ++i) out[i] = list[i];
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_create(const nue_gfx_desc* desc, nue_gfx* out) {
    if (!out) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: desc and out are required");
    *out = nullptr;
    if (!desc || desc->struct_size < offsetof(nue_gfx_desc, reserved)) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: desc and out are required");
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;

    const bool prepare = (desc->flags & NUE_GFX_PREPARE_ONLY) != 0;
    const bool has_adopt = NUE_STRUCT_HAS(desc, nue_gfx_desc, native_window) && desc->native_window != nullptr;
    const uint32_t max_draw_calls = NUE_STRUCT_HAS(desc, nue_gfx_desc, max_draw_calls) ? desc->max_draw_calls : 0;
    const GfxBackend* be = nuei::gfxi::backend();

    // renderer candidates: an explicit request is strict; otherwise config list, otherwise the platform order
    std::vector<nue_renderer> candidates;
    bool strict = false;
    if (desc->renderer != NUE_RENDERER_AUTO) { candidates.push_back(desc->renderer); strict = true; }
    else {
        candidates = configured_renderers();
        if (candidates.empty()) {
            size_t n = 0;
            const nue_renderer* order = platform_order(&n);
            for (size_t i = 0; i < n; ++i) if (prepare || !be || be->renderer_compiled(order[i])) candidates.push_back(order[i]);
        }
    }
    if (candidates.empty()) return nuei::fail(NUE_ERR_UNSUPPORTED, "nue_gfx_create: no renderer is available in this build");
    const bool wants_surface = candidates.front() != NUE_RENDERER_NOOP;
    if (wants_surface && !desc->window && !has_adopt) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: a window is required unless the renderer is NOOP");
    if (desc->window && has_adopt) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: give either window or native_window");
    if (has_adopt && (desc->width <= 0 || desc->height <= 0)) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_create: native_window needs width and height");
    if (!prepare && !be && candidates.front() != NUE_RENDERER_NOOP)
        return nuei::fail(NUE_ERR_UNSUPPORTED, "MANAGED mode needs the bgfx module (build with -DNUE_WITH_GFX=ON); or use NUE_GFX_PREPARE_ONLY and initialise bgfx from nue_gfx_get_native()");

    nue_gfx g = new (std::nothrow) nue_gfx_s();
    if (!g) return NUE_ERR_OUT_OF_MEMORY;
    g->window = has_adopt ? nullptr : desc->window;
    g->window_id = g->window ? nue_window_get_id(g->window) : 0;
    if (g->window && !g->window_id) { delete g; return nuei::fail(NUE_ERR_INVALID_HANDLE, "nue_gfx_create: invalid window handle"); }
    if (has_adopt) { g->adopted = *desc->native_window; g->has_adopted = true; }
    g->desc = *desc;
    g->desc.struct_size = sizeof(nue_gfx_desc);
    if (desc->struct_size < sizeof(nue_gfx_desc)) { g->desc.native_window = nullptr; g->desc.max_draw_calls = 0; }
    g->desc.max_draw_calls = max_draw_calls;
    g->desc.shader_cache_dir = nullptr; g->desc.window = g->window; g->desc.native_window = nullptr;
    g->flags = apply_flag_overrides(desc->flags);
    g->desc.flags = g->flags;
    if (nue_config_has("gfx.msaa")) g->desc.msaa_samples = uint32_t(std::max<int64_t>(0, nue_config_get_int("gfx.msaa", 0)));
    g->managed = !prepare;
    g->vsync = (g->flags & NUE_GFX_VSYNC) != 0;
    g->follow_window = !has_adopt && (desc->width <= 0 || desc->height <= 0);
    if (!g->follow_window) { g->width = desc->width; g->height = desc->height; }
    else if (g->window) nue_window_get_pixel_size(g->window, &g->width, &g->height);
    else { g->width = 1280; g->height = 720; }
    g->applied_width = g->width; g->applied_height = g->height; g->applied_vsync = g->vsync;

    // shader cache directory (VFS path -> native, created on demand); config gfx.shader_cache overrides the descriptor
    std::string cache_vfs = config_string("gfx.shader_cache");
    if (cache_vfs.empty() && desc->shader_cache_dir) cache_vfs = desc->shader_cache_dir;
    if (!cache_vfs.empty()) {
        g->shader_cache_vfs = cache_vfs;
        std::string native = nuei::resolve_native_path(cache_vfs.c_str(), true);
        if (!native.empty() && NUE_SUCCEEDED(nue_vfs_create_directory(cache_vfs.c_str()))) {
            for (char& c : native) if (c == '\\') c = '/';
            while (native.size() > 1 && native.back() == '/') native.pop_back();
            g->shader_cache_native = native;
        } else NUE_LOGW("gfx", "shader cache directory '%s' is not usable; compiled shaders are not cached", cache_vfs.c_str());
    }

    {
        GfxRegistry& r = G();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.all.push_back(g);
        if (!r.hook_installed) { nuei::windows_add_destroy_hook(window_destroying); r.hook_installed = true; }
    }

    // choose and initialise the renderer
    nue_result result = NUE_OK;
    std::string errors;
    g->renderer = candidates.front();
    if (prepare) {
        // PREPARE: the first candidate; the client calls bgfx::init with nue_gfx_get_native()
    } else {
        g->render_thread = nuei::current_thread_id();
        bool done = false;
        for (nue_renderer c : candidates) {
            const bool noop = c == NUE_RENDERER_NOOP;
            if (!noop && be && !be->renderer_compiled(c)) { errors += std::string(" [") + nue_renderer_name(c) + ": not compiled into bgfx for this platform]"; continue; }
            if (!be) { g->renderer = c; done = true; break; }              // NOOP without the bgfx module: bookkeeping only
            bool owner_free;
            { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); owner_free = r.bgfx_owner == nullptr; if (owner_free) r.bgfx_owner = g; }
            if (!owner_free) {
                if (noop) { g->renderer = c; done = true; break; }       // another context owns bgfx: this NOOP context is bookkeeping only
                result = NUE_ERR_BUSY; errors += " [bgfx is already in use by another gfx context]"; break;
            }
            g->desc.max_draw_calls = max_draw_calls;
            std::string err;
            const nue_result ir = be->init(g, c, err);
            if (NUE_SUCCEEDED(ir)) { g->renderer = c; { std::lock_guard<std::mutex> lock(g->m); g->bgfx_initialized = true; } done = true; break; }
            { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); if (r.bgfx_owner == g) r.bgfx_owner = nullptr; }
            errors += std::string(" [") + nue_renderer_name(c) + ": " + err + "]";
            result = ir;
            if (strict) break;
        }
        if (!done) {
            if (NUE_SUCCEEDED(result)) result = NUE_ERR_BACKEND;
            { GfxRegistry& r = G(); std::lock_guard<std::mutex> lock(r.mutex); r.all.erase(std::find(r.all.begin(), r.all.end(), g)); }
            delete g;
            return nuei::fail(result, "nue_gfx_create: no renderer could be initialised:%s", errors.c_str());
        }
    }
    NUE_LOGI("gfx", "context created: renderer %s, %dx%d, %s%s", nue_renderer_name(g->renderer), g->width, g->height, prepare ? "prepare-only" : "managed", live(g) ? " (bgfx initialised)" : "");
    *out = g;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_gfx_destroy(nue_gfx g) {
    if (!known(g)) return;
    bool orphaned;
    { std::lock_guard<std::mutex> lock(g->m); orphaned = g->orphaned; }
    if (!orphaned) notify(g, NUE_GFX_NOTIFY_DESTROYING, 0, 0);
    if (live(g)) {
        if (!on_render_thread(g)) NUE_LOGW("gfx", "nue_gfx_destroy called off the render thread: bgfx::shutdown runs on the wrong thread");
        if (const GfxBackend* be = nuei::gfxi::backend()) be->shutdown(g);
    }
    {
        GfxRegistry& r = G();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.all.erase(std::find(r.all.begin(), r.all.end(), g));
        if (r.bgfx_owner == g) r.bgfx_owner = nullptr;
    }
    delete g;
}

NUE_API nue_result NUE_CALL nue_gfx_get_native(nue_gfx g, nue_gfx_native* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    GFX_OR_RETURN(g);
    nue_gfx_native n;
    NUE_STRUCT_INIT(&n);
    n.renderer = g->renderer;
    NUE_STRUCT_INIT(&n.window);
    if (g->has_adopted) n.window = g->adopted;
    else if (g->window) nue_window_get_native(g->window, &n.window);
    platform_handles(n.window, &n.platform_ndt, &n.platform_nwh);
    {
        std::lock_guard<std::mutex> lock(g->m);
        n.pixel_width = g->width;
        n.pixel_height = g->height;
        n.bgfx_initialized = g->bgfx_initialized ? NUE_TRUE : NUE_FALSE;
        n.generation = g->generation;
    }
    n.single_threaded = (g->flags & NUE_GFX_SINGLE_THREADED) ? NUE_TRUE : NUE_FALSE;
    uint32_t keep = out->struct_size;
    std::memcpy(out, &n, keep < sizeof(n) ? keep : sizeof(n));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_get_caps(nue_gfx g, nue_gfx_caps* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    GFX_OR_RETURN(g);
    const GfxBackend* be = nuei::gfxi::backend();
    if (!live(g) || !be) return nuei::fail(NUE_ERR_BAD_STATE, "bgfx is not initialised (capabilities come from the renderer)");
    nue_gfx_caps c;
    NUE_STRUCT_INIT(&c);
    const nue_result r = be->get_caps(g, &c);
    if (NUE_FAILED(r)) return r;
    uint32_t keep = out->struct_size;
    std::memcpy(out, &c, keep < sizeof(c) ? keep : sizeof(c));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_renderer NUE_CALL nue_gfx_get_renderer(nue_gfx g) { return known(g) ? g->renderer : NUE_RENDERER_NOOP; }

NUE_API nue_result NUE_CALL nue_gfx_notify_initialized(nue_gfx g, nue_bool initialized) {
    GFX_OR_RETURN(g);
    std::lock_guard<std::mutex> lock(g->m);
    g->bgfx_initialized = initialized != 0;
    if (initialized && g->managed == false) g->render_thread = nuei::current_thread_id();   // PREPARE: the render thread is where the client initialised bgfx
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_resize(nue_gfx g, int32_t w, int32_t h) {
    GFX_OR_RETURN(g);
    if ((w == 0) != (h == 0) || w < 0 || h < 0) return NUE_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(g->m);
        if (g->orphaned) return nuei::fail(NUE_ERR_BAD_STATE, "the window of this gfx context was destroyed");
        if (w == 0) {
            if (g->has_adopted || !g->window) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_gfx_resize: there is no nue_window to follow, give a size");
            g->follow_window = true;
            nue_window_get_pixel_size(g->window, &w, &h);
        } else g->follow_window = false;
        if (w == g->width && h == g->height) return NUE_OK_NOOP;
        g->width = w; g->height = h;
        g->dirty_size = true;
    }
    notify(g, NUE_GFX_NOTIFY_RESIZED, w, h);
    if (live(g) && on_render_thread(g)) apply_pending(g);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_set_vsync(nue_gfx g, nue_bool vsync) {
    GFX_OR_RETURN(g);
    {
        std::lock_guard<std::mutex> lock(g->m);
        if ((g->vsync != 0) == (vsync != 0)) return NUE_OK_NOOP;
        g->vsync = vsync != 0;
        g->dirty_vsync = true;
    }
    notify(g, NUE_GFX_NOTIFY_VSYNC_CHANGED, vsync ? 1 : 0, 0);
    if (live(g) && on_render_thread(g)) apply_pending(g);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_suspend(nue_gfx g) {
    GFX_OR_RETURN(g);
    {
        std::lock_guard<std::mutex> lock(g->m);
        if (g->suspended) return NUE_OK_NOOP;
        g->suspended = true;
        g->dirty_surface = true;
    }
    notify(g, NUE_GFX_NOTIFY_SUSPENDED, 0, 0);
    if (live(g) && on_render_thread(g)) apply_pending(g);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_resume(nue_gfx g) {
    GFX_OR_RETURN(g);
    int32_t w, h;
    {
        std::lock_guard<std::mutex> lock(g->m);
        if (!g->suspended) return NUE_OK_NOOP;
        g->suspended = false;
        g->dirty_surface = true;
        if (g->follow_window && g->window) {
            int32_t nw = g->width, nh = g->height;
            nue_window_get_pixel_size(g->window, &nw, &nh);
            if (nw != g->width || nh != g->height) { g->width = nw; g->height = nh; g->dirty_size = true; }
        }
        w = g->width; h = g->height;
    }
    notify(g, NUE_GFX_NOTIFY_RESUMED, w, h);
    if (live(g) && on_render_thread(g)) apply_pending(g);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_handle_event(nue_gfx g, const nue_event* e) {
    GFX_OR_RETURN(g);
    if (!e) return NUE_ERR_INVALID_ARGUMENT;
    switch (e->type) {
        case NUE_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            bool follow;
            { std::lock_guard<std::mutex> lock(g->m); follow = g->follow_window; }
            if (follow && e->window_id == g->window_id) return nue_gfx_resize(g, 0, 0);
            return NUE_OK_NOOP;
        }
        case NUE_EVENT_SURFACE_LOST: if (e->window_id == g->window_id) return nue_gfx_suspend(g); return NUE_OK_NOOP;
        case NUE_EVENT_SURFACE_CREATED: if (e->window_id == g->window_id) return nue_gfx_resume(g); return NUE_OK_NOOP;
        case NUE_EVENT_APP_PAUSED: return nue_gfx_suspend(g);
        case NUE_EVENT_APP_RESUMED: return nue_gfx_resume(g);
        default: return NUE_OK_NOOP;
    }
}

NUE_API nue_result NUE_CALL nue_gfx_set_auto_events(nue_gfx g, nue_bool enabled) {
    GFX_OR_RETURN(g);
    GfxRegistry& r = G();
    std::lock_guard<std::mutex> lock(r.mutex);
    g->auto_events = enabled != 0;
    if (enabled && !r.watcher_installed) { nue_event_add_watcher(gfx_watcher, nullptr); r.watcher_installed = true; }
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_present(nue_gfx g, uint32_t* out_frame) {
    GFX_OR_RETURN(g);
    if (!g->managed) return nuei::fail(NUE_ERR_BAD_STATE, "prepare-only context: the bgfx owner calls bgfx::frame");
    const GfxBackend* be = nuei::gfxi::backend();
    nue_gfx_state st;
    { std::lock_guard<std::mutex> lock(g->m); st = state_locked(g); }
    if (st == NUE_GFX_STATE_ORPHANED) return nuei::fail(NUE_ERR_BAD_STATE, "the window of this gfx context was destroyed");
    if (live(g) && !on_render_thread(g)) return nuei::fail(NUE_ERR_WRONG_THREAD, "nue_gfx_present must be called on the thread that created the context (the bgfx render thread)");
    if (live(g)) apply_pending(g);
    { std::lock_guard<std::mutex> lock(g->m); st = state_locked(g); }
    if (st == NUE_GFX_STATE_SUSPENDED || st == NUE_GFX_STATE_DEVICE_LOST) return NUE_OK_NOOP;
    if (st == NUE_GFX_STATE_NEEDS_RESET) return NUE_ERR_DEVICE_LOST;
    if (live(g) && be) be->frame(g);
    ++g->frame;
    if (out_frame) *out_frame = g->frame;
    return NUE_OK;
}

NUE_API nue_gfx_state NUE_CALL nue_gfx_get_state(nue_gfx g) {
    if (!known(g)) return NUE_GFX_STATE_ORPHANED;
    std::lock_guard<std::mutex> lock(g->m);
    return state_locked(g);
}

NUE_API nue_result NUE_CALL nue_gfx_report_device_lost(nue_gfx g) {
    GFX_OR_RETURN(g);
    nuei::gfxi::on_device_lost(g);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_apply_pending(nue_gfx g) {
    GFX_OR_RETURN(g);
    if (!live(g)) return NUE_OK_NOOP;
    if (!on_render_thread(g)) return nuei::fail(NUE_ERR_WRONG_THREAD, "nue_gfx_apply_pending must be called on the render thread");
    return apply_pending(g);
}

NUE_API nue_result NUE_CALL nue_gfx_reset_device(nue_gfx g, nue_bool force) {
    GFX_OR_RETURN(g);
    nue_gfx_state st;
    { std::lock_guard<std::mutex> lock(g->m); st = state_locked(g); }
    if (st == NUE_GFX_STATE_ORPHANED) return nuei::fail(NUE_ERR_BAD_STATE, "the window of this gfx context was destroyed");
    if (st == NUE_GFX_STATE_SUSPENDED || st == NUE_GFX_STATE_DEVICE_LOST) return nuei::fail(NUE_ERR_DEVICE_LOST, "the surface is suspended: resume first");
    if (st == NUE_GFX_STATE_OK && !force) return NUE_OK_NOOP;
    const GfxBackend* be = nuei::gfxi::backend();
    if (live(g) && be) {
        if (!on_render_thread(g)) return nuei::fail(NUE_ERR_WRONG_THREAD, "nue_gfx_reset_device must be called on the render thread");
        const nue_result r = recreate_backend(g, be);
        if (NUE_FAILED(r)) return r;
    }
    uint32_t gen;
    {
        std::lock_guard<std::mutex> lock(g->m);
        g->device_lost = false;
        gen = ++g->generation;
    }
    NUE_LOGI("gfx", "device reset: generation %u", gen);
    notify(g, NUE_GFX_NOTIFY_DEVICE_RESET, int32_t(gen), 0);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_get_shader_cache_dir(nue_gfx g, char* buffer, size_t capacity, size_t* out_size) {
    GFX_OR_RETURN(g);
    if (g->shader_cache_native.empty()) return NUE_ERR_NOT_FOUND;
    return nuei::copy_out_string(g->shader_cache_native, buffer, capacity, out_size);
}

NUE_API nue_result NUE_CALL nue_gfx_request_capture(nue_gfx g, nue_gfx_capture_fn fn, void* userdata) {
    GFX_OR_RETURN(g);
    if (!fn) return NUE_ERR_INVALID_ARGUMENT;
    const GfxBackend* be = nuei::gfxi::backend();
    if (!(g->flags & NUE_GFX_CAPTURE)) return nuei::fail(NUE_ERR_BAD_STATE, "create the context with NUE_GFX_CAPTURE to read frames back");
    if (!live(g) || !be) return nuei::fail(NUE_ERR_BAD_STATE, "bgfx is not initialised");
    return be->request_capture(g, fn, userdata);
}

NUE_API nue_result NUE_CALL nue_gfx_memory_request(nue_gfx g, uint64_t bytes) {
    GFX_OR_RETURN(g);
    const nue_budget_id id = texture_budget();
    if (!id) return nuei::fail(NUE_ERR_NOT_INITIALIZED, "the GPU memory budget is not available");
    return nue_budget_request(id, bytes);
}

NUE_API void NUE_CALL nue_gfx_memory_report(nue_gfx g, int64_t delta) {
    if (!known(g)) return;
    const nue_budget_id id = texture_budget();
    if (id) nue_budget_report(id, delta);
}

NUE_API nue_result NUE_CALL nue_gfx_add_evictor(nue_gfx g, nue_gfx_evict_fn fn, void* ud) {
    GFX_OR_RETURN(g);
    if (!fn) return NUE_ERR_INVALID_ARGUMENT;
    texture_budget();   // makes sure the budget (and with it the dispatcher) exists
    std::lock_guard<std::mutex> lock(g->m);
    g->evictors.push_back({fn, ud});
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_remove_evictor(nue_gfx g, nue_gfx_evict_fn fn, void* ud) {
    GFX_OR_RETURN(g);
    std::lock_guard<std::mutex> lock(g->m);
    for (size_t i = 0; i < g->evictors.size(); ++i)
        if (g->evictors[i].first == fn && g->evictors[i].second == ud) { g->evictors.erase(g->evictors.begin() + long(i)); return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

NUE_API nue_result NUE_CALL nue_gfx_add_listener(nue_gfx g, nue_gfx_listener_fn fn, void* ud) {
    GFX_OR_RETURN(g);
    if (!fn) return NUE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(g->m);
    g->listeners.push_back({fn, ud});
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gfx_remove_listener(nue_gfx g, nue_gfx_listener_fn fn, void* ud) {
    GFX_OR_RETURN(g);
    std::lock_guard<std::mutex> lock(g->m);
    for (size_t i = 0; i < g->listeners.size(); ++i)
        if (g->listeners[i].first == fn && g->listeners[i].second == ud) { g->listeners.erase(g->listeners.begin() + long(i)); return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

NUE_API nue_result NUE_CALL nue_gfx_get_gpu_caps(nue_gfx g, nue_gpu_caps* out) {
    if (!known(g)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid gfx handle");
    nue_result r = nue_gpu_probe(out);
    if (NUE_FAILED(r)) return r;
    const GfxBackend* be = nuei::gfxi::backend();
    if (g->renderer != NUE_RENDERER_NOOP) out->renderer = g->renderer;
    if (live(g) && be && g->renderer != NUE_RENDERER_NOOP) {
        nue_gpu_caps c = *out;
        if (NUE_SUCCEEDED(be->merge_gpu_caps(g, &c))) {
            const uint32_t keep = out->struct_size;
            std::memcpy(out, &c, keep < sizeof(c) ? keep : sizeof(c));
            out->struct_size = keep;
            return NUE_OK;
        }
    }
    return g->bgfx_initialized && g->renderer != NUE_RENDERER_NOOP ? r : NUE_OK_PARTIAL;   // PARTIAL: not backed by a live renderer
}

NUE_API nue_result NUE_CALL nue_gfx_get_tex_caps(nue_gfx g, nue_tex_caps* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    GFX_OR_RETURN(g);
    const GfxBackend* be = nuei::gfxi::backend();
    if (live(g) && be && g->renderer != NUE_RENDERER_NOOP) {
        const nue_result r = be->get_tex_caps(g, out);
        if (NUE_SUCCEEDED(r)) return r;
    }
    nue_gpu_caps gc;
    NUE_STRUCT_INIT(&gc);
    nue_result r = nue_gfx_get_gpu_caps(g, &gc);
    if (NUE_FAILED(r)) return r;
    const nue_result t = nue_tex_caps_from_gpu_caps(out, &gc);
    return NUE_FAILED(t) ? t : r;
}

}  // extern "C"
