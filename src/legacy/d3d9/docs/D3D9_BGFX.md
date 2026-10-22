# nue_d3d9: a Direct3D 9 to bgfx translator

`nue_d3d9/` is a standalone C++20 library: it answers Direct3D 9 calls (as plain handle-based C++ calls, no COM, no Windows types, no guest pointers) and
draws through [bgfx](https://github.com/bkaradzic/bgfx) (Direct3D 11/12, Vulkan, OpenGL (ES), Metal). It knows nothing about any game: the 32-bit
guest/COM glue that turns a game's `IDirect3DDevice9` calls into library calls stays outside it (the *facade*, `platform/` and `native32.cpp` today).
It was written for Need for Speed: Underground 2 and is meant for other Direct3D 9 games of the same engine family (Most Wanted next).

Status: **a real frame is rendered** (see "Measurements"). Not everything of Direct3D 9 is implemented: the coverage table says what is and is not.

## Using it in the game: the drop-in `d3d9.dll`

`nue_d3d9/dll/` implements the COM objects of Direct3D 9 (`IDirect3D9`, `IDirect3DDevice9`, textures, surfaces, buffers, declarations, shaders, state
blocks, swap chain) on top of the layer and the bgfx adapter, and exports `Direct3DCreate9` and the `D3DPERF_*` stubs, so it works the way DXVK does: **put
`d3d9.dll` in the folder of the program that uses Direct3D 9**. Capability questions (adapter name, display modes, `CheckDeviceFormat`, multisample support,
`GetDeviceCaps`) are answered by the system's real Direct3D 9, which is loaded from the Windows folder and never asked to create a device, so the game takes the
decisions it takes with the real driver; the device and everything it draws are ours.

```
# 32-bit (the game is x86): static C runtime, no redistributable needed; the result is build_nue32\d3d9.dll (about 5 MB)
cmake -S nue_d3d9 -B build_nue32 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_FLAGS=-m32 -msse4.2" "-DCMAKE_C_FLAGS=-m32 -msse4.2" -DNUE_STATIC_CRT=ON
cmake --build build_nue32 --target nue_d3d9_dll
# 64-bit (tests only): cmake --build build_nue_rel --target nue_d3d9_dll
```

* **The recompiled launcher** (`nfsu2_launcher.exe`): copy `d3d9.dll` next to it. `native32.cpp` loads it instead of the system's for the guest's `d3d9.dll`
  imports (and trusts its code like a system library in the "native target" check); without the file nothing changes, and `NFSU2_SYSTEM_D3D9=1` ignores it.
  The guest's own calls are untouched: they go through the same bridge into our COM objects.
* **The original game** (`SPEED2.EXE`): copy `d3d9.dll` next to the executable (Windows looks in the application folder first).
* **F12** saves the frame just presented (the game's back buffer, before scaling to the window) as `nue_screenshot_N.bmp` next to the dll, to compare images.
* Optional `nue_d3d9.ini` next to the dll (see `nue_d3d9/dll/nue_d3d9.ini.sample`): `renderer=d3d11|vulkan`, `vsync`, `render_thread`, `worker_thread` (see "Worker-thread mode"), `log`, `shader_cache`.
  Log: `nue_d3d9.log` next to the dll (device creation, resets, and at exit what the game asked for that the translator cannot do). To go back to the
  system's Direct3D 9 delete the file.
* **Verified without the game**: the replay tool drives the dll through real COM calls with the recorded traces (`NUE_D3D9_DLL=<path> nue_replay <trace> --native
  --frames N`): the same images as the direct path (menu mean error 1.06, loading 0.92), 0 unsupported records, in the 64-bit build and in the 32-bit build run
  from a 32-bit process. **Not verified**: the game itself (nobody ran it with this dll yet): expect to look at `nue_d3d9.log` first.
* **Floating point state**: the real runtime forces the x87 control word to `0x007F` (single precision, every exception masked) in `CreateDevice`/`Reset` unless
  `D3DCREATE_FPU_PRESERVE` is given (then it only masks invalid-operation, zero-divide, overflow and underflow), and leaves it that way. The dll does the same
  (`apply_d3d9_fpu_mode`): the recompiled game unmasks exceptions itself and crashed with `0xc0000090` (`fdivrs` in translated code) one second after
  `CreateDevice` when the dll left them unmasked.
* Not offered through COM (the game does not use them in any capture): volume textures, queries, `UpdateSurface`, `ColorFill`, `DrawPrimitive`
  (non-UP), `DrawIndexedPrimitiveUP`, patches, additional swap chains, multiple devices, `GetRenderTargetData` of anything but the back buffer (see the
  coverage table). Display-mode changes for fullscreen are not done: the frame is scaled to the game's window.

## Layers

```
 game (Direct3D 9 calls)  ->  facade (COM objects, refcounts, guest memory; NOT in the library)
        |  d3d9::Device  (Level A: Direct3D 9 semantics, handles, spans)          nue/d3d9_device.hpp
 +------+--------------------------------------------------------------------------+
 | nue::d3d9::Layer: state shadow, state blocks, resources and locks, formats,     |   src/layer/
 | vertex declarations, shader translation + fixed function as generated shaders,  |
 | passes (render targets), clears, buffer policy                                   |
 +------+--------------------------------------------------------------------------+
        |  gpu::Gpu  (Level B: what a modern API does)                              nue/gpu.hpp
   bgfx adapter (bgfx/)                 null Gpu (tests, deterministic hash)        nue/null_gpu.hpp
```

Other parts: `src/shader/` (Direct3D 9 shader bytecode decoder, SPIR-V writer, SPIRV-Cross wrapper), `src/d3d9_trace.cpp` (trace reader and headless
replay of recorded Direct3D 9 call traces), `tools/replay*.cpp` (the replay tool, native Direct3D 9 ground truth, image comparison).

## Public API

```cpp
#include "nue/d3d9_layer.hpp"
#include "nue/bgfx_gpu.hpp"

gpu::BgfxOptions o; o.window = hwnd; o.window_width = 1280; o.window_height = 720; o.renderer = gpu::Renderer::d3d11;
std::string error;
auto gpu   = gpu::create_bgfx_gpu(o, error);        // or nue::NullGpu for tests
auto layer = d3d9::create_layer(*gpu);               // implements d3d9::Device
layer->reset({1280, 720, /*D3DFMT_X8R8G8B8*/ 22, /*D3DFMT_D24S8*/ 75, 0, 1, true, 0});
auto vb = layer->create_vertex_buffer(bytes, usage, fvf, pool);          // D3DUSAGE_* / D3DPOOL_* as the published numbers
auto lr = layer->lock_buffer(vb.value, 0, 0, flags);  std::memcpy(lr.value.data, vertices, n);  layer->unlock_buffer(vb.value);
layer->set_vertex_shader(...); layer->draw_indexed(...); layer->present();
```

`d3d9::Device` (about 50 methods) takes Direct3D 9's own numbers (`D3DRS_*`, `D3DFMT_*`, `D3DPT_*`, ...) as integers. `Layer::stats()` counts what
happened, `Layer::report()` lists unsupported features that were met (each is logged once), `Layer::flush()` closes the open pass.

Threading: one game thread calls the Layer; the adapter hides bgfx's render thread (`BgfxOptions::single_threaded` calls `bgfx::renderFrame()` first
so there is none, which the tools use). Handles are `Id<Tag>` (`uint32_t`, 0 = none), never pointers.

## Coverage (honest)

Exercised = replayed from real captures of the game (`captures/menu`, `captures/loading`, `captures/garage`) and compared with the reference images.
Ground truth = the same calls drawn by the system's Direct3D 9 and by the dll, pixel by pixel (`nue_state_test` 117 cases, `nue_format_test`,
`nue_defaults_test`, see "Ground-truth tests"). Through COM = what the dll offers to a game (`nue_d3d9/dll`); the layer (`d3d9::Device`) offers a little more.

| Area | State |
|---|---|
| Resources: textures 2D/cube, vertex/index buffers, render targets, depth surfaces, declarations, FVF, shaders | implemented, exercised |
| Lock / Unlock of buffers and textures (`DISCARD`/`NOOVERWRITE`, rectangles, relock of managed textures returns their contents) | implemented, exercised, ground truth. `D3DLOCK_NO_DIRTY_UPDATE` and `AddDirtyRect` are accepted and ignored (the locked range is always uploaded) |
| `UpdateTexture` (system memory -> default pool, all levels) | implemented, ground truth |
| Texture formats: A8R8G8B8, X8R8G8B8, DXT1/3/5 (native BCn, decoded to RGBA8 when the GPU lacks it), A8, L8, A8L8, R5G6B5 (decoded on the CPU), A4R4G4B4, A1R5G5B5, X1R5G5B5, V8U8 (signed RGBA8), G16R16 (blue = 1 as on the real runtime), R16F/G16R16F/A16B16G16R16F, R32F/A32B32G32R32F, depth D16/D24S8/D24X8 | implemented, ground truth (`nue_format_test`, every format read back after a draw) |
| Render states: Z, blend (incl. separate alpha), alpha test, cull, fill, colour write mask, stencil (one side), depth bias, fog, texture factor | implemented, ground truth. **Not**: two-sided stencil, user clip planes (`SetClipPlane` accepted, ignored), scissor rectangle (`SetScissorRect` stored, not applied), point sprites, multisample mask |
| Sampler states: filters, addressing (wrap/mirror/clamp/mirror once), anisotropy, `MIPFILTER NONE` (top level only), `MAXMIPLEVEL` | implemented, ground truth. **Not applied**: `MIPMAPLODBIAS`, border colour (bgfx has no per-draw border colour; border addressing samples as clamp), sRGB read/write |
| State blocks (Begin/End/Apply/Capture, `CreateStateBlock` all/pixel/vertex) | implemented, exercised; Apply runs from compiled lists. The `Get*` calls after an Apply return the facade's shadow, which Apply does not update |
| Programmable pipeline: vertex and pixel shaders 1.x, 2.0, 2.x, 3.0 (control flow `if`/`else`/`rep`/`loop`/`break`, relative addressing, `sincos`, `pow`, `nrm`, `lit`, `texldl`/`texldb`/`texldp`, `dsx`/`dsy`, `texkill`, integer/boolean constants, `vPos`, `vFace`) | implemented; shader model 1.x exercised by the game (45 shaders); models 2/3 validated against the real Direct3D 9 with 33 HLSL cases (`spike/shader_spike/sm_cases`, 126 renders, 0 failures). **Not tested**: `call`/`callnz`/`ret`, predication (`setp` + `(p0)`), `oDepth`, multiple render targets, vertex texture fetch |
| Fixed function: transform, vertex colours (missing colour = material colour), texture transforms incl. `D3DTTFF_PROJECTED`, texture-coordinate generation (camera-space normal, position, reflection vector, sphere map), lighting (directional, point, spot; diffuse, specular, ambient, emissive; material sources), texture-stage combiners (all `D3DTOP_*` but bump mapping and premodulate), specular add, vertex fog | implemented, ground truth (the garage car's reflections use it). **Not**: vertex blending / indexed vertex blending, tweening, `D3DTOP_BUMPENVMAP(LUMINANCE)`, `D3DTOP_PREMODULATE`, N-patches |
| Fog: vertex fog, table (pixel) fog with the fixed pipeline | implemented; table fog with a vertex shader: **not** |
| Render targets: textures, surfaces, cube faces, mip levels, depth; a depth surface larger than the target (or with another sample count) | implemented; the larger depth case gets a depth surface of the target's size for that pass (bgfx needs equal sizes), so depth written in such a pass is not seen by a later pass that uses the big surface with another target. Multiple render targets: attachments supported, untested |
| `StretchRect`, `Clear` (full, rectangles, viewport), depth/stencil clear | implemented (copy, or a draw when sizes differ) |
| `GetRenderTargetData` | the back buffer only |
| Multisampling (`MultiSampleType` 2-16 and `NONMASKABLE` with quality levels) | implemented: `NONMASKABLE` quality 0/1/2+ -> 2/4/8 samples, capped by the backend and by `msaa=` |
| Through COM, **not offered** (`D3DERR_NOTAVAILABLE`/`INVALIDCALL`, logged in `nue_d3d9.log` at exit): queries (`CreateQuery`; the game falls back), `DrawPrimitive` (non-UP), `DrawIndexedPrimitiveUP`, `ProcessVertices`, `UpdateSurface`, `ColorFill`, volume textures, patches, additional swap chains, `GetFrontBufferData`, palettes, `CreateOffscreenPlainSurface` in the default pool | not implemented (no capture of the game uses them) |
| Gamma ramp, cursor, software vertex processing | accepted and ignored |
| Lost device / `Reset` | `Reset` resizes and restores every default (as the real runtime does); the device is never lost |
| Fullscreen | not done: the device is always windowed and the frame is scaled (aspect kept) into the game's window; no display-mode change |
| Backends: bgfx Direct3D 11 and Vulkan | both working with the same fidelity (loading capture, mean MAE 0.99 on both) and speed; the Vulkan path hands our SPIR-V to bgfx as is, with the vertex input locations renumbered to the position in the container's attribute list (bgfx binds by position) and a depth-format fallback. Direct3D 12, OpenGL(ES), Metal: not written (the shader path emits GLSL/MSL, the container for them is not written) |

## Dependencies in detail: device parameters, files, build, guest pointers

The dependency list by layer (window, threads, system dlls, diagnostics) is the section "Dependencies on Windows and on the game repo" written on the
worker-thread branch; this section adds the details it does not list. Nothing was restructured.

**`CreateDevice` / `Reset` parameters** (`dll/main.cpp` `Direct3D9::CreateDevice`, `dll/device.cpp` `Device9::Reset`)

* Used: `BackBufferWidth/Height` (0 = the window's client size), `BackBufferFormat` (`UNKNOWN` = X8R8G8B8), `BackBufferCount` (0 = 1),
  `EnableAutoDepthStencil` + `AutoDepthStencilFormat`, `MultiSampleType` + `MultiSampleQuality` (`NONMASKABLE` quality 0/1/2+ = 2/4/8 samples, capped
  by the backend and by `msaa=`), `PresentationInterval` (`IMMEDIATE` turns vsync off, anything else follows `vsync=`), `hDeviceWindow` (else the focus
  window), behaviour flag `D3DCREATE_FPU_PRESERVE`.
* Ignored: `Windowed = FALSE` (no exclusive fullscreen, no display-mode change: the frame is scaled into the window), `FullScreen_RefreshRateInHz`,
  `SwapEffect`, `Flags` (lockable back buffer, discard depth), a `BackBufferCount` above 1, the other behaviour flags (`MULTITHREADED` does not make
  the dll thread safe; software/hardware vertex processing are the same path).
* `Reset` restores every render, sampler and stage state to the real runtime's defaults (measured with `nue_defaults_test`) and keeps the HWND unless
  a new one is given. The device is never lost (`TestCooperativeLevel` is always `D3D_OK`).

**Files and configuration** (folder = the dll's, from `GetModuleFileNameA`)

* `nue_d3d9.ini`, section `[graphics]`, read with `GetPrivateProfileStringA` (`ini_value`): `renderer`, `vsync`, `render_thread`, `log`,
  `log_unsupported`, `shader_cache`, `msaa` (`off`, `auto`, N), `texture_copy_mb`, `texture_report_at`, `mark_missing_textures`,
  `render_thread_priority`, `render_thread_cpu`.
* Environment (wins over the ini): `NUE_RENDERER`, `NUE_VSYNC`, `NUE_RENDER_THREAD`, `NUE_TEXTURE_REPORT_AT`, `NUE_MARK_MISSING`,
  `NUE_RENDER_THREAD_PRIORITY`, `NUE_RENDER_THREAD_CPU`. `NUE_MSAA` is read by the layer itself (`src/layer/layer_resources.cpp`: the one `getenv` in
  the portable part). Debugging only, in the adapter: `NUE_NO_ANISO`, `NUE_NO_BC`, `NUE_LOG_CAPS`, `NUE_DUMP_GLSL`; in the dll: `NUE_PDH_DEBUG`.
* Written: `nue_d3d9.log` (appended), `nue_shader_cache\<hash>.nsc` (Direct3D 11 bytecode only, key = hash of the translated shader and the target;
  `shader_cache=<folder>` moves it), `nue_screenshot_N.bmp` (F12), `nue_textures_N.txt` (F11 or `texture_report_at`). F10 toggles drawing textures
  without data as a magenta checker.

**Build**

* `nue_d3d9/CMakeLists.txt` sets `REPO` to the folder above it and builds bgfx and SPIRV-Cross from `${REPO}/third_party/bgfx.cmake` (bgfx's
  vendored SPIRV-Cross; the translator writes SPIR-V itself, no glslang). Nothing else of the game repository is compiled into the dll.
* 32-bit dll: clang `-m32 -msse4.2`, `NUE_STATIC_CRT=ON`; `dll/x86_stdcall_fix.h` undoes bx's `#define __stdcall` (wrong COM calling convention on
  x86 otherwise); `NUE_EXPORT` writes the x86 decorated export names.
* The game repository's top `CMakeLists.txt` compiles `nue_d3d9/src/d3d9_trace.cpp` and `nue_d3d9/src/null_gpu.cpp` into the launcher (its recorder
  `d3d9_recorder.cpp` writes the trace format), adds `nue_d3d9/include` to the include path, and `mods_system.cpp` includes `nue/window_fit.hpp`.
* The launcher side of the drop-in lives in the game repository: `native32.cpp` `d3d9_override_module` loads `d3d9.dll` from the launcher's folder for
  the guest's `d3d9.dll` imports and trusts its code range like a system dll (`NFSU2_SYSTEM_D3D9=1` turns it off).

**32-bit guest pointers**

* Library: none. Objects are `Id<Tag>` handles (`uint32_t`, 0 = none); locked memory is a `LockedRegion` the layer owns (the header notes that the
  shim may place it in the guest arena).
* dll: real COM pointers, no guest addresses. In the recompiled launcher it must be the 32-bit build: the guest is 32-bit and calls the objects through
  the launcher's bridge, so their pointers, vtables and locked memory have to be 32-bit addresses of the same process. The 64-bit dll is for the tools.
* Trace format (`nue/d3d9_trace.hpp`): objects are named by the 32-bit guest address of their COM object.

## Known open issues

* The game thread is mostly translated game code: in a race the dll is 3.7 ms of a 42-65 ms frame with the render thread (8 %). Faster frames come
  from the speed layer of the recompilation, not from this library.
* Worker-thread mode (translator and bgfx on a thread of their own) is on branch `claude/laughing-heyrovsky-c8cd6f`, off by default, not on this branch.
* Texture contents kept for relock and `UpdateTexture` are capped by `texture_copy_mb` (default 160 MB): past the cap a relock returns zeros, the
  failure that hid the garage car's environment map before the copies existed.
* `BeginStateBlock` failed three times at the start of one live session (seen in `nue_d3d9.log`, not reproduced by any capture).
* Not implemented: scissor rectangle, user clip planes, two-sided stencil, LOD bias, border colour, sRGB, vertex blending, point sprites, bump mapping,
  queries, volume textures (no capture of NFSU2 needs them; Most Wanted may).
* A depth surface larger than the target gets a substitute of the target's size per pass (see "Known differences").
* `Get*State` after `StateBlock::Apply` returns the values from before the Apply (the facade's shadow is not refreshed).
* 4x multisampling costs the same GPU time as 8x on the test machine's integrated GPU; the test folder uses `msaa=off`.
* Never lost, never fullscreen: alt-tab and display-mode changes are left to the window.
* The first frame of every recording window differs more in replays (state from before the window was not recorded).

## Shader path

The game's shaders are Direct3D 9 **bytecode** (game data: never in the repository). At run time:
`bytecode -> decoder (src/shader/d3d9_shader.cpp, disassembly identical to Microsoft's on the 45 shaders) -> SPIR-V (our lowering, d3d9_to_spirv.cpp)
-> per renderer: Vulkan takes the SPIR-V; Direct3D 11/12: SPIRV-Cross -> HLSL -> D3DCompile (d3dcompiler_47.dll, Windows only); OpenGL(ES)/Metal: SPIRV-Cross
-> GLSL/MSL` and is wrapped in the bgfx shader container (`bgfx/shader_container.cpp`, written from `bgfx/tools/shaderc`'s format) so **no shaderc is
needed at run time**. The fixed-function pipeline is generated as decoded shader programs (`src/layer/fixed_function.cpp`) and goes through the same
path, so there is one translator, not two. Programs are cached per (vertex shader, pixel shader, key); a key covers what changes the code: sampler
types, projective coordinates, alpha-test function, fog mode, `D3DCOLOR`/integer vertex attributes.

Decision (build time vs first run): **first run, in memory**, because the bytecode is the user's data. Measured on this PC (Direct3D 11, 25 programs):
SPIR-V -> HLSL about 0.5 s and D3DCompile about 1.0 s in total for the 3 first frames (about 60 ms per program, all at first use). A persistent cache keyed
by (bytecode hash, backend, translator version) and a warm-up step are **not written yet**; mobile needs them (Vulkan needs no compile at all; GLES/Metal compile
in the driver).

## Buffer policy (the real game locks the same buffers ~90 times per frame)

Every buffer keeps a CPU copy; `Lock` always answers from it, so **a lock never waits for the GPU**. A buffer that is *never rewritten after it was drawn
from* becomes an immutable bgfx buffer (a new generation when it changes). A buffer that is rewritten after being drawn from (score from the unlock history), or has
`D3DUSAGE_DYNAMIC`, is served through **transient copies of the range each draw reads** (de-duplicated per frame); indices are rewritten to be relative to the first
vertex read when needed, triangle fans become lists, `DrawPrimitiveUP` data goes straight into transient buffers. The captured race scenes: 2 MB of static buffers
uploaded, 0.3 MB of transient data per 3 frames (the bulk of the draws read static geometry). bgfx applies resource updates before a frame's draws, which is why in-place
updates between draws are never used.

## Memory policy

Textures are uploaded at `Unlock` and the staging memory is freed (`keep_texture_copies` keeps it). DXT stays compressed on GPUs with BCn (the Windows ones);
on GPUs without it the layer decodes to RGBA8 (4x the memory). Transcoding to ASTC/ETC2 for phones, a texture-memory budget (2 GB process on Android) and
mip dropping are **not implemented**. Buffer CPU copies are kept for every buffer (the captures hold ~100 MB of resources).

## How it differs from DXVK / Wine d3d9

Same job (Direct3D 9 on a modern API) with different choices: DXVK targets Vulkan only and is written against its own Vulkan layer; Wine's wined3d targets OpenGL
(and Vulkan/Direct3D 12 backends) and is LGPL. This library goes through bgfx, so it reaches Direct3D 11/12, Metal and OpenGL ES with one front end; the API
is handle-based so it works on a 64-bit host without COM; and it was built to be driven by *recorded call traces* (replay, per-draw comparison against the
real Direct3D 9 runtime), not only by a live game. Code from DXVK (zlib licence) and Wine may be reused where it saves time; where code is copied the
file says so. So far nothing was copied: the design of the fixed-function generator and the half-pixel/position-fixup rules were checked against DXVK's behaviour only.

## Tools and how to verify

```
cmake -S nue_d3d9 -B build_nue -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build_nue   # always measure an optimised build
build_nue/nue_replay.exe <trace> --null --twice            # headless: statistics + deterministic call hash (run twice: identical)
build_nue/nue_replay.exe <trace> --bgfx d3d11 --frames 3 --out dir   # renders, saves ours_*.bmp / diff_*.bmp, prints MAE and p99.9 outside edges
build_nue/nue_replay.exe <trace> --bench --bgfx d3d11 --mt --loops 3 --window 0 --shader-cache cache_dir   # frame time, layer/adapter/GPU profile
build_nue/nue_replay.exe <trace> --native --frames 3 --out dir       # the same trace on the real Direct3D 9 (ground truth, tool only)
build_nue/nue_replay.exe <trace> --bgfx d3d11 --frames 1 --draws 0-1108 ...   # only draws 0..1108: bisect a difference against --native
# debugging aids (environment variables of the replay tool): NUE_RECENT_DRAWS=<frame> NUE_RECENT_COUNT=<n> (the last draws with their gpu ids), NUE_SKIP_DRAWS=a-b,
# NUE_MEAN_TEX=<gpu texture>:<first>-<last> (+ NUE_SAVE_TEX=<prefix> to save it), NUE_DUMP_GLSL=<dir> (translated shaders as GLSL), NUE_LOG_PASSES, NUE_FRAME_DRAWS
python tools/bmp_compare_crop.py ref.bmp ours.bmp out.png x y w h   # reference | ours | amplified difference
python tools/bmp_diff_grid.py ref.bmp ours.bmp                        # where the images disagree
python tools/d3d9_trace_census.py <trace>                             # what a trace asks of a translator
```

Metrics: mean absolute error over RGB (0-255) and the 99.9th percentile of the per-pixel maximum channel difference outside a one-pixel edge mask (pixels
within one pixel of a gradient above 32 in the reference). Thresholds are **not fixed yet**: the first results are in "Measurements".

## Measurements

Machine: AMD Radeon (integrated), Windows 11, Direct3D 11 through bgfx, offscreen backbuffer 1280x720, optimised build (RelWithDebInfo). Scenes: the
recorded in-race windows of `captures/menu` and `captures/loading`, replayed from memory (`--bench`, trace decoding included: the real game would add
none of it). A "frame" is one game frame: 1500-1700 draws and ~50k recorded calls.

| Scene / mode | mean | p99 | notes |
|---|---|---|---|
| menu window 0, null GPU (layer only) | 5.2 ms | 6.1 ms | layer ~1.3 us per draw |
| menu window 0, D3D11, one thread | 11.2 ms | 13.3 ms | bgfx::frame() runs the backend inline (4.5 ms) |
| menu window 0, D3D11, render thread (`--mt`) | 6.4 ms | 7.9 ms | CPU 4.5 ms, GPU 6.4 ms per frame |
| menu window 3 (975 draws), D3D11 one thread | 4.8 ms | 10 ms | |
| menu window 0, Vulkan, render thread | 6.6 ms | 8.0 ms | GPU 4.6 ms |
| loading window 0, D3D11 `--mt` | 6.5 ms | 7.7 ms | one run had a stretch of ~16 ms frames (GPU clock of the integrated part; not reproduced) |

So the translator itself fits a 60 fps budget (16.6 ms) with room to spare on this machine; the game's own CPU work is not included. Shader translation
cold cost: 25 programs = ~1 s of the Direct3D compiler (about 20 ms per program), which is a stall the first time a program is used; the disk cache
(`BgfxOptions::shader_cache_dir`) removes it from the second run. Process memory of the benchmark: ~870 MB, most of it the cached trace window.

Image fidelity (mean absolute error 0-255 against the recorded reference images, Direct3D 11): all 24 reference frames of `captures/menu` have a mean of
**1.06** (typical frame 0.6-0.8, the first frame of each recording window 2.6-3.4 because the game state before the window was not recorded); the 15 frames
of `captures/loading` (three windows) have a mean of **0.99** (typical 0.4-0.9, first frame of a window up to 3.0, one 2.2). The native Direct3D 9 replay of
the same traces sits at 0.6-0.9, which is the floor of the method (the reference images come from a multisampled back buffer, ours is not multisampled).
Bug history that mattered: setting render target 0 must reset the viewport to the whole target (Direct3D 9 behaviour); without it the post-processing
chain of the game rendered 4x too large and the bloom washed the image out (loading frame 1829 went from MAE 32 to 0.94, the menu from 4.3 to 0.7).

## Multisampling

The game asks for a multisampled back buffer (`D3DMULTISAMPLE_NONMASKABLE` with a quality level, as the recorder's reference images say). Without it cars and
every silhouette were jagged and shimmered. The layer now maps the request to a sample count (`samples_for`): type 2..16 = that count; NONMASKABLE: quality
0-1 = 2 samples, 2-3 = 4, 4 and above = 8, and an unknown quality (the recorded traces do not keep it) = 8; the real AMD runtime here offers 8 quality levels.
The adapter makes the back buffer, the automatic depth surface and any render target or depth surface with the same multisample type multisampled
(`BGFX_TEXTURE_RT_MSAA_Xn`; they must all agree or bgfx cannot make the framebuffer), and bgfx resolves when the texture is sampled or copied
(`StretchRect` of the back buffer = the resolve). `nue_d3d9.log` says "multisample: requested type T quality Q -> N samples"; `msaa=` in `nue_d3d9.ini`
forces 1 (off), 2, 4 or 8. Measured on the car frame of `captures/loading` (frame 1829, crop of the car): error on the edge band 3.68 (no MSAA), 2.75 (2x),
2.36 (4x), **2.28 (8x)**; whole-image mean error of the three loading windows 0.99 -> **0.75**, of the four menu windows 1.06 -> **0.81**; the real system
Direct3D 9 without multisampling scores 0.95 on the same trace. Cost: 8x on an integrated Radeon at 1280x720 is about 12.7 ms of GPU time against 7.0 ms
without (still 78 fps); `msaa=4` halves the extra cost.

Texture filtering was checked at the same time and is right: the game uses anisotropic filtering (min/mag = 3, 16 samples) with linear mips, wrap/clamp
addressing, LOD bias 0, no sRGB and no auto-generated mips in any capture; turning anisotropy off doubles the error (1.7 vs 0.9), so it is applied.

## Window size, resource limits and the audit in `nue_d3d9.log`

* **Output size.** The back buffer is never stretched to the window: the adapter draws it fitted into the part of the window that is visible on its monitor, keeping the aspect ratio
  (`nue/window_fit.hpp`, `fit_aspect`: equal aspect fills, otherwise letterbox / pillarbox, centred). The facade passes the visible part of the client area (`ClientToScreen` clipped to the monitor)
  to `Gpu::set_visible_area`. The game's own HWND is respected; the launcher's scaled-window mode (a "virtual" resolution the monitor does not list, such as windowed 1280x720) uses the same function
  for the window it creates, so it fits the desktop instead of being sized from the desktop height only.
* **bgfx handle limits** are compile-time: the defaults (4096 textures, vertex buffers and index buffers; 128 frame buffers; 64 vertex layouts) are raised in `CMakeLists.txt` (16384 / 16384 / 16384 / 1024 / 1024).
  Every bgfx creation is checked with `bgfx::isValid`; a failure is counted and logged (`bgfx could not create: ...`, with the handle usage), and the draws that need the object are skipped (counted in `draws skipped`).
* **Periodic audit** (first after 120 presents, then every 600): `audit: textures: N alive (...), created, failed; sampled-before-upload; never uploaded; partly uploaded; locked now; buffer failures; uploads`, the eight
  most common (format, size, levels) groups with how many are complete / partial / empty, and `audit: bgfx handles in use / limit` (textures, buffers, frame buffers, layouts, programs, shaders, uniforms, memory).
  A texture sampled by a draw before any data reached its top level is logged once (`sampled by a draw before any data was uploaded to it`).
* **Profile line** (every 3 s): `profile: inside the Direct3D 9 dll X ms per frame (Y% of wall time), N calls per frame; most expensive: ...` shows how much of the frame the dll itself takes; the rest of the frame is the game.

## Finding missing textures in the live game

* **F11** writes `nue_textures_N.txt` next to the dll (and a screenshot of that same frame): one line per texture the next frame's draws sample, with id, size, format, levels, pool and usage, the mip levels that
  have data, a hash of level 0 (equal hashes = same picture; a changed hash = the game updated it), uploads (and how many came after the texture was first used), `NO-DATA`, `ALL-ZERO`, `MIPS-MISSING`,
  `SYSTEM-MEMORY-ONLY`, `SAMPLED-BEFORE-RENDERED-THIS-FRAME` / `SAMPLED-BUT-NEVER-RENDERED` for render-target textures, and the first draw (number inside the frame, stage, pixel shader id) that used it.
* **F10** (or `mark_missing_textures=1`) draws every sampled texture that has no data as a magenta / black checker; the audit counts those draws.
* **F12** screenshot of the game's back buffer.
* A level that is locked again returns what it held (a copy kept in system memory, up to `texture_copy_mb`), like the managed pool of the real runtime; before, a lock without `DISCARD` returned zeros, so a game that
  changed part of a level wrote the rest back as black. `UpdateTexture` (system memory -> default pool) is implemented.
* `nue_window_test <d3d9.dll>` checks the window behaviour with windows that are never shown: the dll creates no window, leaves the game's style and position alone, follows resizes and moves, and keeps the game's
  back buffer size through `Reset`.

## Ground-truth tests against the system runtime

`nue_format_test <d3d9.dll>` and `nue_state_test <d3d9.dll> [filter]` run the same Direct3D 9 calls on a window that is never shown through the dll and through the system `d3d9.dll`, and compare the pixels.

* **Formats** (8x8, two mip levels, Lock/Unlock, point sampling, colour and alpha): A8R8G8B8, X8R8G8B8, R5G6B5, X1R5G5B5, A1R5G5B5, A4R4G4B4, A8, L8, A8L8, V8U8, Q8W8V8U8, A2R10G10B10, G16R16, DXT1/3/5 all match.
  Found and fixed by it: R5G6B5 came out with red and blue swapped (now decoded on the CPU), V8U8 / Q8W8V8U8 were read as unsigned (now signed RGBA8: V8U8 reads as (u, v, 1, 1)), G16R16 had blue 0 instead of 1.
* **States** (110 cases): alpha test (8 functions), 13 blend factor / operation pairs, depth bias and slope-scaled depth bias, cull modes and windings, colour write masks, every texture-stage operation as stage 0 and as stage 1 after a modulate,
  alpha operations, texture-factor and complement / alpha-replicate arguments, the temporary register, texture transforms, texture coordinate generation (camera-space normal, position, reflection vector with a cube map; sphere map) and
  fixed-function lighting (directional, point with attenuation and range, spot cones, specular with local / infinite viewer, several lights, colour material sources) all match.
  Found and fixed by it: texture transforms and coordinate generation do not apply to pre-transformed (XYZRHW) vertices in the real runtime; coordinate generation and lighting did not exist; when the vertex has no colour the material colour is the source
  (not white); the spot factor is not capped at 1 inside the inner cone.
* Not covered by the tests: vertex blending, point sprites, bump-map operations, projected texture coordinates (`D3DTTFF_PROJECTED`), user clip planes, the indexed user-pointer draw, volume textures.

## The garage car (what the live game did that a replay cannot)

Replaying the garage capture (`captures\garage`, the native Direct3D 9 as reference) through the layer, through the COM dll, in 32 and 64 bits gives the same picture as the real runtime (car crop MAE 1.2, logo, headlights, windshield reflection present),
so the shaders, states, formats and cube sampling are right for what the game sends. The live game looked flat (dark glass and headlights, matte paint) because its environment cube map was empty: the game builds the 256x256 cube from six 2D textures by
locking them again (`Lock` without `DISCARD`) and copying their pixels, and a lock used to return zeros for a texture that already held data (the trace records only what was written, so no replay can show this). A lock now returns what the texture holds
(`texture_copy_mb`), `nue_format_test` checks that a Lock after Unlock returns what was written, and `NUE_DEBUG_ZERO_CUBES=1` shows the replay with empty cube maps (it looks like the live picture).
Also found with `nue_defaults_test` (device state right after CreateDevice and Reset, real runtime against the dll): `Reset` did not return the state to its defaults and the state the dll reports with `Get*State` differed from the real defaults; both fixed.

## Where the time of a live frame goes (`timeline:` in nue_d3d9.log)

Every 3 s the log has a `timeline:` line with the frame as the game thread lives it: wall time per frame = game code outside the dll + time inside the dll (of which waiting for the render thread), the share of the wall time the game
thread was running on a CPU (`CPU busy`: ~100 % means CPU bound, the game thread never waits for anything), how long the render thread was busy and how long it waited for the game thread, the GPU time, the cores the whole process used, and a
measured core speed (a fixed chain of integer operations timed in the heartbeat; 100 % = the best seen since the start, a later 80 % means the cores run 20 % slower than then: the operating system reports the rated clock all the time on this machine).
`threads:` lines list the game thread and the threads that appeared while bgfx started, with their priority. Compare with the same scene on the system runtime: `NFSU2_SYSTEM_D3D9=1` makes the launcher ignore the dll; `--profile` of the launcher writes the main thread's
time inside Windows and instructions per second to logs
ative_trace.log.
Numbers measured in a race of the real game (nue_d3d9.log, 8x MSAA, window 1280x720): 17,078 calls per frame cost 3.65 ms inside the dll (8 % of a 42-65 ms frame); Present takes 0.06 ms (the game thread never waits for the render thread); the GPU takes 12-13 ms and
the render thread 4-5 ms per frame, in parallel. Replay of the loading capture: 1661 draws per frame, layer 1.5 us and adapter 0.8 us per draw; GPU 6.5 ms with msaa off, 7.9 with 2x, 12.3 with 4x and with 8x.

### What the live race log showed (render_thread=0, msaa=off)

With `render_thread=0` bgfx has no render thread: the whole backend (4-5 ms of Direct3D 11 submission) runs on the game thread inside `Present`, so `Present` shows 4.9 ms per frame, "inside the dll" doubles (3.65 -> 8.2 ms), the render
thread stats read 0 (there is none: its busy time is the Present time) and the GPU frame span equals the frame (it runs from the first to the last timestamp of a frame and includes the gaps before the commands arrive).
`render_thread=1` (the default) gives Present 0.1 ms. The timeline now prints the GPU engine's real busy share from the operating system's counters (`GPU 3D engine really busy`), the time in `bgfx::frame()`, and `StateBlock9::Apply` is timed like every other call.
Measured on the loading capture (replay, same machine): GPU busy 5.1 ms per frame at full speed and 6.9 ms when the game is the bottleneck at 45 ms per frame (the GPU runs at lower clocks when its duty cycle is low: with bgfx's own frame span 7.5 -> 10.0 ms at msaa off and 14.1 -> 19.7 ms at 8x).
The render thread priority and ideal processor (`render_thread_priority`, `render_thread_cpu`, or the environment variables NUE_RENDER_THREAD_PRIORITY / NUE_RENDER_THREAD_CPU) made no measurable difference to the replay's main thread (22.5 / 22.3 / 22.7 / 21.7 / 22.7 ms per frame for default / lowest / above / cpu 14 / cpu 6).
`NUE_BENCH_PACE_MS=N` makes the replay tools present one frame every N ms (a game that is the bottleneck) to measure the GPU side under that duty cycle.

## Worker-thread mode (`worker_thread=1`)

`nue_d3d9.ini` `worker_thread=1` (or `NUE_WORKER_THREAD=1`; default 0) puts the translator and the bgfx calls on a thread of their own, as DXVK's
command-stream thread does: the game thread only records each call into a queue (`nue_d3d9/dll/threaded.cpp`, `ThreadedLayer`, a `d3d9::Layer` in
front of the real one), and the worker replays the queue on the real layer and the adapter. The worker also creates and destroys the adapter and the
layer, because bgfx must be called from the thread that started it (bgfx keeps its own render thread behind that).

* Queue: a chain of 1 MB blocks, one writer and one reader, calls as small fixed records (draw data and shader constants copied inline, larger payloads
  as a heap block); the worker sees new calls every 32 calls and at once at `Present` and whenever the game thread waits; a sleeping worker is woken only every 2048 calls
  (and at `Present`, at a wait, at a new block): in a live race waking it every 32 calls cost 2.4 % of the game thread in `SetEvent`, now 0.1 %. Repeated render, sampler and
  stage states (the layer ignores them anyway) are not queued.
* Answered on the game thread without waiting: creation (ids of the queue, mapped to the layer's ids by the worker; unknown formats are still refused at
  once), buffer locks (memory of the game's side: a fresh block, or a copy of the buffer kept once a lock needed its contents, up to `worker_copy_mb`;
  unlock sends the written range), texture locks of levels without data or with DISCARD (a fresh block sent at unlock), texture locks of levels that
  hold data once that level has a copy here (the level as the game sees it, read once from the worker and kept up to date with every write, within
  `worker_copy_mb`; served with the pitch of the whole level, as the real runtime does; unlock sends the locked part), `TestCooperativeLevel`.
* Waiting for the worker (after everything queued before is done): the first lock that reads a texture level or a buffer (to make its copy), texture
  locks of levels that hold data when the copies are over the budget or the texture has automatic mipmaps (the game then writes into the layer's own
  memory), `Get*ShaderConstant*`, `Reset`, back buffer and depth surfaces, `GetRenderTargetData`, F12, the audits
  and the release of the device. Messages sent to the game's window during such a wait are answered (DXGI may send them from the worker).
* Back-pressure: `Present` waits until the worker has finished the previous frame. Every 3 s the log gets a `worker thread:` line (next to `timeline:`):
  the worker's busy time per frame, the wait in `Present`, and how many times (and how long) the game thread waited outside `Present` and why; the
  totals again when the device is released. On the loading capture: ~3 waits per frame, all texture locks of levels with data, 0.65 ms per frame.

Measured with the COM bench (`NUE_D3D9_DLL=<dll> nue_replay <capture> --bench --bgfx com --loops 3 --window 0`, `NUE_MSAA=1`, hidden window, same
machine as above; mean of three runs each). "Replaying thread CPU" is the CPU time of the thread that plays the game's part, per frame; it includes
the replay tool's own trace decoding (about half of it in worker mode, which the game does not have):

| Capture | build | replaying thread CPU, off -> on | frame mean, off -> on |
|---|---|---|---|
| menu | 64-bit | 6.83 -> 5.09 ms | 6.85 -> 5.12 ms |
| menu | 32-bit | 7.06 -> 5.02 ms | 7.07 -> 5.06 ms |
| loading | 64-bit | 6.51 -> 4.57 ms | 6.82 -> 6.12 ms (GPU bound) |
| loading | 32-bit | 6.73 -> 4.57 ms | 6.90 -> 6.08 ms (GPU bound) |
| garage | 64-bit | 1.81 -> 1.36 ms | 4.05 -> 3.99 ms (GPU bound) |
| garage | 32-bit | 1.78 -> 1.10 ms | 5.46 -> 5.43 ms (GPU bound) |

The worker is busy 4.3-4.5 ms per frame on loading (translator + bgfx calls, its waits for the render thread included), so with the worker the game
thread's share of the dll drops from ~2.5-3.5 ms to ~1.1 ms per frame (COM entry points ~0.7 ms + queueing ~0.4 ms, from `--sample`). Images: the
`--native` COM replays of menu, loading and garage give the same error for every compared frame with the mode off and on (mean 1.057 / 0.920 / 0.702,
32 and 64 bit), and `nue_state_test`, `nue_format_test`, `nue_window_test`, `nue_defaults_test`, `nue_tests` pass with it on. **Not verified**: the game
itself. Replays never read locked data back, so the waits for texture locks of levels with data (34 in 240 garage frames) and for buffer copies (none in
the captures) are what to look at in the live log (`worker thread:` line at the end) before making it the default.

First live race with the mode on (launcher with the faster bridge of the fps work, msaa off): 22.7-36.7 fps, typical 28-30 (bgfx without the worker
~18, the system Direct3D 9 ~23); inside the dll 0.2 ms per frame (was ~8); the worker busy 1.2-3.5 ms per frame; the game thread waited for the worker
outside `Present` 124-360 times per 3 s (0.3-0.8 ms per frame), all of them texture locks of levels that hold data. Those locks are now served from the
copies described above: only the first lock of each level waits.

## Hitches: `spike:` lines and the shader program list

Every frame `Present` hands its numbers to `note_frame` (`nue_d3d9/dll/spikes.cpp`, always on, a copy of a few counters per frame). A frame slower than
max(2.5 x the median of the last 64 frames, 40 ms) gets one `spike:` line: wall time, game code outside the dll, time inside the dll with the three
slowest calls, waits for the worker outside `Present` and why, the worker's busy time, `bgfx::frame()`, render thread and GPU time, draws, resources
created and destroyed (from the COM calls), and the translator's counters of that frame (UP draws, dynamic vertex bytes, buffer and texture uploads,
textures created, new programs with their time, shader builds and compiler time), ending with the biggest part ("mostly ..."). At most 30 lines per
10 s; every 10 s with spikes a `spikes:` line counts them by cause.

New shader programs: the translator times each one (shader translation + the adapter's program, which includes the shader compiler when the disk cache
does not have it; the driver's own shader creation happens on bgfx's render thread at the next frame) and logs the ones of 2 ms or more (`new shader
program ... in frame N`). In the live race the number of programs grew during the race (37 -> 39 -> 44): programs made at their first draw, in the
middle of the race. The dll now keeps a list of the programs made from the game's shaders (`nue_programs.txt` in the shader cache folder:
`LayerOptions::program_list`): in the next run, a listed program is made as soon as the game has created both of its shaders, which it does while
loading. Measured with the captures through the dll: the first run lists 33 programs (about 2-2.5 ms each in the translator), every later run makes
the 33 ahead in 25-35 ms at load and none at a draw; images identical.

## Dependencies on Windows and on the game repo (to replace with the NUE engine's native API)

nue_d3d9 is planned to become the engine's `legacy_d3d9` module (nue-engine repo, LEGACY ABI). What it depends on today, by layer, so that each item
can later be swapped for a native-API call. Nothing here was restructured yet.

**Layer (`src/layer`, `include/nue/d3d9_*.hpp`) and shader translator (`src/shader`)**: portable C++20, no Windows types, no guest pointers
(`include/nue/types.hpp`). Inputs are handles and spans. Two notes: `d3d9_device.hpp` documents that lock memory is handed to a shim that converts
it to a guest address (the recompiled game's 32-bit arena); the trace format (`d3d9_trace.hpp`) names objects by the 32-bit guest address of their
COM object. Logging goes through `LayerOptions::log` (a C function pointer); the program list path comes in `LayerOptions::program_list`.

**bgfx adapter (`bgfx/`)**:
* Window: `BgfxOptions::window` is a native handle (`HWND` on Windows), `window_width/height`, `set_window_size`, `set_visible_area`; the final pass
  scales the game's back buffer into the window with `fit_aspect` from `include/nue/window_fit.hpp` (letterbox; also used by the launcher's scaled window).
* `bgfx::init` / `shutdown` are called by the adapter itself (the seam's PREPARE mode); reset flags in one place (`reset_flags()`).
* Shader compiler: `shader_container.cpp` loads `d3dcompiler_47.dll` with `LoadLibraryA` (Direct3D 11 bytecode) and compiles on a `CreateThread`
  thread; the disk cache is a folder path (`BgfxOptions::shader_cache_dir`). Log through a C function pointer.

**COM facade, the drop-in `d3d9.dll` (`dll/`)**: Windows only by nature.
* Exports `Direct3DCreate9(Ex)`, `D3DPERF_*` with the 32-bit stdcall decorations; COM object layout of the x86 ABI.
* Capability queries (`GetDeviceCaps`, adapter modes, formats) are forwarded to the system `d3d9.dll` (`GetSystemDirectoryA` + `LoadLibraryA`; no
  device is made there).
* `CreateDevice`/`Reset`: the window comes from `D3DPRESENT_PARAMETERS::hDeviceWindow` or the focus window; sizes from `GetClientRect`,
  `ClientToScreen`, `MonitorFromWindow`/`GetMonitorInfo` (visible part of the client area); back buffer size/format/depth/multisample/interval come
  straight from the game's parameters (and `msaa` of the ini).
* x87 control word forced like the real runtime (`apply_d3d9_fpu_mode`, `fldcw`), thread id from the TEB (`fs:[0x24]` / `gs:[0x48]`).
* Configuration: `nue_d3d9.ini` next to the dll read with `GetPrivateProfileStringA`, plus environment variables (`NUE_*`); log file
  `nue_d3d9.log` next to the dll (`GetModuleFileNameA`); shader cache and program list default to `<dll folder>\nue_shader_cache`.
* Diagnostics: vectored exception handler, `set_terminate`/`SIGABRT`/invalid-parameter handlers, a heartbeat thread, GPU busy share from the
  `pdh.dll` performance counters, thread CPU time (`GetThreadTimes`, `QueryThreadCycleTime`), `QueryPerformanceCounter`, `__rdtsc`.
* Threads: `tune_render_threads` sets the priority / ideal processor of the threads bgfx starts (`SetThreadPriority`, `SetThreadIdealProcessor`);
  worker mode uses Win32 events and `MsgWaitForMultipleObjects` + `PeekMessageW(PM_QS_SENDMESSAGE)` so the game thread keeps answering messages sent
  to its window while it waits.
* Keys: F10/F11/F12 read with `GetAsyncKeyState` in `Present` (debug aids).
* Guest pointers: none in the dll itself. It runs natively in the launcher's process; the launcher's bridge turns the guest's 32-bit COM calls into
  calls on these objects. The game's own `HWND`s are real handles.

**Tools (`tools/`)**: hidden Win32 windows, `LoadLibraryA` of our dll and of the system one; test only.

## What the NUE engine's graphics seam expects (nue_engine/docs/GFX_D3D9_SEAM.md, API.md, branch claude/nue-core-0454cf)

* Direction: nue_d3d9 depends on the engine (core + gfx), never the other way round. `nue_gfx` provides the window, the renderer choice, lifecycle
  events (resize, suspend/resume, device lost) and shared policy (shader cache directory under the `cache` VFS root, log routing, power/GPU
  preference); nue_d3d9 is a client.
* First path: PREPARE mode (`NUE_GFX_PREPARE_ONLY`). The engine selects the renderer and hands `nue_gfx_get_native()` values; nue_d3d9 keeps
  calling `bgfx::init` itself, with `BgfxOptions.window = nue_gfx_native.window.window`, `renderer`, `vsync` and sizes. This works with today's code.
* Requested changes (not done yet): `BgfxOptions::external_init` (skip `bgfx::init`/`shutdown`, for MANAGED mode) and `platform_data`; a
  `create_bgfx_gpu_from_gfx(nue_gfx, options)` entry that calls `nue_gfx_get_native`, `nue_gfx_notify_initialized` and registers a listener;
  `set_window_size` driven by `NUE_GFX_NOTIFY_RESIZED`; suspend/resume/device lost mapped to `TestCooperativeLevel` (`D3DERR_DEVICELOST`, then
  `DEVICENOTRESET`) and `Reset`; `Reset` with new back buffer sizes mapped to `nue_gfx_resize`; the shader cache folder from
  `nue_gfx_desc.shader_cache_dir` resolved with `nue_vfs_resolve`.
* Window ownership: the game creates its own HWND; the launcher attaches it with `nue_window_create(desc.attach)`; the engine observes and never
  destroys it.
* Threading: the API says `nue_gfx_present` and bgfx calls run on the render thread. With `worker_thread=1` the dll calls `bgfx::init`, every bgfx
  call and `bgfx::shutdown` on its own worker thread (not the game thread): a MANAGED mode must either init bgfx on that thread or the worker must be
  the thread the engine calls the render thread.
* Renderers: the seam orders Windows D3D11, Vulkan, D3D12, OpenGL; Linux Vulkan, OpenGL, GLES; Android Vulkan, GLES; iOS Metal. The translator is
  tested on D3D11 and Vulkan; GLSL ES and MSL output exist in SPIRV-Cross but are untested here.
* Texture formats are to come from nue-texture (D3DFMT/FOURCC tables, codecs, transcoding for GPUs without BC); today they are the layer's own
  `src/layer/formats.cpp` (decode to RGBA8 when the GPU lacks a format).

## Known differences

* The first frame of every recording window differs more (2-3) because the state accumulated before the window was not recorded.
* A render target drawn with a larger depth surface (legal in Direct3D 9, which uses its top-left part), or one with another sample count, gets a
  depth surface of the target's size and samples of its own for that pass (`resolve_attachments` / `matching_depth` in the layer): the depth test
  inside the pass is right, but its depth values are not shared with the larger surface. Before, bgfx refused the framebuffer and the pass was
  dropped: in the live race at 1920x1080 that was one pass every frame ("could not create: framebuffer (colour texture 7, depth texture 2)").
  `nue_state_test` checks it against the system runtime, with MIPFILTER NONE / MAXMIPLEVEL (now honoured through a LOD clamp per texture).

Note for measurements: a `d3d9.dll` placed next to `nue_replay.exe` is loaded by `--native` instead of the system's (that is the whole point of the dll). For the real Direct3D 9 as ground truth, run a copy of the tool from a folder that does not have the dll.
