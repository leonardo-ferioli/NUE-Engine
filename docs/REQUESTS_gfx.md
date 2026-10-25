# Requests from the gfx session to other areas

The module is described in `GFX.md`; what the D3D9 layer does today is in `GFX_D3D9_SEAM.md`. Branch of the gfx session: `gfx`.

## To legacy-d3d9 (session `nue-d3d9`; folder `src/legacy/d3d9`, not touched by gfx)

Everything below is **additive and optional**: with none of it the layer keeps working in PREPARE mode exactly as today (`NUE_GFX_PREPARE_ONLY`). Order = value.

### D1. Take the window and the renderer from nue_gfx (PREPARE mode, no layer change needed)

`nue_gfx_get_native` gives what `BgfxOptions` asks for: `platform_nwh` -> `options.window`, `renderer` -> `options.renderer`, `pixel_width/height` -> `window_width/height`,
`NUE_GFX_VSYNC` -> `vsync`. For ndt / Wayland (Linux) `BgfxOptions` needs two more fields because bgfx now takes them in `Init::swapChain.ndt` and
`Init::platformData.type`:

```cpp
struct BgfxOptions {
    ...
    void* display = nullptr;          // NEW: native display (X11 Display*, wl_display*); nue_gfx_native.platform_ndt
    bool wayland = false;             // NEW: nue_gfx_native.window.kind == NUE_NATIVE_WAYLAND_SURFACE -> init.platformData.type = bgfx::NativeWindowHandleType::Wayland
    ...
};
// bgfx_gpu.cpp, BgfxGpuImpl::init:  init.swapChain.ndt = options.display;  init.platformData.type = options.wayland ? Wayland : Default;
```

### D2. `external_init`: let nue_gfx own `bgfx::init` / `shutdown` (MANAGED mode)

```cpp
struct BgfxOptions {
    ...
    bool external_init = false;       // NEW: bgfx is already initialised by nue_gfx_create (MANAGED mode): skip bgfx::init and bgfx::shutdown, keep everything else
};
```
`BgfxGpuImpl::init`: when `external_init`, skip `renderFrame()`, `bgfx::init` and the `init.limits` / callback setup (nue_gfx already applied `transient_*_mb`
from `nue_gfx_desc`, the callback routes bgfx's log to the engine log, `numDrawCalls` from `max_draw_calls`); start at `initialised_ = true; renderer_ = bgfx::getRendererType();`.
Destructor: skip `bgfx::shutdown()`. **Thread:** the layer's worker (`worker_thread=1`) must be the thread that calls `nue_gfx_create` (it is the render thread, `GFX.md` section 2),
so create the context inside the worker before `create_bgfx_gpu`. The layer must not call `bgfx::reset` itself in this mode for *window* size changes: nue_gfx does (next item);
its own offscreen target / MSAA handling stays untouched. Reset flags that the layer owns (vsync from the game's present interval) go through `nue_gfx_set_vsync`.

### D3. Resize from a listener instead of polling `GetClientRect`

Register once after creating the gpu:

```cpp
nue_gfx_add_listener(gfx, [](void* ud, nue_gfx, nue_gfx_notification what, int32_t a, int32_t b) {
    auto* self = static_cast<Dev*>(ud);
    if (what == NUE_GFX_NOTIFY_RESIZED) self->pending_window_size.store(pack(a, b));        // any thread: just record it
    if (what == NUE_GFX_NOTIFY_SUSPENDED || what == NUE_GFX_NOTIFY_DEVICE_LOST) self->lost.store(true);
    if (what == NUE_GFX_NOTIFY_DEVICE_RESET) self->generation.store(uint32_t(a));            // GPU objects of the old generation are gone
}, dev);
```
and apply `pending_window_size` with `BgfxGpu::set_window_size` at the next `Present` (render thread). With `external_init` `set_window_size` must only update the
layer's letterbox rectangle (`fit_aspect`) and not call `bgfx::reset` (nue_gfx already did, on the same thread, in `nue_gfx_resize` / `present`). The Win32 polling
(`apply_window_size`) stays as the source for the *visible area* (monitor work area), not for the swap chain size; a nue_window that follows the HWND (attached window) sends
`PIXEL_SIZE_CHANGED` by itself when the game resizes it.

### D4. `TestCooperativeLevel` / `Reset` (today always `D3D_OK`)

```cpp
HRESULT TestCooperativeLevel() {
    switch (nue_gfx_get_state(gfx)) {
        case NUE_GFX_STATE_OK:           return D3D_OK;
        case NUE_GFX_STATE_NEEDS_RESET:  return D3DERR_DEVICENOTRESET;
        default:                         return D3DERR_DEVICELOST;       // SUSPENDED, DEVICE_LOST, ORPHANED
    }
}
HRESULT Reset(params) {
    if (state == SUSPENDED || state == DEVICE_LOST) return D3DERR_DEVICELOST;
    if (state == NEEDS_RESET) { release default-pool objects; nue_gfx_reset_device(gfx, NUE_FALSE); /* GPU objects of the old generation are gone: rebuild textures, buffers, programs */ }
    ... re-create the back buffer at the new size (the layer's own offscreen target) as today ...
}
```
`Present` while SUSPENDED/DEVICE_LOST should return `D3DERR_DEVICELOST` without touching bgfx (nue_gfx_present returns `NUE_OK_NOOP` / `NUE_ERR_DEVICE_LOST`).
Note `Reset` is **not** `nue_gfx_resize` (see `GFX_D3D9_SEAM.md`).

### D5. Shader cache directory from the descriptor

`nue_gfx_get_shader_cache_dir(gfx, buf, cap, &n)` returns the native path (created) of `nue_gfx_desc.shader_cache_dir` (VFS `cache:...`). Use it as
`BgfxOptions::shader_cache_dir` (and for `nue_programs.txt`) when the ini does not name one. bgfx's own cache callbacks write `bgfx_<id>.bin` next to your `.nsc` files.

### D6. GPU memory through the budget (later, with nue-texture)

Before creating a texture / render target: `nue_gfx_memory_request(gfx, bytes)` (`NUE_ERR_OVERFLOW` -> free MANAGED-pool copies via your own eviction first, or fail the
`CreateTexture` with `D3DERR_OUTOFVIDEOMEMORY`); on destroy `nue_gfx_memory_report(gfx, -bytes)`; register `nue_gfx_add_evictor(gfx, fn, ud)` to drop MANAGED-pool copies and unused
mip levels under pressure (level = `nue_memory_pressure`). Caps for `nue_tex_make_plan`: `nue_gfx_get_tex_caps(gfx, &caps)` (live bgfx caps) or, before any device, `nue_gpu_probe`.

### D7. Two build facts

* With `-DNUE_WITH_GFX=ON` the root CMake builds `bgfx`, `bx`, `bimg` itself (limits 16384/16384/16384/1024/1024, the numbers your CMake sets); your `if(NOT TARGET bgfx)` already
  reuses them. **bimg is built without ASTC decoding there** (the texture module links the current astc-encoder; two copies clash). Your layer never used bimg's ASTC path.
* `bgfx::Init::swapChain` / `bgfx::reset(flags, &swapChain)` is the API of the pinned bgfx (no `PlatformData.nwh`, no `setPlatformData`): MSAA, sRGB and HDR10 are *swap chain* flags
  (`BGFX_SWAP_CHAIN_*`), vsync / max anisotropy are *reset* flags. nue_gfx follows that split.

## To the platform sessions (`src/platform/*`)

* **nue-linux / nue-core (SDL3 backend `src/platform/sdl3/backend_sdl3.cpp`)**: nothing required for X11/Wayland (surface + display are enough, bgfx creates the `wl_egl_window`).
* **nue-ios (and the SDL3 backend on Apple)**: `nue_native_window.extra` should carry the `CAMetalLayer*` for `NUE_NATIVE_UIKIT_WINDOW` / `COCOA_WINDOW` (the header already says so;
  the backend does not fill it). SDL3: create the window with `NUE_WINDOW_GFX_METAL`, `SDL_Metal_CreateView` then `SDL_Metal_GetLayer(view)`. Without it gfx hands bgfx the `UIWindow*`,
  which bgfx's Metal backend accepts as a view; the layer is the safer value. Also: on `applicationDidEnterBackground` / `WillResignActive` the lifecycle layer already pushes
  `APP_PAUSED`/`RESUMED`; gfx suspends and resumes on them.
* **nue-android**: `SURFACE_LOST` must be delivered **synchronously, before the Activity returns** from `surfaceDestroyed` (bgfx must not present to a dead `ANativeWindow`); the
  render thread may be inside `bgfx::frame` while the event thread suspends, so please call `nue_gfx_suspend`-equivalent work through the watcher (`nue_gfx_set_auto_events`) and
  block `surfaceDestroyed` until the render thread acknowledges, if you can. `SURFACE_CREATED` after it carries the new window; gfx rebuilds the device when the handle changed.
* **All four `gpu_probe` hooks exist**; gfx merges them with the live renderer. Wanted additions (all optional, `0` = unknown is fine): `dedicated_memory_bytes` (Windows/Linux),
  `max_msaa_samples` per format family where the API exposes it (D3D11 `CheckMultisampleQualityLevels`), `max_anisotropy`, `unified_memory`. bgfx exposes none of these.
* **nue-windows**: `NUE_WINDOW_GFX_*` flags are ignored on Windows (bgfx D3D11 needs only the HWND); nothing to do. If you wire the native HWND attach module into the
  backend later, `nue_window_create(desc.attach)` already is the ATTACH path gfx uses.

## To core (session `nue-core`)

Files gfx takes over from core: `src/core/gfx.cpp` (rewritten; it now includes `../gfx/gfx_internal.h`), `include/nue/gfx.h`. Files gfx touches in core's area, minimal:
`CMakeLists.txt` (link bgfx into `nue`, `tests/gfx`, install docs, `NUE_CONFIG_HAS_GFX`), `cmake/nueConfig.cmake.in` (`find_dependency(bgfx)`, `nue_HAS_GFX`), `samples/CMakeLists.txt`
(`hello_gfx` has its own CMakeLists), `base.h` + `CMakeLists.txt` project version (**0.1.2 -> 0.1.3**, `docs/API_CHANGES.md`), `tests/abi/reference/*.txt`. `src/core/gpu_caps.cpp` is **not**
touched. Please keep new code out of `gfx.cpp` and tell me if `internal/core_internal.h` changes (`nuei::fail`, `current_thread_id`, `resolve_native_path`,
`windows_add_destroy_hook`, `backend()` with `->name`, `NUE_HAS_GFX`).

## To texture (session `nue-texture`)

* `nue_gfx_get_tex_caps` calls `nue_tex_caps_from_bgfx` with `caps->formats` narrowed to `uint16_t` (the pinned bgfx widened it to `uint32_t[]`; the low 16 bits are all you read).
* bgfx's `bimg` is built without ASTC decoding because your `astc-encoder` 5.7.0 and bimg's bundled one define the same global symbols (lld `duplicate symbol` in every executable that links
  `nue` + bgfx). Nothing to do unless you want to rename/namespace your copy.
* Budget: gfx registers the engine budget `"texture"` only if nobody did (`nue_budget_find` first). If `nue_tex_budget_attach_engine` runs after a gfx evictor was added the budget is shared and
  the first registrant's hook is the engine hook. Tell me which you prefer for the final wiring (texture budget calls gfx evictors / gfx forwards to texture's).

## To the main session (CI)

* Add a job step `cmake --preset <p> -DNUE_WITH_GFX=ON` after the submodule `third_party/bgfx.cmake` is initialised **recursively** (`git submodule update --init --recursive --depth 1
  third_party/bgfx.cmake`). Android and iOS only build; nothing runs there. The new ctest entries are `nue_gfx_test_lifecycle` and (module on) `nue_gfx_smoke`.
* The shallow clone of `bgfx` is ~100 MB; the Windows runners need it only when the module is on.
