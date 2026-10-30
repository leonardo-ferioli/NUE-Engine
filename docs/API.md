# NUE engine - public API (C), version 0.1.1 (draft for the platform sessions)

> There are **two ABIs**: this document describes the *native* C API. The compatibility ABI for recompiled Windows-era games lives on top of it
> (`libnue_legacy`); see `ABI.md` for both, their stability promises and the package layout.

NUE is a general runtime + SDK for games that need everything from the operating system: window and display, input, audio,
files/storage, time and threads, power and *game mode*, logging and crash reports, app lifecycle and graphics (through the
Direct3D 9 -> bgfx layer `nue_d3d9`). The first game is the recompiled Need for Speed Underground 2; Most Wanted follows, so
nothing in the API is NFSU2 specific.

The API is **pure C99** (`include/nue/*.h`, prefix `nue_`, macros/enums `NUE_`) with a header-only C++17 wrapper on top
(`include/nuepp/*.hpp`, namespace `nuepp`). Targets: Windows (x86 and x64), Linux, Android, iOS (macOS later). The base is SDL3 plus
our own code where SDL3 is not enough (see `ARCHITECTURE.md`).

## 1. Conventions

| Topic | Rule |
|---|---|
| Linkage | `NUE_API` (export/import) + `NUE_CALL` (= `cdecl` everywhere, also 32-bit Windows). Static and shared builds expose the same functions. |
| Results | `nue_result` (int32). `>= 0` success (`NUE_OK`, `NUE_OK_PENDING`, `NUE_OK_PARTIAL`, `NUE_OK_NOOP`), `< 0` failure (`NUE_ERR_*`). `nue_result_name()` gives the identifier. After a failure `nue_get_last_error_message()` gives per-thread detail text. |
| No exceptions | Nothing throws across the ABI; callbacks must not throw or `longjmp` into the engine. |
| Struct versioning | Every struct passed across the ABI starts with `uint32_t struct_size`. Init with `NUE_STRUCT_INIT(&s)`. The engine reads/writes only what fits in `struct_size`; fields are only ever appended; reserved fields must be zero. Fixed-layout data without `struct_size` (events, vectors, guid, specs) never changes size. |
| Handles | Pointer handles are opaque (`nue_window`, `nue_mixer`, `nue_audio_stream`, `nue_thread`, ...), `NULL` = invalid. Files, voices, buffers, buses, I/O requests, imports and gamepads use 32-bit ids (index+generation, 0 = invalid) so a recompiled game can hand them out as its own `HANDLE` values. Stale ids fail with `NUE_ERR_INVALID_HANDLE`, never crash. |
| Strings | UTF-8. Text output uses `(char* buffer, size_t capacity, size_t* out_size)`: always NUL terminated when `capacity > 0`, truncated if needed, `out_size` = size needed including the terminator, `buffer == NULL` is a size query. |
| 32/64-bit | Same API. Layouts of public structs are identical on ILP32 and LP64 except where a struct holds a pointer; 64-bit quantities that carry OS handles (`nue_native_window`) are always `uint64_t`. |
| Thread tags | `[any]` thread safe; `[init]` between `nue_init` and `nue_shutdown`, not concurrent; `[event]` on the event thread; `[audio]` real-time safe. Each header states the rules next to the functions. |
| Allocation | `nue_alloc/nue_free`; hooks via `nue_init_desc.allocator`. Memory returned by the engine is freed with `nue_free` where documented (`nue_file_load`). |
| Single context | One engine instance per process, reference counted `nue_init`/`nue_shutdown`. |

## 2. Modules

| Header | Contents |
|---|---|
| `base.h` | platform/arch macros, version, result codes, handle & struct conventions, allocator, native window handle |
| `core.h` | init/shutdown, build info, log (levels, tags, sinks: stderr/file/platform/callback/ring), config store (INI, typed, watch), hashing (crc32/sha256), monotonic clock + precise sleep, threads/mutex/cond/semaphore/TLS, extensions |
| `app.h` | event model (64-byte `nue_event`), **host loop** (`nue_pump_events`/`nue_poll_event`) and **engine loop** (`nue_app_run`), lifecycle events, watchers |
| `window.h` | displays + modes, windows (windowed/borderless/exclusive, DPI, safe area, refresh rate, native handle, **attach to an existing HWND/native window**), cursor visibility/clip/relative mode |
| `input.h` | keyboard (HID scancodes), mouse, gamepad + raw joystick (mapping DB, rumble, LED, sensors), touch, device sensors, text input/IME, haptics, **last used device + glyph family** |
| `audio.h` | device list, output streams: **push** and **callback** models, int16/float32, 1-8 channels, latency query/control, position, flush/start/stop |
| `mixer.h` | software mixer: static / **ring (DirectSound-style, play+write cursors)** / callback voices, volume/pan/pitch, buses + effects, 3D listener and voices, deterministic manual rendering |
| `storage.h` | VFS: roots `data/user/cache/settings/temp`, mount overlays, case-insensitive paths, `\`->`/`, 32-bit file ids, map, list, glob, async read, manifest verification, **game data import** (SAF/Documents) |
| `power.h` | power/battery/thermal/low-power/**game mode** state, one `nue_power_request` (profile, hint, keep-awake, GPU/CPU preference, timer resolution, frame-time target), thread priority/affinity/core class, performance hint sessions (ADPF) |
| `system.h` | OS/CPU/GPU/memory info, **platform class flags** (handheld, SteamOS-like, Steam Deck/Machine/Frame, gamescope, Steam gaming mode, Steam Runtime, Flatpak, read-only root, compatibility layer) and ~50 capability flags |
| `memory.h` | memory information, pressure events, **configurable budgets** (policies resolved against the machine, config-key overrides, eviction hooks); no hard-coded limits anywhere |
| `gfx.h` | graphics context on a window (bgfx), renderer selection per platform, resize/suspend handling, listeners, **seam to the Direct3D 9 layer** |
| `diag.h` | crash handler framework + reports/minidumps, notes, breadcrumbs, asserts, counters, frame statistics (`--fps-log` style), Chrome-trace zones |
| `vm.h` | **virtual address space service** (0.1.4): reserve (aligned, hinted) / commit / protect / decommit / release, 32-bit generation-checked region ids; the base of the 4 GiB guest arena of the legacy layers (`GUEST_MEMORY.md`) |
| `entry.h` | portable `nue_main` entry contract (`nue::entry` target) |
| `null_backend.h` | test control of the headless backend (inject input/lifecycle/power, advance virtual time, read played audio) |

## 3. Life of a program

### Host-owned loop (the recompiled game)

```c
nue_init_desc d; NUE_STRUCT_INIT(&d);
d.api_version = NUE_API_VERSION; d.app_name = "NFS Underground 2"; d.app_id = "nfsu2"; d.org_name = "NUE";
d.loop_model = NUE_LOOP_HOST; d.game_data_path = "C:/Games/NFSU2";
if (NUE_FAILED(nue_init(&d))) { /* nue_get_last_error_message() */ }

nue_window_desc wd; NUE_STRUCT_INIT(&wd);
nue_native_window nw; NUE_STRUCT_INIT(&nw); nw.kind = NUE_NATIVE_WIN32_HWND; nw.window = (uintptr_t)hwnd;
wd.attach = &nw;                                  /* the game made its own window; NUE observes it */
nue_window w; nue_window_create(&wd, &w);

for (;;) {                                         /* the game's own loop */
    nue_pump_events();                             /* on the event thread */
    nue_event e;
    while (nue_poll_event(&e)) { if (e.type == NUE_EVENT_QUIT) goto done; /* ...handle... */ }
    /* game frame; presenting goes through nue_gfx / the Direct3D 9 layer on the game's render thread */
    nue_frame_mark();
}
done: nue_shutdown();
```

### Engine-owned loop (mobile, or desktop apps that prefer it)

```c
static nue_app_result on_frame(void* u) { /* update + render one frame */ return NUE_APP_CONTINUE; }
int NUE_CALL nue_main(int argc, char** argv) {      /* link nue::entry */
    nue_init_desc d; NUE_STRUCT_INIT(&d); /* ... */
    nue_app_callbacks cb; NUE_STRUCT_INIT(&cb); cb.on_frame = on_frame; /* on_init, on_event, on_quit */
    return nue_app_run(&d, &cb, argc, argv);
}
```

### Lifecycle (mobile and desktop)

`APP_PAUSING` (watchers get it synchronously; stop audio/GPU work) -> `APP_PAUSED` -> `APP_RESUMING` -> `APP_RESUMED`;
`SURFACE_LOST`/`SURFACE_CREATED` for the native surface (Android); `APP_LOW_MEMORY`; `APP_TERMINATING`. `nue_gfx` and audio streams react by
themselves (suspend swapchain, pause streams unless `NUE_AUDIO_KEEP_RUNNING_IN_BACKGROUND`).

## 3b. Cooperating with the platform (0.1.1)

| Need | API |
|---|---|
| memory limits are data, not constants | `nue_memory_get_info`, `nue_budget_*` (`memory.h`), config keys `budget.<name>.*` |
| memory pressure, device suspend, audio interruption | events `NUE_EVENT_MEMORY_PRESSURE`, `NUE_EVENT_SYSTEM_SUSPENDING/RESUMED`, `NUE_EVENT_AUDIO_INTERRUPTED/RESUMED/ROUTE_CHANGED` |
| compositors that own the refresh rate (gamescope, Steam Deck, Android, ProMotion) | `nue_window_request_refresh_rate`, `nue_window_set_frame_limit`, `nue_frame_limiter_wait`, `nue_display_info.min/max_refresh_hz` |
| handhelds | `nue_power_info.tdp_watts_*`, `nue_power_request.tdp_watts`, `nue_text_input_show_screen_keyboard`, `NUE_GLYPHS_STEAM`, `nue_input_set_glyph_override` |
| GPU format support before a device exists (texture planning) | `nue_gpu_probe`, `nue_gpu_caps` (format families with sample/filter/render-target flags) |
| timers (`timeSetEvent`-style) | `nue_timer_*` |

## 3c. C++ wrapper

`include/nuepp/*.hpp` (namespace `nuepp`, header only, C++17, no exceptions thrown or required): `Result`/`Expected<T>`, `Context` (init/shutdown RAII), `Mutex`/`LockGuard`/`Semaphore`/`Thread`/`Timer`,
`config::get/set`, `Window` (including `Window::attach` for a host-owned native window), `Gfx`, `events::poll/drain`, `input::*`, `AudioStream`, `Mixer`, `File`, `vfs::*`, `Budget`, `TraceZone`.
Handles are move-only and released by the destructor. The C API stays the ABI; the wrapper adds no state of its own.

## 4. Threading model in one table

| Group | Rule |
|---|---|
| `nue_init/shutdown` | caller thread becomes the event thread (`nue_set_event_thread` rebinds) |
| `nue_pump_events`, window mutators, cursor | event thread only |
| `nue_poll_event`, `nue_push_event`, queries (`nue_window_get_*`, input polling, config, log, power, system) | any thread |
| audio stream/mixer control | any thread; callbacks run on the audio thread (`[audio]`: no allocation, no blocking, no logging) |
| VFS and files | any thread; one file handle is not used concurrently except `nue_file_read_at` |
| `nue_gfx` create/destroy/resize | event thread; `nue_gfx_present` and bgfx calls on the render thread |
| callbacks (log sinks, io, import progress, config watch, gfx listeners) | run on the producing thread; documented per type |

## 5. Platform behaviour contract

The portable API never promises a feature the OS lacks: check `nue_has_capability()` (`system.h`) or accept `NUE_ERR_UNSUPPORTED` / `NUE_OK_NOOP`.
Platform-only features that do not belong in the portable API are published as **named extensions** (`nue_get_extension("nue.windows.v1")`),
whose headers live in `src/platform/<os>/include` and install under `nue/ext/`.

## 6. Direct3D 9 layer seam (summary; details in `GFX_D3D9_SEAM.md`)

`nue_d3d9` (branch `claude/d3d9-bgfx-d42a87`) translates Direct3D 9 to bgfx. It sits **above** `nue_gfx`: `nue_gfx_create` selects the renderer and (MANAGED
mode) calls `bgfx::init`, or (`NUE_GFX_PREPARE_ONLY`) hands `nue_gfx_get_native()` values to the translator, which keeps calling `bgfx::init` itself as today.
`nue_gfx_add_listener` tells it about resize / suspend / resume so it can implement `Reset` / `TestCooperativeLevel`.

## 7. Stability

* Everything in these headers is **API 0.1 (draft)**; the platform sessions may implement against it now. Changes go through
  `API_CHANGES.md`; breaking changes bump the minor version while < 1.0, and announce to all platform sessions.
* Platform sessions request additions in `REQUESTS_<platform>.md`; the core session answers there.
