# Seam between nue_gfx and the Direct3D 9 layer (nue_d3d9)

Context (read, not edited): branch `claude/d3d9-bgfx-d42a87`, folder `nue_d3d9/`. Today `nue::gpu::create_bgfx_gpu(BgfxOptions{window=HWND, renderer, vsync,
transient sizes, shader_cache_dir, ...})` calls `bgfx::init` itself on a window handle it is given; the `d3d9.dll` facade creates/owns the draw-to-window logic
(offscreen colour target of the game's size, final blit pass, letterbox via `fit_aspect`).

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

**First supported path: PREPARE** (no change in nue_d3d9 needed). The change requests for the nue_d3d9 session:

1. `BgfxOptions` gains `bool external_init` (skip `bgfx::init`/`shutdown`) and `const void* platform_data` (optional, instead of `window`).
2. `BgfxGpu::set_window_size` is called from a `nue_gfx` listener on `NUE_GFX_NOTIFY_RESIZED`.
3. A thin `nue_d3d9` entry that takes a `nue_gfx` handle: `create_bgfx_gpu_from_gfx(nue_gfx, options)`; it calls `nue_gfx_get_native`, `nue_gfx_notify_initialized` and registers a listener (suspend/resume/device lost -> D3D9 `TestCooperativeLevel` returns `D3DERR_DEVICELOST`/`DEVICENOTRESET` then `Reset`).
4. The shader cache directory comes from `nue_gfx_desc.shader_cache_dir` (VFS path -> `nue_vfs_resolve`).

## Window ownership for the recompiled game

The game creates its own HWND (the native widescreen fix hooks `CreateWindowExA`). The launcher attaches it: `nue_window_create` with `desc.attach`,
then `nue_gfx_create(window)`. NUE only observes the window (events, DPI, display, refresh rate), never destroys it, and the game's window procedure keeps priority.
The d3d9 `Reset` with new back buffer sizes maps to `nue_gfx_resize`.

## Renderer selection

Order per platform (overridable by `nue_gfx_desc.renderer` and config key `gfx.renderer`): Windows D3D11, Vulkan, D3D12, OpenGL; Linux Vulkan, OpenGL, GLES;
Android Vulkan, GLES; iOS Metal. The nue_d3d9 translator already targets D3D11 (HLSL via SPIRV-Cross) and Vulkan (SPIR-V); GLSL ES / MSL paths exist in `spirv_cross` but are untested there.
