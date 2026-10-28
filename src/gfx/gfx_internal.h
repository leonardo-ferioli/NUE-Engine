// Internal declarations of the graphics module (not installed). The platform independent part (src/core/gfx.cpp: registry, state machine, listeners,
// budget, renderer selection) is always compiled; the bgfx part (src/gfx/gfx_bgfx.cpp) only with NUE_WITH_GFX and is reached through GfxBackend.
#pragma once

#include "internal/core_internal.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct nue_gfx_s {
    // ---- identity and configuration (set in create, not changed afterwards unless noted)
    nue_window window = nullptr;
    nue_window_id window_id = 0;
    nue_native_window adopted{};            // desc.native_window: a native window the caller owns (struct_size == 0 when unused)
    bool has_adopted = false;
    nue_renderer renderer = NUE_RENDERER_NOOP;
    nue_gfx_desc desc{};                    // copy; pointers inside are not valid after create (strings are copied below)
    nue_gfx_flags flags = 0;
    bool managed = false;                   // MANAGED mode: nue_gfx owns bgfx
    std::string shader_cache_vfs, shader_cache_native;

    // ---- state shared between threads, guarded by `m`
    std::mutex m;                           // protects the fields below up to "render thread only"
    bool follow_window = true;
    int32_t width = 0, height = 0;          // wanted back buffer size
    bool vsync = false;
    bool suspended = false;                 // wanted
    bool orphaned = false;                  // the window was destroyed under us
    bool device_lost = false;
    bool dirty_size = false, dirty_vsync = false, dirty_surface = false;   // changes recorded for the render thread
    bool auto_events = false;
    bool bgfx_initialized = false;
    uint32_t generation = 0;
    uint64_t render_thread = 0;             // nuei::current_thread_id() of the thread that created a MANAGED context
    std::vector<std::pair<nue_gfx_listener_fn, void*>> listeners;
    std::vector<std::pair<nue_gfx_evict_fn, void*>> evictors;

    // ---- render thread only
    int32_t applied_width = 0, applied_height = 0;
    bool applied_vsync = false, applied_suspended = false;
    uint32_t frame = 0;
    void* impl = nullptr;                   // backend private state (bgfx)
};

namespace nuei {
namespace gfxi {

// One implementation exists: bgfx. NUE_WITH_GFX=OFF builds return nullptr from backend() and MANAGED contexts fail with NUE_ERR_UNSUPPORTED.
struct GfxBackend {
    // Which of the renderers are compiled into bgfx for this platform (NOOP is always in).
    bool (*renderer_compiled)(nue_renderer r);
    // [render] bgfx::init for `g` with renderer `r` using g->width/height/flags/vsync and the window's native handles. On failure nothing is left initialised.
    nue_result (*init)(nue_gfx g, nue_renderer r, std::string& error);
    void (*shutdown)(nue_gfx g);
    // [render] apply g->width/height/vsync to the live renderer (bgfx::reset). `surface_changed`: the native window handle may have changed (surface re-created).
    nue_result (*reset)(nue_gfx g, bool surface_changed);
    // [render] submit the frame (bgfx::frame); returns the frame number.
    uint32_t (*frame)(nue_gfx g);
    nue_result (*get_caps)(nue_gfx g, nue_gfx_caps* out);
    nue_result (*merge_gpu_caps)(nue_gfx g, nue_gpu_caps* inout);
    nue_result (*get_tex_caps)(nue_gfx g, nue_tex_caps* out);
    nue_result (*request_capture)(nue_gfx g, nue_gfx_capture_fn fn, void* userdata);
};

const GfxBackend* backend();                         // nullptr when NUE_WITH_GFX is off
const GfxBackend* bgfx_backend();                    // defined by src/gfx/gfx_bgfx.cpp (only when NUE_HAS_GFX)
// bgfx PlatformData.ndt / .nwh for a native window (pure data mapping; also used by PREPARE clients through nue_gfx_get_native)
void native_handles(const nue_native_window& nw, uint64_t* ndt, uint64_t* nwh);

// Called by the backend (any thread) when the renderer reports that the device was removed.
void on_device_lost(nue_gfx g);
// Helpers shared with the backend.
std::string shader_cache_file(nue_gfx g, const char* name);   // native path inside the shader cache directory ("" when none)
bool window_native(nue_gfx g, nue_native_window* out);        // adopted handle or the nue_window's; false when there is none

}  // namespace gfxi
}  // namespace nuei
