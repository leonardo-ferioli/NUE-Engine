/*
 * NUE engine - graphics context (nue_gfx).
 *
 * nue_gfx connects a nue_window to bgfx: it picks the renderer for the platform (Direct3D 11/12 on Windows, Vulkan on
 * Windows/Linux/Android, Metal on iOS/macOS, OpenGL ES as the fallback on mobile and Linux), initialises bgfx on the window's native
 * handle, resets it on resize / surface loss, and exposes the native handles so other modules can share the context.
 *
 * The Direct3D 9 layer (library nue_d3d9, branch claude/d3d9-bgfx-d42a87) sits ABOVE this module, not inside it: the recompiled game
 * calls a d3d9.dll-compatible API; the translator turns it into bgfx calls. The seam is deliberately small:
 *
 *   1. MANAGED mode (default): nue_gfx_create() calls bgfx::init itself. The translator then runs on the already-initialised bgfx
 *      (it needs a BgfxOptions "use existing context" flag; see docs/GFX_D3D9_SEAM.md for the exact change request).
 *   2. PREPARE mode (NUE_GFX_PREPARE_ONLY): nue_gfx_create() only chooses the renderer, validates the window/surface and keeps the
 *      lifecycle bookkeeping; the translator (or any other bgfx user) calls bgfx::init with the values from nue_gfx_get_native().
 *      This is how the current nue_d3d9 adapter works, so it is the first-supported path on Windows.
 *
 * In both modes nue_gfx keeps listeners informed (resize, surface lost/created, suspend/resume, vsync change) so the Direct3D 9
 * layer can implement IDirect3DDevice9::Reset/TestCooperativeLevel semantics from them.
 *
 * Thread: nue_gfx_create/destroy/resize on the event thread; nue_gfx_present and everything bgfx related on the render thread
 * (the thread that called bgfx::init - for the d3d9 layer, the game's thread). Queries are [any].
 */
#ifndef NUE_GFX_H
#define NUE_GFX_H

#include "nue/base.h"
#include "nue/window.h"

#ifdef __cplusplus
extern "C" {
#endif

NUE_DEFINE_HANDLE(nue_gfx);

typedef enum nue_renderer {
    NUE_RENDERER_AUTO = 0,
    NUE_RENDERER_NOOP,                      /* no rendering: tests, servers */
    NUE_RENDERER_D3D11,
    NUE_RENDERER_D3D12,
    NUE_RENDERER_VULKAN,
    NUE_RENDERER_METAL,
    NUE_RENDERER_OPENGL,
    NUE_RENDERER_GLES
} nue_renderer;

typedef uint32_t nue_gfx_flags;
enum {
    NUE_GFX_VSYNC            = 1u << 0,
    NUE_GFX_DEBUG            = 1u << 1,     /* validation layers / debug runtime, shader debug info */
    NUE_GFX_HDR10            = 1u << 2,
    NUE_GFX_SRGB_BACKBUFFER  = 1u << 3,
    NUE_GFX_PREPARE_ONLY     = 1u << 4,     /* see header comment: do not call bgfx::init */
    NUE_GFX_SINGLE_THREADED  = 1u << 5,     /* no bgfx render thread: renderFrame() before init (deterministic, easier debugging) */
    NUE_GFX_MAXANISOTROPY    = 1u << 6,
    NUE_GFX_CAPTURE          = 1u << 7      /* allow frame readback/screenshots */
};

typedef struct nue_gfx_desc {
    uint32_t struct_size;
    nue_window window;                      /* NULL only with NUE_RENDERER_NOOP (offscreen) */
    nue_renderer renderer;                  /* AUTO: platform order (see nue_gfx_list_renderers) */
    nue_gfx_flags flags;
    int32_t width, height;                  /* backbuffer pixels; 0 = the window's pixel size (and follow it) */
    uint32_t msaa_samples;                  /* 0/1 = off; 2,4,8,16 */
    uint32_t max_texture_handles;           /* 0 = bgfx default; the Direct3D 9 layer needs 16384 (see nue_d3d9 CMake) */
    uint32_t max_buffer_handles;            /* vertex + index buffers each */
    uint32_t max_framebuffer_handles;
    uint32_t transient_vertex_mb, transient_index_mb;   /* 0 = bgfx default (~6 MB / 2 MB); the d3d9 layer uses 64 / 16 */
    const char* shader_cache_dir;           /* VFS path, e.g. "cache:shaders" (created on demand); NULL = none */
    uint32_t reserved[8];
} nue_gfx_desc;

typedef struct nue_gfx_native {
    uint32_t struct_size;
    nue_renderer renderer;                  /* the one actually selected (never AUTO) */
    nue_native_window window;               /* native surface handles to pass as bgfx::PlatformData.nwh / ndt */
    uint64_t context;                       /* bgfx PlatformData.context (GL context / device) or 0 */
    uint64_t backbuffer, backbuffer_ds;     /* shared render targets if the renderer provides them */
    int32_t pixel_width, pixel_height;
    nue_bool bgfx_initialized;              /* true in MANAGED mode after create, or in PREPARE mode after nue_gfx_notify_initialized */
    nue_bool single_threaded;
} nue_gfx_native;

typedef struct nue_gfx_caps {
    uint32_t struct_size;
    char renderer_name[32];                 /* "Direct3D 11", "Vulkan"... */
    char adapter_name[128];
    uint32_t max_texture_size;
    uint32_t max_framebuffer_attachments;
    uint32_t max_anisotropy;
    nue_bool compute, instancing, texture_compare, hdr10, occlusion_query;
    uint32_t reserved[8];
} nue_gfx_caps;

/* Renderers that are compiled in and plausible on this machine, best first; *out_count = total. [any] */
NUE_API nue_result NUE_CALL nue_gfx_list_renderers(nue_renderer* out, uint32_t capacity, uint32_t* out_count);
NUE_API const char* NUE_CALL nue_renderer_name(nue_renderer renderer);

NUE_API nue_result NUE_CALL nue_gfx_create(const nue_gfx_desc* desc, nue_gfx* out_gfx);
NUE_API void NUE_CALL nue_gfx_destroy(nue_gfx gfx);
NUE_API nue_result NUE_CALL nue_gfx_get_native(nue_gfx gfx, nue_gfx_native* out);
NUE_API nue_result NUE_CALL nue_gfx_get_caps(nue_gfx gfx, nue_gfx_caps* out);                    /* needs bgfx_initialized */
NUE_API nue_renderer NUE_CALL nue_gfx_get_renderer(nue_gfx gfx);
/* PREPARE mode: the bgfx user tells nue_gfx that bgfx::init succeeded (or shut down: initialized = false). */
NUE_API nue_result NUE_CALL nue_gfx_notify_initialized(nue_gfx gfx, nue_bool initialized);

/* Feed window events here from the event loop (or enable nue_gfx_set_auto_events) so the backbuffer follows the window:
 * PIXEL_SIZE_CHANGED -> resize; SURFACE_LOST -> suspend (swapchain released); SURFACE_CREATED/APP_RESUMED -> resume. */
NUE_API nue_result NUE_CALL nue_gfx_handle_event(nue_gfx gfx, const nue_event* event);
NUE_API nue_result NUE_CALL nue_gfx_set_auto_events(nue_gfx gfx, nue_bool enabled);               /* nue_gfx installs an event watcher; resets then happen on the producing thread's next pump */
NUE_API nue_result NUE_CALL nue_gfx_resize(nue_gfx gfx, int32_t pixel_width, int32_t pixel_height);   /* 0,0 = window pixel size */
NUE_API nue_result NUE_CALL nue_gfx_set_vsync(nue_gfx gfx, nue_bool vsync);
NUE_API nue_result NUE_CALL nue_gfx_suspend(nue_gfx gfx);                                           /* release the swapchain/surface (background) */
NUE_API nue_result NUE_CALL nue_gfx_resume(nue_gfx gfx);
/* MANAGED mode: submit the frame (bgfx::frame). Returns the frame number. PREPARE mode: NUE_ERR_BAD_STATE (the owner calls bgfx::frame). */
NUE_API nue_result NUE_CALL nue_gfx_present(nue_gfx gfx, uint32_t* out_frame_number);

/* Listeners: notified synchronously on the thread that triggers the change (resize/suspend: the event thread). */
typedef enum nue_gfx_notification {
    NUE_GFX_NOTIFY_RESIZED = 1,             /* a, b = new pixel width, height */
    NUE_GFX_NOTIFY_VSYNC_CHANGED,           /* a = 0/1 */
    NUE_GFX_NOTIFY_SUSPENDED,
    NUE_GFX_NOTIFY_RESUMED,
    NUE_GFX_NOTIFY_DEVICE_LOST,             /* renderer reported device removal; the owner must recreate everything */
    NUE_GFX_NOTIFY_DESTROYING
} nue_gfx_notification;
typedef void (NUE_CALL *nue_gfx_listener_fn)(void* userdata, nue_gfx gfx, nue_gfx_notification what, int32_t a, int32_t b);
NUE_API nue_result NUE_CALL nue_gfx_add_listener(nue_gfx gfx, nue_gfx_listener_fn fn, void* userdata);
NUE_API nue_result NUE_CALL nue_gfx_remove_listener(nue_gfx gfx, nue_gfx_listener_fn fn, void* userdata);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_GFX_H */
