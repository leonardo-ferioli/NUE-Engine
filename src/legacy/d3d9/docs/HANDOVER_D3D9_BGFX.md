# Hand-over: Direct3D 9 -> bgfx translator (Phase 2, from step 2b on)

Branch `claude/platform-api-5da14e` (worktree `.claude/worktrees/platform-api-5da14e`), nothing pushed. Design is in `docs/PLATFORM_API.md`
(sections 3 to 6); this note says where the work stopped. Direction given by the user in the last messages: the D3D9 layer + its bgfx adapter + the
shader translator must become a **general, standalone Direct3D 9 translator** (other games of the same engine family will use it): own folder,
own CMake target and tests, no dependency on the guest, `NativePlatform`, the mods or NFSU2 addresses; the guest-COM facade stays outside as thin glue.
Also pending in the docs: a coverage table (what D3D9 is implemented and tested, what is not) and a comparison with DXVK d3d9 (Vulkan only, zlib
licence) and Wine d3d9 (code may be reused: the user lifted the licence gate).

## Done (committed)

- Phase 0 inventory, Phase 1 skeleton (`platform/`: Display, Window, Services, null and win32 backends, tests `platform_api`, `fast_facade`).
- Level A `platform/d3d9_device.hpp` (D3D9 semantics, handles, no guest pointers) and the **trace**: `platform/d3d9_trace.*` (writer, reader, headless
  `replay()` into any `d3d9::Device`), recorder inside the native bridge (`d3d9_recorder.*`, guest side: stays outside the future library).
- Shader path S1 (`platform/shader/`: D3D9 bytecode decoder, SPIR-V writer, SPIRV-Cross wrapper) proven on 27/27 pixel shaders and the vertex shaders
  with `dcl` (see the S1 paragraph in `PLATFORM_API.md` 3.4). `spike/` builds standalone (x64 clang, `build_spike`).
- Captures (valid, on the user's disk, **never in git**): `C:\Users\Leonardo\Projects\NFSU2 REINGENIER\captures\menu` and `...\loading`, 4 windows and
  24 reference BMPs each (1280x720). Check with `python tools/d3d9_trace_info.py <folder>`. `race` was not recorded (not needed, see below).

## What the captures really show (checked on the reference images)

- `menu`: **not a menu**, four windows of race gameplay (4th/4, lap 1/3), 72k-206k draws per 120-frame window (about 600-1700 draws per frame).
- `loading`: race gameplay (windows 0-2, one with an intro-style HUD text) and window 3 ends on the **"Need for Speed Underground 2 - Loading" screen**.
  So there is race coverage in both; a real front-end menu is the only scene missing (cheap to record later with the same command).
- Totals per capture (menu / loading): 388k / 519k indexed draws, 168k / 147k `draw_up`, 16M / 18.5M `set_*` calls, 0 unknown calls (so SetLight,
  SetMaterial, scissor, DrawIndexedPrimitiveUP are unused or unhooked: the recorder would have logged them).

## Half done (uncommitted work is now committed as WIP)

- `platform/gpu.hpp` rewritten for Level B: explicit `BlendState`, `DepthStencilState`, `RasterState`, `PixelFixedParams` (alpha test ref, fog, texture factor),
  `gpu::backbuffer` sentinel, `allocate_transient(bytes, stride, out)` (bgfx needs the stride to allocate), `create_texture` without initial data
  (levels are uploaded with `update_texture`), `create_static_buffer(bytes, index_buffer)`. The draw struct is called **`DrawDesc`** (not `DrawState`:
  `<windows.h>` defines `DrawState` as `DrawStateA` and breaks the link).
- `platform/null/null_platform.*`: `NullGpu` keeps a running `call_hash` of every call (all arguments, transient data included) and cost counters
  (draws, static/transient bytes, texture bytes, constants...). It is the determinism check: same trace twice => same hash.
- `platform/api_tests.cpp` adapted; `platform_api_tests` builds and passes (32-bit, `build32`).

## Not started: what I was about to do (2b)

1. **Move to a library** (`d3d9tr/` or similar, own `CMakeLists.txt`, include root = its parent): `types.hpp`, `d3d9_device.hpp`, `gpu.hpp`, `d3d9_trace.*`,
   `shader/*`, NullGpu/NullDevice (split them from `null_platform.*`, which also holds NullDisplay/NullWindow = platform side) and `Method` name table.
   Keep the recorder (`d3d9_recorder.*`) and the COM facade outside. Fix includes (`platform/...` used in `native32.*`, `d3d9_recorder.*`, tests, `spike/`).
2. **`d3d9::Layer : d3d9::Device`** on top of `gpu::Gpu` + a `ProgramFactory` (request = vertex/pixel bytecode, declaration, alpha-test function, fog mode,
   fixed-function key; answer = `gpu::Program` + float4 registers read, from `shader::Program::constant_registers_read`). Decisions already taken:
   - state shadow with defaults; one `DrawDesc` per draw; `clear` ends the pass and begins one with the clear flags and the same attachments;
     partial-rect clears and `stretch_rect` with scaling need a built-in quad program (`ProgramFactory::builtin`), plan for it;
   - buffers keep a CPU shadow. Usage DYNAMIC (or locked often) -> copy only the used ranges (min_vertex..+vertex_count, start_index..+count) into transient
     buffers and rebase indices (subtract min_vertex); otherwise a static GPU buffer re-created when the shadow changes (versioned). `base_vertex` and stream
     offsets fold into the start vertex of static buffers (negative or unaligned -> copy path). Triangle fans become lists through a transient index buffer;
   - textures: CPU shadow only for the lock rectangle (temporary staging, converted at unlock). Formats to convert when the GPU lacks them: X8R8G8B8 (alpha
     forced to 255), L8/A8L8 (expand to RGBA8), R8G8B8, BC1-3 -> RGBA8 on GPUs without BC (write a small decoder); take the real list from a census;
   - vertex colours (`D3DCOLOR`) arrive as B,G,R,A bytes: the vertex shader needs a `.zyxw` swizzle for those inputs (program key includes the declaration);
   - programs cached by (vs, ps, declaration, alpha function, fog mode, fixed-function key).
3. **Census first**: have the layer count draws by (vertex shader set?, pixel shader set?, primitive), distinct program variants, fixed-function draws and
   their stage states, rect clears, `stretch_rect` sizes, texture formats/usages, lock flags, `draw_up` bytes, render targets. It drives what to build.
4. **Replay tool** (`tools/d3d9_replay.cpp`): trace folder -> `Layer` -> `NullGpu`, print stats and `call_hash`, run twice to prove determinism, plus a
   ctest on a synthetic trace made with `TraceWriter`.

Then 2d (bgfx adapter + out-of-game spike) and 2e (replay on bgfx + comparison with the reference BMPs), as in `PLATFORM_API.md` section 6.

## Traps found (read before touching anything)

- **Replay gaps in `platform/d3d9_trace.cpp`**: `unlock_texture` records `[obj, level, face, pitch, blob]` but the replayer **does not write the blob** into the
  returned region (only buffers do). Textures would stay empty. Copy row by row (`min(recorded pitch, returned pitch)` bytes, `blob size / recorded pitch`
  rows; compressed formats use block rows). Also `lock_texture` replay ignores the region it gets.
- The trace records resources from run start, but **state and draws only inside windows** (a state snapshot opens each window; `Present` and frame markers
  only inside windows too). Replay must start each window from that snapshot; resources between windows carry on.
- Objects in a trace are keyed by the guest address of the COM object; addresses can be reused after `Release` (the replay forgets them on destroy).
- bgfx facts: resource updates and commands run before the frame's draws (frame-latent), so never rely on update-after-draw ordering; transient buffers
  need a stride (and a layout handle on `setVertexBuffer`); MAX_VIEWS 4096; attribute enum order is fixed (Position 0 ... TexCoord0..13 = 10..23).
- D3D9 semantics learned empirically in S1: stages link by register (fixed full varying interface), half-pixel fixup `u_fix = (+1/W, -1/H)`, unwritten
  `oD0` reads as white, ps 1.x constants clamp to [-1,1].
- `<windows.h>` macros bite headers shared with the platform code (`DrawState` -> `DrawStateA`; beware `near`, `far`, `min`, `max` too).
- Tool traps in this environment: heredocs and `sed` mangle backslashes and `\n` in Windows paths (use the Write/Edit tools or a script file);
  no PIL/numpy installed: `bmp2png.py`-style helpers must use `zlib` and `struct` only (one lives in the session scratchpad, not in git).
- `tools/package_launcher.py` fails inside this worktree (needs `../data`); run `build32\nfsu2_launcher.exe` directly with `--game-dir`.

## Build and run

```powershell
# 32-bit game side and all tests (once: cmake -S . -B build32 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-m32 -DGAME_DIR="C:/Users/Leonardo/Projects/NFSU2 REINGENIER/data game (pc)")
cmake --build build32 --target platform_api_tests fast_recorder_tests
cd build32; ctest -R "platform_api|fast_facade|fast_recorder"      # tests need a logs/ folder next to the working directory
# every test of the repo (19 passed at the last full run)
ctest
# shader spike, x64 (build_spike, not in git)
cmake -S spike -B build_spike -G Ninja -DCMAKE_CXX_COMPILER=clang++ && cmake --build build_spike
# summarise a capture
python tools/d3d9_trace_info.py "C:\Users\Leonardo\Projects\NFSU2 REINGENIER\captures\loading"
```

The game is never started from here: the user plays and sends captures; recording commands are in `PLATFORM_API.md` section 6 (2a).
