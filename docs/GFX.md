# nue_gfx - the graphics context (`include/nue/gfx.h`)

Owner: the gfx session. Files: `include/nue/gfx.h`, `src/core/gfx.cpp` (always compiled: registry, state machine, listeners, renderer choice, budget),
`src/gfx/` (bgfx half, compiled with `-DNUE_WITH_GFX=ON`), `cmake/NueGfx.cmake`, `samples/hello_gfx`, `tests/gfx`. Third-party code: `THIRD_PARTY_GFX.md`.
The Direct3D 9 layer is a client of this module: `GFX_D3D9_SEAM.md` and `REQUESTS_gfx.md`.

## 1. What it does

`nue_gfx` connects a window to bgfx. One context = one window + one bgfx instance (bgfx is a process-wide singleton; a second MANAGED context gets
`NUE_ERR_BUSY`, except NOOP ones, which then are bookkeeping only).

| Mode | Flag | Who calls `bgfx::init` | State |
|---|---|---|---|
| **MANAGED** (default) | - | `nue_gfx_create` | implemented; **needs `-DNUE_WITH_GFX=ON`**, otherwise `NUE_ERR_UNSUPPORTED` (NOOP renderer still works as bookkeeping) |
| **PREPARE** | `NUE_GFX_PREPARE_ONLY` | the client, with the values of `nue_gfx_get_native` (`platform_nwh`, `platform_ndt`, `renderer`, pixel size) | implemented, always available; the path the D3D9 layer uses today |
| NOOP renderer | `NUE_RENDERER_NOOP` | with the module: bgfx's own Noop renderer (all API calls are legal, nothing is drawn); without it: bookkeeping | for tests, servers, CI |

### Build

```
git submodule update --init --depth 1 third_party/SDL third_party/bgfx.cmake
git -C third_party/bgfx.cmake submodule update --init --recursive --depth 1
cmake --preset windows-x64 -DNUE_WITH_GFX=ON            # also linux-x64, android-arm64, ios-arm64, windows-x86
```

`NUE_WITH_GFX` is **OFF by default** (bgfx adds ~2 minutes to a clean build and a submodule download of ~150 MB). Handle limits are bgfx compile-time constants
(`NUE_GFX_MAX_TEXTURES` etc., default 16384 textures/buffers, 1024 frame buffers, because the D3D9 layer needs them; `nue_gfx_desc.max_*_handles` only warn when
asking for more). With the module on, `nue_build_info.has_gfx` is true, bgfx/bx/bimg are folded into `libnue` and a **static SDK installs them next to it**
(targets `nue::bgfx`, `nue::bx`, `nue::bimg` in the `nue` export set, `nue_HAS_GFX` in `nueConfig.cmake`; bgfx.cmake's own install is off because it would also install bimg_decode/encode); a shared SDK needs nothing else.

## 2. Threads

The **render thread** is the thread that called `bgfx::init`: in MANAGED mode the thread that calls `nue_gfx_create`; in PREPARE mode the thread where the client
initialised bgfx (`nue_gfx_notify_initialized(true)` records it). `[render]` calls (`create`, `destroy`, `present`, `apply_pending`, `reset_device`, `request_capture`)
must be made there (`NUE_ERR_WRONG_THREAD` otherwise). Everything else is `[any]`: `resize`, `suspend`, `resume`, `set_vsync`, `handle_event`, `report_device_lost` only
record the change under a mutex and notify listeners synchronously on the calling thread; the render thread applies the change (`bgfx::reset`) in its next
`present` / `apply_pending`, or immediately when the caller already is the render thread. A client with a worker thread that owns bgfx (the D3D9 dll with
`worker_thread=1`) creates the context on that worker.

## 3. Renderer choice

Order of precedence: `nue_gfx_desc.renderer` (strict: no fallback) > config key `gfx.renderer` (comma list, first that initialises wins; names: `d3d11 d3d12 vulkan
metal opengl gles noop auto`, aliases `dx11 vk gl`) > platform order, filtered by what bgfx has compiled in:

| Platform | Order |
|---|---|
| Windows | Direct3D 11, Vulkan, Direct3D 12, OpenGL |
| Linux | Vulkan, OpenGL, OpenGL ES |
| Android | Vulkan, OpenGL ES |
| iOS | Metal |

`bgfx::Init::fallback` is off: the engine owns the fallback loop, and a failed candidate is logged with the reason. Flag overrides from config:
`gfx.vsync gfx.debug gfx.hdr10 gfx.srgb gfx.single_threaded gfx.max_anisotropy gfx.capture` (bool), `gfx.msaa` (int), `gfx.shader_cache` (VFS path).
Flags: `VSYNC`, `DEBUG` (debug runtime / validation layers), `HDR10` and `SRGB_BACKBUFFER` (swap chain flags), `MAXANISOTROPY`, `SINGLE_THREADED` (calls
`bgfx::renderFrame()` before init: no bgfx render thread; the context thread must stay the same), `CAPTURE` (frame read-back, section 8). GPU preference on hybrid machines:
set `nue_power_request.gpu` before `nue_gfx_create`; with two or more adapters the context picks the first discrete (HIGH_PERFORMANCE) or integrated
(LOW_POWER) one by PCI vendor/device id (`bgfx::Init.vendorId/deviceId`).

## 4. Native handles

`nue_gfx_get_native` returns `platform_nwh` / `platform_ndt`, already in bgfx terms. Source: the `nue_window` (SDL3 window properties through the backend) or an
**adopted** native window (`nue_gfx_desc.native_window`, plus width/height): never destroyed, never moved.

| Native kind | `nwh` | `ndt` | Note |
|---|---|---|---|
| `WIN32_HWND` | HWND | 0 | verified (D3D11, hidden window) |
| `X11_WINDOW` | X11 `Window` id | `Display*` | written, not run |
| `WAYLAND_SURFACE` | `wl_surface*` | `wl_display*` | bgfx gets `platformData.type = Wayland` and makes its own `wl_egl_window`; written, not run |
| `ANDROID_WINDOW` | `ANativeWindow*` | 0 | written, not run |
| `UIKIT_WINDOW` / `COCOA_WINDOW` | `extra` (the `CAMetalLayer*`) when the backend fills it, else the window | 0 | the SDL3 backend does not fill `extra` yet: `REQUESTS_gfx.md` |

The null backend invents handles for tests; a real renderer is refused on them (`NUE_ERR_UNSUPPORTED`, "headless").

## 5. Lifecycle, device lost, reset (what `TestCooperativeLevel`/`Reset` need)

`nue_gfx_get_state`: `OK`, `SUSPENDED` (surface gone or app in background), `DEVICE_LOST` (lost while suspended), `NEEDS_RESET`, `ORPHANED` (window destroyed).

| D3D9 | gfx |
|---|---|
| `TestCooperativeLevel` | OK -> `D3D_OK`; SUSPENDED / DEVICE_LOST -> `D3DERR_DEVICELOST`; NEEDS_RESET -> `D3DERR_DEVICENOTRESET` |
| `Reset` after NOTRESET | recreate the layer's GPU objects, `nue_gfx_reset_device` (MANAGED: `bgfx::shutdown` + `init` again, `generation`++, `NUE_GFX_NOTIFY_DEVICE_RESET`; PREPARE: only clears the state and notifies) |
| `Reset` as resize of the game's back buffer | **not** a gfx call: the game's back buffer is the layer's offscreen target; `nue_gfx_resize` is the *window's* swap chain |

Sources of events: `nue_gfx_set_auto_events(true)` or `nue_gfx_handle_event` (`PIXEL_SIZE_CHANGED` -> resize while following the window; `SURFACE_LOST`, `APP_PAUSED` ->
suspend; `SURFACE_CREATED`, `APP_RESUMED` -> resume). MANAGED `present` while suspended returns `NUE_OK_NOOP`, while NEEDS_RESET `NUE_ERR_DEVICE_LOST`. Device loss in MANAGED mode comes from
bgfx's `fatal(DeviceLost)` callback (D3D11/D3D12 device removed) and is reported to listeners (`NUE_GFX_NOTIFY_DEVICE_LOST`); a PREPARE client that sees its own bgfx fatal calls
`nue_gfx_report_device_lost`. **Surface re-creation** (Android `SURFACE_CREATED` with a new `ANativeWindow`, iOS layer change): bgfx cannot re-point its main swap chain, so on
resume with a different native handle the device is rebuilt (generation++, `DEVICE_RESET`); same handle (desktop): plain `bgfx::reset`. While suspended nothing is submitted.

## 6. Capabilities

* `nue_gpu_probe` (no device): the platform layer's probe (DXGI, Vulkan, EGL, Metal: `nue_platform_ops.gpu_probe`; all four platform layers have one) or a conservative table.
* `nue_gfx_get_gpu_caps`: that probe **merged with the live renderer**: per `nue_format_family` the bgfx format caps of every format of the family (sample, filter, mipmaps,
  render target, blend, sRGB, storage, MSAA, `NUE_FMTSUP_EMULATED` = bgfx's `2D_EMULATED`), max sizes, layers, render targets, compute; anisotropy, 3D size, MSAA count and memory stay
  the probe's (bgfx does not expose them). `D32F_STENCIL8` is reported unsupported (bgfx has no such format). NOOP contexts return the probe only (bgfx's Noop caps are not real).
* `nue_gfx_get_caps`: renderer name, adapter name (matched by PCI id in `nue_system_info.gpus`), vendor/device, handle limits.
* `nue_gfx_get_tex_caps`: `nue_tex_caps` for `nue_texture` from the exact bgfx format flags (`nue_tex_caps_from_bgfx`); before bgfx exists from the probe.
* bimg is built **without its ASTC decoder** (see `THIRD_PARTY_GFX.md`): a driver without ASTC (every desktop GPU) reports ASTC as unsupported, not emulated; the texture module transcodes.

## 7. Memory, cache, log

* GPU allocations are accounted in the engine's **`texture` budget** (memory.h): `nue_gfx_memory_request(bytes)` before creating a big resource (`NUE_ERR_OVERFLOW` = does
  not fit even after eviction), `nue_gfx_memory_report(-bytes)` when it is destroyed, `nue_gfx_add_evictor` for the hook (called on `request` that does not fit and on
  `NUE_EVENT_MEMORY_PRESSURE` through the budget, with the pressure level). The limit is `nue_memory_info` policy (fraction of GPU/process memory), overridable with `budget.texture.*`.
  If the texture module registered the budget first (`nue_tex_budget_attach_engine`) its own eviction hook stays and gfx evictors are not called by it; register the client's
  hook with the texture budget in that case (open point, see 10).
* `shader_cache_dir` (VFS path, `gfx.shader_cache` overrides) is resolved and created at `nue_gfx_create`; bgfx's callback `cacheRead/Write` (pipeline caches, driver binaries)
  stores `bgfx_<id>.bin` files there (atomic rename). `nue_gfx_get_shader_cache_dir` gives the native path to clients with their own caches (the D3D9 layer's `.nsc` files).
* bgfx's trace and fatal messages go to the engine log under the tag `bgfx` (errors as warnings, the rest as debug). Fatal errors other than device lost are logged and do **not**
  abort the process (bgfx's own default does).

## 8. Frame capture

Flag `NUE_GFX_CAPTURE`, then `nue_gfx_request_capture(fn, ud)` (one shot): `fn` receives the next presented frame as BGRA8, top row first. Used by the local test.

## 9. Sample and tests

| What | Where | Runs |
|---|---|---|
| `nue_sample_hello_gfx` | `samples/hello_gfx` | clear + coloured triangle + textured quad with bgfx's pre-compiled shaders (every renderer, no shader compiler); `--noop --frames N` runs headless |
| `nue_gfx_smoke` (ctest) | the sample with `--noop --frames 30` on the null backend through bgfx's Noop renderer | CI (when `NUE_WITH_GFX=ON`) |
| `nue_gfx_test_lifecycle` (ctest) | `tests/gfx/test_lifecycle.cpp`: names, config overrides, adopted windows and handle mapping, state machine, listeners, cache dir, budget with eviction, NOOP presents | CI, with and without the module |
| `nue_gfx_local_renderers_hidden` (ctest label `gpu`) | `tests/gfx/local`: real renderers (D3D11 required; Vulkan, D3D12, OpenGL if present) on **hidden** windows (never shown, never fullscreen): caps, clear + draw + pixel read-back, resize, suspend/resume, device reset, budget | only with `-DNUE_GFX_LOCAL_TESTS=ON`, by hand on a machine with a GPU |

## 10. Status (honest)

| Piece | State |
|---|---|
| MANAGED mode on Windows x64: **Direct3D 11, Vulkan, Direct3D 12 and OpenGL 4.3** | implemented and **run** on the dev machine (AMD Radeon Graphics, hidden windows): caps, clear + draw, pixel read-back, resize (through a window resize event), suspend/resume, device reset, budget (`nue_gfx_local_renderers_hidden`, 177 checks); `hello_gfx --hidden` runs 20+ frames on each |
| Windows x86 (clang i686, static CRT) | built; all 34 ctest entries pass, including the hidden-window test on Direct3D 11, Vulkan, Direct3D 12 and OpenGL (177 checks); needs `-msse4.2` and two bgfx workarounds, in `cmake/NueGfx.cmake` and `src/gfx/x86` |
| Notes from the multi-renderer run | Vulkan swap chains are bound to the window surface: the back buffer can only follow the window (an explicit `nue_gfx_resize` to another size is for D3D/GL). A capture requested in the first frames after a reset/resize of a **Direct3D 12** device returned black; after a few settle frames it is correct (bgfx D3D12 is several frames deep) |
| Android arm64 (Vulkan/GLES, surface lifecycle) | **compiled** here (`android-arm64` preset, NDK 28.2: `libnue.a` with bgfx, no warnings in gfx files); **never run**. CI builds Android and iOS but **runs nothing there** |
| Linux (X11/Wayland, Vulkan/GL/GLES), iOS (Metal) | **written, not compiled or run here** (no Linux/GCC or Xcode on this machine); GCC is stricter: the gfx files were reviewed by hand for it, not compiled with it |
| Surface re-creation / device rebuild on resume | written, not run on any device |
| Device lost on real hardware | the path is covered by `report_device_lost` + `reset_device` in tests; a real TDR was not provoked |
| eviction hook ordering with the texture module's own budget hook | open point (first registrant owns the engine hook) |
| Fatal errors other than device lost keep running | by design, documented |
