# Seam between nue_gfx and the Direct3D 9 layer (nue_d3d9)

Context: the layer is `src/legacy/d3d9` (the engine's legacy_d3d9 module, branch `legacy-d3d9`). Today `nue::gpu::create_bgfx_gpu(BgfxOptions{window=HWND,
renderer, vsync, transient sizes, shader_cache_dir, ...})` calls `bgfx::init` itself on a window handle it is given; the `d3d9.dll` facade creates/owns the
draw-to-window logic (offscreen colour target of the game's size, final blit pass, letterbox via `fit_aspect`). What the layer does today in detail: section
"What the layer does today" below.

## Direction of dependency

`nue_d3d9` depends on `nue` (core + gfx); `nue` never depends on `nue_d3d9`. `nue_gfx` is therefore a *provider*: it supplies the window, the renderer
choice, the lifecycle events and shared policy (shader cache directory in the `cache` root, log routing, power/GPU preference), and the Direct3D 9 layer is a *client*.

## Two integration modes

| | MANAGED (default) | PREPARE (`NUE_GFX_PREPARE_ONLY`) |
|---|---|---|
| who calls `bgfx::init` | `nue_gfx_create` | the client (today's `create_bgfx_gpu`) |
| client needs | `BgfxOptions.use_existing_context = true` (to add) | `BgfxOptions.window = nue_gfx_native.window.window`, `renderer = nue_gfx_native.renderer`, `vsync`, sizes (works today) |
| `bgfx::frame` | `nue_gfx_present` or the client | the client |
| lifecycle | nue_gfx resets on resize/surface events | client reacts to listener notifications and calls `bgfx::reset` |

**Threads.** bgfx must be called from the thread that called `bgfx::init`. With the dll's `worker_thread=1` that is the layer's own worker thread, not the
game thread (the game thread only records calls into a queue; the worker runs the translator and every bgfx call, init and shutdown included). A MANAGED
mode must therefore either run `bgfx::init` on a thread the client names, or treat the client's worker as the thread it calls the render thread.

**First supported path: PREPARE** (no change in nue_d3d9 needed). The change requests for the nue_d3d9 session:

1. `BgfxOptions` gains `bool external_init` (skip `bgfx::init`/`shutdown`) and `const void* platform_data` (optional, instead of `window`).
2. `BgfxGpu::set_window_size` is called from a `nue_gfx` listener on `NUE_GFX_NOTIFY_RESIZED`. (Today it is called by polling: the dll reads the
   window's client size at every `Present` and calls it when the size changed.)
3. A thin `nue_d3d9` entry that takes a `nue_gfx` handle: `create_bgfx_gpu_from_gfx(nue_gfx, options)`; it calls `nue_gfx_get_native`, `nue_gfx_notify_initialized` and registers a listener (suspend/resume/device lost -> D3D9 `TestCooperativeLevel` returns `D3DERR_DEVICELOST`/`DEVICENOTRESET` then `Reset`).
4. The shader cache directory comes from `nue_gfx_desc.shader_cache_dir` (VFS path -> `nue_vfs_resolve`).

## Window ownership for the recompiled game

The game creates its own HWND (the native widescreen fix hooks `CreateWindowExA`). The launcher attaches it: `nue_window_create` with `desc.attach`,
then `nue_gfx_create(window)`. NUE only observes the window (events, DPI, display, refresh rate), never destroys it, and the game's window procedure keeps priority.

A Direct3D 9 `Reset` does **not** map to `nue_gfx_resize`: the game's back buffer is the layer's own offscreen colour target, which `Reset` re-creates at the
new back buffer size; the swap chain follows the window's client size, which changes independently (window resized, monitor, DPI) and is what
`nue_gfx_resize` / `NUE_GFX_NOTIFY_RESIZED` are about. The final pass scales the back buffer into the window.

## Textures and memory (plan, not current behaviour)

The translator asks `nue_gfx_get_gpu_caps` (or `nue_gpu_probe` before a device exists) which `nue_format_family` values the GPU samples. At `CreateTexture` /
`UnlockRect` it hands DXT/uncompressed data to `nue_texture` (nue-texture session) with those caps; the texture module transcodes when the GPU cannot sample the
format (BC on Apple/most mobile GPUs) and caches the result. Every GPU allocation is requested through the `texture` budget (`nue_budget_request`, `memory.h`);
the layer registers an eviction hook that drops MANAGED-pool copies and unused levels under pressure. No size is hard-coded: the limit is a policy resolved from
`nue_memory_info` (see `ARCHITECTURE.md`, section Memory).

None of this is implemented yet: what the layer does today is under "What the layer does today", points 3 and 4.

## Renderer selection

Order per platform (overridable by `nue_gfx_desc.renderer` and config key `gfx.renderer`): Windows D3D11, Vulkan, D3D12, OpenGL; Linux Vulkan, OpenGL, GLES;
Android Vulkan, GLES; iOS Metal. The nue_d3d9 translator already targets D3D11 (HLSL via SPIRV-Cross) and Vulkan (SPIR-V); GLSL ES / MSL paths exist in `spirv_cross` but are untested there.

## What the layer does today

Paths are under `src/legacy/d3d9/`.

1. **bgfx init / reset / frame / shutdown and threads.** `bgfx/bgfx_gpu.cpp`: `BgfxGpuImpl::init` calls `bgfx::init` on the given window handle;
   `set_window_size` calls `bgfx::reset` (twice: flags, then a swap chain description of the new size) with `reset_flags()` = vsync (from the game's
   present interval and the ini) | `BGFX_RESET_MAXANISOTROPY`; `present` draws the back buffer into the window and calls `bgfx::frame`; the destructor
   calls `bgfx::shutdown`. The dll creates the adapter in `dll/main.cpp` `CreateDevice`, on the game thread, or on its worker thread with
   `worker_thread=1` (`dll/threaded.cpp`). `render_thread=1` (default) = bgfx's own render thread; `render_thread=0` = `bgfx::renderFrame()` before init
   (single-threaded: the backend runs inside `bgfx::frame`).
2. **Window and lifecycle.** The window is the game's HWND (`D3DPRESENT_PARAMETERS::hDeviceWindow` or the focus window); the dll never creates one.
   `dll/device.cpp` `apply_window_size` polls `GetClientRect` / `ClientToScreen` / `MonitorFromWindow` at every `Present`; the visible part of the client
   area and `include/nue/window_fit.hpp` (`fit_aspect`) letterbox the back buffer. `Reset` re-creates the back buffer and restores every state to its
   default. `TestCooperativeLevel` always returns `D3D_OK` (`src/layer/layer.hpp`): no device lost, no suspend/resume; a minimised window only skips
   the resize. No display mode change for fullscreen (the frame is scaled into the window; window modes are the launcher's).
3. **Capabilities and formats.** At init the adapter reads bgfx's caps (`bgfx_gpu.cpp`): max texture size, origin, homogeneous depth, MRT, MSAA
   (from the BGRA8 format's MSAA bit), and per texture format whether it can be sampled; `src/layer/formats.cpp` uses that to choose the native format
   or a conversion (DXT decoded to RGBA8 when BC is missing, X8 forced opaque, and so on). Assumed: anisotropy 16; D24S8 with a fallback to D32F then
   D16 if it cannot be created; bgfx handle limits raised at compile time (16384 textures and buffers). The game's capability questions
   (`GetDeviceCaps`, `CheckDeviceFormat`, adapter modes) are answered by the system `d3d9.dll`. A real probe would add VRAM size and budget, vendor and
   device id, BC/ETC/ASTC support before a device exists, MSAA levels per format, and a Direct3D 9 caps table for systems without a Direct3D 9.
4. **Texture memory.** No GPU budget and no eviction. Only system-memory copies so that a later `Lock` shows what a level holds: `texture_copy_mb`
   (default 160 MB) in the layer, and `worker_copy_mb` (default 256 MB) on the game thread in worker mode. MANAGED pool = a GPU texture plus that
   optional copy; SYSTEMMEM / SCRATCH = CPU only. The log's `audit:` lines report texture and render-target memory and bgfx handles against their limits.
5. **Shader cache and pre-warm.** `bgfx/shader_container.cpp` keeps compiled shader containers on disk in `BgfxOptions::shader_cache_dir`: the ini's
   `shader_cache`, default `<dll folder>\nue_shader_cache`. D3D11 bytecode comes from `d3dcompiler_47.dll` (HLSL from SPIRV-Cross); Vulkan uses the
   SPIR-V. Pre-warm: `nue_programs.txt` in the same folder lists the programs of earlier runs; each is made when the game has created both of its
   shaders (while loading) instead of at its first draw. The driver's own shader creation still happens on bgfx's render thread at the next frame.
6. **Renderers.** `renderer=d3d11` (default) or `vulkan` in the ini (`NUE_RENDERER`). D3D11 is used in the game; Vulkan is verified with the captured
   traces (image comparison), not in the game. D3D12, OpenGL and Metal exist in `gpu::Renderer` but are untested.

### Windows-only today

* Window size by polling an HWND (`GetClientRect`, monitor queries) at every `Present`; `BgfxOptions::window` is an HWND (Android needs an
  `ANativeWindow` and surface lost/recreated events, iOS a `CAMetalLayer`, Linux X11/Wayland handles).
* `d3dcompiler_47.dll` for the D3D11 shader path.
* Capability queries forwarded to the system `d3d9.dll`.
* No device lost, no suspend/resume handling (`TestCooperativeLevel` always succeeds).
* The whole `dll/` folder: COM objects and exports of the x86 ABI, ini through `GetPrivateProfileStringA`, message-aware waits
  (`MsgWaitForMultipleObjects`), PDH GPU counters, F10/F11/F12 through `GetAsyncKeyState`, the x87 control word.

The layer (`src/layer`) and the shader translator (`src/shader`) are portable C++20.
