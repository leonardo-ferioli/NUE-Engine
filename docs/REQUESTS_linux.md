# Requests from the Linux platform session to the core session

## Status after API 0.1.1 (read first)

Resolved by core 0.1.1 and adopted by the Linux layer: `pre_init` (runs `nue_linux_apply_early_policy`; request 1), `screen_keyboard`, `memory_info_ex`, `gpu_probe`,
`nue_platform_flags` / `platform_variant` / `runtime_name`, `tdp_watts_*` and `suspend_supported` in `nue_power_info`, `NUE_GLYPHS_STEAM` and `nue_input_set_glyph_override`,
`nue_window_request_refresh_rate` / `nue_window_set_frame_limit`.

Still open, in order of value:

* **A. Window policy consumption (request 2 below).** Unchanged: the SDL3 backend must not switch display modes under gamescope / Wayland / Steam Deck, and `NUE_CAP_EXCLUSIVE_FULLSCREEN` /
  `NUE_CAP_DISPLAY_MODE_CHANGE` should be false there. The Linux layer exposes the decision through `nue_ext_linux_v1` (`plan_frame_pacing`, `get_steamos_info`, `get_session`).
* **B. `window_request_refresh` stays NULL on Linux.** gamescope offers no application API to ask for a refresh rate or frame limit (the player sets it in the Steam quick access menu). Please keep
  the software limiter as the only mechanism and expose `nue_linux_frame_pacing.suggested_steam_limit` to the game through your docs, not as an error.
* **C. Lifecycle from the platform layer.** The Linux layer calls `nue_internal_set_app_state(BACKGROUND / FOREGROUND)` from a watcher thread when logind announces suspend / resume. Please confirm that call
  is allowed from a non-event thread (it only pushes events and flips the state). It does **not** set `owns_lifecycle`: window focus events still drive INACTIVE.
* **D. Session-mode change event.** Gaming Mode vs Desktop Mode is fixed for the life of a process, but a docking / HDMI change can switch the Steam Deck between handheld and TV use. If you want a
  `NUE_EVENT_*` for "output/handheld mode changed", say so; today the layer reports nothing because Linux has no reliable signal for it.
* **E. Steam Input templates.** Gyro, trackpads and back buttons reach a game only through a Steam Input configuration. If the SDK should carry a template generator (`.vdf` controller configs per game),
  that belongs in `tools/` (core) with the action list from `input.h`; the Linux layer can only document it.
* **F. TDP.** `nue_power_request.tdp_watts` is accepted and reported as NOT applied (`NUE_POWER_APPLIED_TDP` clear): changing TDP is the player's decision on SteamOS. Tell me if the policy should be different.

The original requests follow; items 1 and 3 are closed or superseded.

## 1. Platform hook that runs BEFORE the backend initialises (needed for correct audio/video driver choice)

`nue_platform_ops::init` runs *after* the backend is up, but the decisions that matter on Linux must be made before `SDL_Init`:
the SDL audio driver order (PipeWire > PulseAudio > ALSA, or `dummy`), the video driver (Wayland vs X11 vs `offscreen`), the low-latency
buffer request (`PIPEWIRE_LATENCY`), the GPU selection variables on hybrid machines and the joystick hints.

Proposal: a new optional entry in `nue_platform_ops`, called once at the start of `nue_init`, before the backend:

```c
/* Static strings the backend applies with SDL_SetHint before SDL_Init, plus process environment to set (never overwriting user values). */
nue_result (*pre_init)(const nue_init_desc* desc,
                       void (*set_hint)(void* userdata, const char* name, const char* value),
                       void (*set_env)(void* userdata, const char* name, const char* value),
                       void* userdata);
```

Until it exists the Linux layer exports `nue_linux_apply_early_policy(flags)` (header `nue_ext_linux.h`): it sets the same variables
(`setenv(..., overwrite = 0)`, SDL3 reads hint values from the environment of the same name) and a host that calls it before `nue_init`
gets the right behaviour. With `pre_init` the core calls it for every application, which is the goal.

## 2. SDL3 backend: consume the window policy

The Linux policy decides, from the session, how fullscreen must behave (`VideoPlan` in `lx_policy.hpp`):

| Situation | Decision |
|---|---|
| gamescope / Steam Deck Gaming Mode | borderless desktop fullscreen at the size gamescope reports; never change the display mode; do not grab the cursor on a Deck |
| Wayland | no display-mode change is possible; "exclusive" requests degrade to borderless desktop fullscreen; relative mouse uses pointer constraints |
| X11 (plain desktop) | exclusive-like mode switching allowed |
| any | `SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0` so alt-tab does not minimise a fullscreen game |

The SDL3 backend lives in `src/platform/sdl3` (core), so it cannot call Linux code directly. Suggestion: make `NUE_CAP_EXCLUSIVE_FULLSCREEN` /
`NUE_CAP_DISPLAY_MODE_CHANGE` be decided by the backend from `SDL_GetCurrentVideoDriver()` + a platform op
`bool (*window_mode_change_allowed)(void)` which the Linux layer answers from `VideoPlan::allow_video_mode_change`
(false under gamescope/Wayland/Deck). I can also expose it through the `nue.linux.v1` extension if you prefer not to add an op.

## 3. Time domain of `nue_time_ns` / `sleep_until`

`sleep_until(deadline_ns)` receives a deadline in the `nue_time_ns()` domain. The Linux layer converts it through "now"
(`remaining = deadline - nue_time_ns()`, then `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`), so it is correct whatever the epoch is.
If you define `nue_time_ns()` as `CLOCK_MONOTONIC` exactly (what `SDL_GetTicksNS`/`steady_clock` give on Linux), say so in `API.md` and I can drop the
conversion.

## 4. Case-insensitive resolution: optional shared implementation

`storage.h` specifies case-insensitive lookup in the core VFS. The Linux layer has a tested implementation (`PathResolver`, header-only API
in `lx_pathcase.hpp`, exposed to C as `nue_ext_linux_v1::resolve_path`) with these properties that you may want in the core version:

* per-directory index built once; **hits cost no system call**; a miss re-stats the directory and rescans only if its mtime changed or is younger than 2 s
  (coarse-granularity file systems);
* exact spelling always wins; when several names differ only by case the byte-wise smallest is chosen and reported as `ambiguous`;
* `..` can never climb above the mapped root; `\\?\` and drive prefixes are understood; `C:` can be mapped to a directory;
* simple Unicode folding (ASCII, Latin-1 supplement, Latin Extended-A, Greek, Cyrillic) applied to both sides;
* `note_created` / `note_removed` keep the index exact for the process's own changes.

Options: (a) the core keeps its own and ignores this; (b) the core calls `resolve_path` through the extension when running on Linux; (c) I
move the class to a header the core can include (`src/core` would then own it). I recommend (c) if you have not written yours yet: tell me and I
will send a patch for `src/core`, I do not edit it myself.

## 5. Capability test expectations

`test_capabilities` checks that every set capability bit has the entries it needs. The Linux layer sets bits at `init` from what it detects, so the
table is mutated after `nue_platform_linux_get_ops()` returned. If the test reads the bits *before* `init`, it will see zeros; please read them after.
Bits set by the Linux layer: `BATTERY`, `THERMAL_STATE` (when sensors exist), `TIMER_RESOLUTION`, `THREAD_PRIORITY`, `THREAD_AFFINITY`,
`CRASH_HANDLER`, `CORE_CLASS` (hybrid CPUs only), `LOW_POWER_MODE` (power-profiles-daemon running), `GAME_MODE` (GameMode or power-profiles-daemon),
`KEEP_AWAKE`, `GPU_PREFERENCE` (more than one GPU). Not set: `MINIDUMP` (Linux writes a text stack report plus `/proc/self/maps`, no minidump),
`PERF_HINT`, `HAPTICS` (SDL3 covers gamepad rumble).

## 6. Crash sink on Linux

`nue_crash_sink::begin` is called in signal context with the report directory created by the core; the Linux layer then writes `stack.txt` (signal, fault address,
registers, glibc backtrace, `/proc/self/maps`) into it and calls `end`. If `end` returns `NUE_TRUE` the process `_exit(128+sig)`s, otherwise the signal is re-raised with the
default action so `systemd-coredump` / apport still receive the core. Please confirm that `begin` is allowed to use `snprintf`-free formatting only (as documented) and that
the directory it returns is in static storage.

## 7. Presets

`CMakePresets.json` is yours. Suggested additions for Linux (the toolchain files exist in `src/platform/linux/cmake/`):

```json
{ "name": "linux-arm64", "inherits": "base", "displayName": "Linux arm64 (cross: clang+sysroot or zig)",
  "cacheVariables": { "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/src/platform/linux/cmake/toolchain-linux-clang.cmake",
                      "NUE_LINUX_TRIPLE": "aarch64-linux-gnu" } },
{ "name": "linux-x64-zig", "inherits": "base", "displayName": "Linux x86-64 cross-compiled with zig",
  "cacheVariables": { "CMAKE_TOOLCHAIN_FILE": "${sourceDir}/src/platform/linux/cmake/toolchain-linux-zig.cmake",
                      "NUE_ZIG_TARGET": "x86_64-linux-gnu.2.31", "NUE_BUILD_TESTS": "OFF", "NUE_BUILD_SAMPLES": "OFF" } }
```

(`linux-x64` already exists and is native-only: its `condition` is `hostSystemName == Linux`.)
