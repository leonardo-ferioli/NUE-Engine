# Porting guide: writing a platform layer or a backend

There are two extension points, both internal C interfaces (not ABI):

| Interface | Header | Who writes it | What it is |
|---|---|---|---|
| Backend | `src/core/internal/backend.h` | core (null, sdl3) | raw devices: displays/windows, input devices, audio devices, OS event pump |
| Platform ops | `src/core/internal/platform_ops.h` | platform sessions (`src/platform/<os>`) | what SDL3 does not give: power/game mode, storage rules, system info, thread priorities, crash handlers, import sources, log sink |

The SDL3 backend covers window/input/audio/time on all platforms. The platform layer **adds** to it; it does not replace it. If a
platform ever needs a different backend for a subsystem (a native WASAPI exclusive-mode device, say), request it in
`REQUESTS_<platform>.md`: the backend table is per subsystem (video/input/audio) so a platform can supply its own sub-table.

## What a platform session delivers

1. `src/platform/<os>/CMakeLists.txt` (included by the root file when building for that OS):

   ```cmake
   target_sources(nue_platform_obj PRIVATE platform_windows.cpp power_windows.cpp ...)   # relative paths work
   target_link_libraries(nue_platform_obj PUBLIC dbghelp ...)                             # OS libs / frameworks
   ```
   `nue_platform_obj` already links `nue_internal` (include paths for `nue/*.h` and `internal/*.h`).

2. `const nue_platform_ops* nue_platform_<os>_get_ops(void)` returning a **static** table. Leave unimplemented entries `NULL`.
   Set `capabilities[]` bits for what you really provide (`nue_capability` indices; the test `test_capabilities` checks that every set bit has the entries it needs).
3. Optional OS-only extensions: `nue_internal_register_extension("nue.<os>.v1", &table)` in `init`, header in `src/platform/<os>/include/nue_ext_<os>.h`.
4. Rows in `docs/STATUS.md` for your OS (send the text to the core session or ask to edit).
5. Your own tests under `src/platform/<os>/tests/` (console only). Register them from your CMakeLists with `add_executable` + `add_test`, linking `nue::nue`.

## Initialisation order (what a platform layer can rely on)

`nue_init` runs: log -> `pre_init` (platform; set environment/SDL hints here, `storage_root` must already work) -> storage roots (`vfs_init`) ->
log file sink -> settings file load (`config_get_*` already sees the saved values) -> backend `init` -> platform `init` -> event queue -> memory/power
-> diagnostics (crash handler). Capability bits are read lazily on every query, so a layer may still set `capabilities[]` inside `init`.

Table fields appended after 0.1.0 (`pre_init`, `owns_lifecycle`, `memory_info_ex`, `gpu_probe`, `window_request_refresh`, `screen_keyboard`) are guarded by
`struct_size`; always set `struct_size = sizeof(nue_platform_ops)` of the header you compiled against.
`owns_lifecycle = NUE_TRUE` (Android Activity, iOS app delegate): you push `NUE_EVENT_APP_*`; backends will not derive app state from window events.
Events that must reach watchers synchronously (`SURFACE_LOST`, `APP_PAUSING`) do: `nue_internal_push_event` runs watchers on the calling thread before it returns,
and `nue_gfx_set_auto_events` makes `nue_gfx` suspend its swapchain inside that call. Fill the real `window_id` on surface events.

## Link dependencies and other modules

Libraries or frameworks a fragment links `PUBLIC` to `nue_platform_obj` (or `nue_texture_obj`) are forwarded by the root CMake to the final `nue` library
(this is how iOS frameworks reach the app). On Apple the root enables OBJC/OBJCXX when the toolchain has them.
`src/texture/CMakeLists.txt` (nue-texture) uses `target_sources(nue_texture_obj ...)`; `src/legacy/CMakeLists.txt` defines the target `nue_legacy` (see `ABI.md`).

## Rules

* Do not edit files outside your folder. Need a core change? `docs/REQUESTS_<os>.md` or a message; the core answers quickly.
* Never change system-wide state without undoing it: timer resolution, power requests, cursor clip, display modes, process priority.
  `shutdown` and `crash_release_input` must undo everything (the desktop must never stay captured after a crash).
* Honest capability reporting. A function that cannot do the job returns `NUE_ERR_UNSUPPORTED` or `NUE_OK_NOOP`; it never pretends.
* Thread rules are in `platform_ops.h`. `pump` runs on the event thread each `nue_pump_events`; push events with `nue_internal_push_event`.
* Crash code runs in a broken process: no heap allocation, no locks, no stdio (`crash_install` doc in `platform_ops.h`).
* 32-bit Windows is a first-class target (the launcher is x86): no assumptions about `size_t` being 64-bit; use `uint64_t` for handle-ish values.

## Capability table template (copy into STATUS.md)

| Capability | Provided by | Implemented | Tested how | Notes |
|---|---|---|---|---|
| `NUE_CAP_GAME_MODE` | `power_apply` | no | - | - |

## Backend authors (null / sdl3)

* `init/shutdown/pump` on the event thread; push events with `nue_internal_push_event` (it updates the polled-state tables, last-used-device logic and watchers).
* Window snapshot: keep `nue_window_snapshot` current; call `nue_internal_window_changed` after changes made outside `pump`.
* Audio: `open` returns a device whose `callback` you call from your device thread; `close` must not return while the callback runs.
* The null backend is the reference: if the null backend and SDL3 backend disagree with `docs/API.md`, the doc wins and the backend is the bug.

## SDL3

Pinned shallow submodule at `third_party/SDL` (release tag recorded in `STATUS.md`). Third-party code is untrusted: review statically, never run scripts from it.
Built as a static library inside `nue`. Tests use `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` (set by ctest).
