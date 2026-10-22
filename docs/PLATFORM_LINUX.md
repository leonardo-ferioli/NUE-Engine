# NUE on Linux

Owner: the Linux platform session. Code: `src/platform/linux/`. Targets: x86-64 and arm64 (desktop distributions, Steam Deck / SteamOS, Steam Runtime
`soldier`/`sniper`, Flatpak, Snap, AppImage; Raspberry-class arm64 boards later).

## Verification status (read this first)

The development machine is Windows 11 without WSL. Zig 0.17 (bundled clang + glibc 2.31 headers) was installed with the user's consent, so the Linux code is now **cross-compiled and linked** for x86-64 and arm64, but **nothing has been run on Linux yet** (no WSL, no Linux machine).

| What | State |
|---|---|
| Portable logic (`nue_linux_logic`): path resolver, XDG, `/proc` `/sys` parsers, session detection, GPU/renderer policy, audio/video/input policy, report | compiled with clang 23 for the Windows host and **run**: 54 unit tests / 537 checks, all pass |
| Platform ops table pure entries (`system_info`, `memory_info`, `system_locale`, `storage_root`, `power_info`, `pump` events, `nue.linux.v1` extension) | compiled for the Windows host and **run** against in-memory Steam Deck / Intel hybrid / arm64 / hybrid-GPU machines: 12 tests / 162 checks, all pass |
| `linux_probe` | **run** on the Windows host against generated machine fixtures (`make_fixture` + `--root`), real output checked by eye |
| Linux-only runtime (`nue_linux_runtime`: D-Bus, GameMode, power profiles, rtkit, scheduling, timers, storage, io_uring, crash handler) and the runtime entries of the ops table (`power_apply`, thread priority/affinity/core class, `sleep_until`, GPU preference, crash, syslog) | **compiled and linked** with `zig c++` (glibc 2.31 headers) for `x86_64-linux-gnu` and `aarch64-linux-gnu`, zero warnings under `-Wall -Wextra`; test and probe ELF executables produced; **never run**. Unit tests exist (`tests/test_runtime_linux.cpp`) and are built only on Linux |
| Live Vulkan / EGL / GL probe (`probe_vulkan`, hand-declared ABI structs with `static_assert`s) | compiled for both Linux targets (hand-declared Vulkan structs pass their `static_assert`s); **never run** |
| Window, input, audio on a real X11/Wayland/PipeWire session, gamescope, Steam Deck hardware, Flatpak, AppImage | **not possible here**, not tried |
| Integration into the `nue` CMake build with SDL3 | CMake fragment written to the contract in `PORTING.md`; **not built** (SDL3 submodule is empty in this tree) |

What is needed to turn "compiled" into "run": WSL2 with a distribution or a Linux machine. The cross build is reproducible with `cmake/toolchain-linux-zig.cmake` (see "Building and testing").

## Architecture

```
game / host application
        |
   nue C API  ------------------------------------------------------------------  core (src/core)
        |                                                                          VFS, events, mixer, config, log, diag policy
   +----+-------------------+
   | SDL3 backend (shared)  |   windows, input, audio devices, clocks       (src/platform/sdl3, core's)
   +------------------------+
   | Linux platform ops     |   system info, XDG roots, power / thermal / game mode, thread priority, crash, syslog
   |   platform_linux.cpp   |   + extension "nue.linux.v1" (resolve_path, session, renderer plan, report)
   +----+-------------------+
        |
   lx_* modules (this directory)
      portable   : lx_text lx_fs lx_pathcase lx_xdg lx_sysinfo lx_session lx_gpu lx_policy lx_diag lx_report lx_signal_safe lx_taskqueue
      Linux only : lx_dl lx_dbus lx_gamemode lx_sched lx_storage lx_crash lx_runtime (+ lx_gpu_probe live part)
```

Design rules:

* **Everything optional is `dlopen`ed** (`lx_dl.hpp`). The engine links only against libc, libdl, libpthread, libm. A missing library disables one feature and the reason is logged.
* **Decisions are data.** Policies return `HintSet` / `RendererPlan` / `AffinityPlan` structures (no side effects, no SDL types), so they are unit-tested without a window.
* **Detection reads through `IFileSystem` / `IEnv`.** The same code runs on `/` and on a captured machine tree (`linux_probe --root DIR`) or an in-memory fixture.
* **Honest capabilities.** Bits are set at `init` from what is really there; unsupported calls return `NUE_ERR_UNSUPPORTED` / `NUE_OK_PARTIAL` and `nue_power_applied` says field by field what was honoured.
* **Undo everything** at `shutdown`: GameMode registration, power-profile hold, idle inhibitors, timer slack, process affinity, GPU environment, syslog connection.

## Capability table

Legend: **I** implemented, **C** compiled (where), **R** run (where), **-** not possible/not done. "host" = Windows development machine, "linux" = needs a Linux machine.

| Area | Feature | I | C | R | Notes |
|---|---|---|---|---|---|
| Window | SDL3 X11 + Wayland, fractional scale, HiDPI, native handles | core | - | - | in the shared backend; Linux supplies the policy (`VideoPlan`), see REQUESTS_linux.md 2 |
| Window | fullscreen policy: gamescope / Deck / Wayland never switch modes, X11 may | I | host | host (unit) | `plan_video` |
| Window | multi-monitor, refresh rate, VRR, cursor lock | core | - | - | SDL3 |
| Audio | driver order PipeWire > PulseAudio > ALSA > dummy from the sockets that exist | I | host | host (unit) | `detect_audio`, `plan_audio` |
| Audio | low latency: `SDL_AUDIO_DEVICE_SAMPLE_FRAMES`, `PIPEWIRE_LATENCY`, `PULSE_LATENCY_MSEC` | I | host | host (unit) | default 512 frames at 48 kHz |
| Audio | null backend for tests | I | host | host (unit) | SDL `dummy` driver; the core also has its own null backend |
| Audio | default-device change | core | - | - | SDL3 events |
| Input | gamepad discovery from `/proc/bus/input/devices`, Steam virtual pad / Deck pad recognition | I | host | host (unit) | `read_input_devices` |
| Input | udev rule advice (steam-devices, uaccess), `/dev/uinput` | I | host | host (unit) | `check_input_setup` |
| Input | Steam Input awareness: no HIDAPI access behind Steam's virtual pad; direct Deck controls without Steam | I | host | host (unit) | `plan_input` |
| Input | haptics, touch, keyboard, mouse | core (SDL3) | - | - | |
| Power | battery (energy/charge families, time left, controller batteries ignored) | I | host | host (unit) | `read_power` |
| Power | thermal: thermal zones + hwmon, 4 states | I | host | host (unit) | thresholds 75 / 85 / 95 C |
| Power | events `POWER/THERMAL/LOW_POWER/GAME_MODE_CHANGED` from a 2 s poller thread | I | host | host (unit, fake FS) | pump never blocks on D-Bus |
| Game mode | Feral GameMode: `libgamemode.so.0`, D-Bus fallback | I | - | - | `GameModeClient` |
| Game mode | power-profiles-daemon hold performance / power-saver (both bus names) | I | - | - | `PowerProfileHold` |
| Game mode | idle / sleep inhibit: `org.freedesktop.ScreenSaver`, `org.freedesktop.PowerManagement.Inhibit` | I | - | - | SDL3 adds its own portal/X11/Wayland inhibition |
| Game mode | rtkit real-time / high priority (RLIMIT_RTTIME + RESET_ON_FORK prerequisites) | I | - | - | `RtKit` |
| Scheduling | nice, SCHED_BATCH/RR, ioprio, timer slack, affinity (by tid), hybrid core classes | I | - | - | `lx_sched` |
| Scheduling | CPU topology: Intel hybrid sysfs, arm capacity tiers, max-frequency gap fallback | I | host | host (unit) | `read_cpu_topology`, `plan_affinity` |
| Timers | `clock_nanosleep` absolute + spin tail, `timerfd` periodic timer | I | - | - | plan math host-tested (`plan_sleep`) |
| Storage | XDG data/config/cache/state/runtime, user-dirs, Flatpak/Snap via inherited variables | I | host | host (unit) | `resolve_xdg`, `storage_root` |
| Storage | case-insensitive Windows paths (`\`, drives, `..`, cached listings, wildcards) | I | host | host (unit, MemFs) | `PathResolver`; Linux file-system access path (`GameFiles`) is Linux-only and unrun |
| Storage | `mmap` (`FileMap`), atomic write, instance lock, free space | I | - | - | `lx_storage` |
| Storage | async reads: thread pool; io_uring (raw syscalls, opt-in, probed) | I | pool: host (unit) | - | io_uring only builds if `<linux/io_uring.h>` exists |
| Storage | mount table / file-system warnings (FAT, exFAT, NTFS, network, overlay) | I | host | host (unit) | `parse_mountinfo` |
| Graphics | DRM devices (vendor, device, driver, boot VGA, VRAM, render node) | I | host | host (unit) | `read_drm_devices` |
| Graphics | Vulkan ICD manifests incl. `VK_DRIVER_FILES` / `VK_ICD_FILENAMES`, driver naming (radv, anv, nvidia, lavapipe...) | I | host | host (unit) | `read_vulkan_icds` |
| Graphics | live Vulkan probe (devices, types, surface extensions) | I | - | - | `probe_vulkan` |
| Graphics | renderer plan: Vulkan > GL / GLES, discrete vs integrated, PRIME / Mesa device select env, software warning | I | host | host (unit) | `plan_renderer` |
| Graphics | GPU preference at runtime (`power_apply.gpu`, `window_set_gpu_preference`) | I | - | - | sets/unsets the env variables before device creation |
| Diagnostics | system info (OS, CPU, hybrid counts, features, memory with cgroup limit, GPUs, device class) | I | host | host (unit) | `system_info` |
| Diagnostics | crash handler: sigaltstack, registers (x86-64 / arm64), glibc backtrace, `/proc/self/maps`, sink protocol, re-raise for core dumps | I | - | - | `lx_crash`; Linux unit test forks a child and checks the report |
| Diagnostics | core dump configuration advice (`core_pattern`, RLIMIT_CORE) | I | host | host (unit) | `describe_core_dumps` |
| Diagnostics | minidump | - | - | - | Not provided; text stack report only (`NUE_CAP_MINIDUMP` is not set) |
| Diagnostics | log sink to syslog / journald | I | - | - | `p_log_write` |
| Tools | `linux_probe` console sample | I | host | host (fixtures) | `--live` and the runtime section need Linux |
| Packaging | notes for tarball, AppImage, Flatpak, Steam Runtime, SteamOS | doc | - | - | below; untested |

## Runtime dependencies (all optional, all `dlopen`ed except libc)

| Library (soname) | Used for | If missing |
|---|---|---|
| `libvulkan.so.1` | Vulkan renderer and live probe | falls back to GL / GLES; warning |
| `libEGL.so.1`, `libGLESv2.so.2`, `libGL.so.1`, `libOpenGL.so.0` | fallback detection | those fallbacks are not offered |
| `libdbus-1.so.3` | GameMode fallback, power profiles, idle inhibit, rtkit, dark mode | those services are skipped, logged |
| `libgamemode.so.0` | Feral GameMode | D-Bus path, else none |
| SDL3's own dependencies: `libpipewire-0.3.so.0`, `libpulse.so.0`, `libasound.so.2`, `libudev.so.1`, `libwayland-client.so.0`, `libdecor-0.so.0`, `libX11.so.6`, `libXi.so.6`, ... | window, audio, input | SDL picks another driver; `plan_audio` sets the order |
| kernel: `io_uring` | async reads | thread pool |

Build-time: a C++17 compiler, CMake 3.21, `<linux/io_uring.h>` (kernel headers, optional). No development package of any optional library is needed.

## Environment the layer reads or sets

Reads: `HOME`, `XDG_*`, `WAYLAND_DISPLAY`, `DISPLAY`, `SDL_VIDEO_DRIVER`, `SteamDeck`, `SteamGamepadUI`, `SteamAppId`/`SteamGameId`, `STEAM_COMPAT_CLIENT_INSTALL_PATH`,
`LD_PRELOAD` (Steam overlay), `FLATPAK_ID`, `SNAP`/`SNAP_NAME`, `APPIMAGE`, `GAMESCOPE_WAYLAND_DISPLAY`, `PIPEWIRE_REMOTE`, `PULSE_SERVER`, `VK_DRIVER_FILES`,
`VK_ICD_FILENAMES`, `LC_ALL`/`LC_MESSAGES`/`LANGUAGE`/`LANG`, `TMPDIR`.
Sets (never overwriting a value the user already exported): `SDL_AUDIO_DRIVER`, `SDL_AUDIO_DEVICE_*`, `SDL_VIDEO_DRIVER`, `SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS`,
`SDL_VIDEO_ALLOW_SCREENSAVER`, `SDL_JOYSTICK_*`, `PIPEWIRE_LATENCY`, `PULSE_LATENCY_MSEC`; on hybrid GPU machines `MESA_VK_DEVICE_SELECT`,
`MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE`, `DRI_PRIME`, `__NV_PRIME_RENDER_OFFLOAD`, `__VK_LAYER_NV_optimus`, `__GLX_VENDOR_LIBRARY_NAME`.

## Case-insensitive paths (critical for this game)

The game spells paths the Windows way (`C:\Games\NFSU2\GLOBAL\GlobalA.BUN`, `.\cars\peugeot_206\geometry.bin`) and the files on disk rarely match. `PathResolver`:

1. normalises `\` to `/`, removes `\\?\`, `.`, repeated separators; `..` cannot leave the mapped root; drive letters are dropped or mapped to a directory;
2. walks the components; **exact spelling first**, then the case-folded match (ASCII + Latin-1/Latin Extended-A/Greek/Cyrillic simple folding);
3. caches one listing per directory; a hit costs no system call. A miss re-stats the directory and rescans only if its mtime changed or is younger than 2 s;
4. if several names differ only by case, the byte-wise smallest wins and `ambiguous` is reported (never a random pick);
5. a missing last component with an existing parent reports `parent_found` so the caller can create the file with the given spelling.

Limits: a cached positive entry is not re-validated (an external delete is noticed when `open` fails, `GameFiles::open_read` then drops the cache and retries once); no 8.3 short names; no
NTFS-style trailing dot/space stripping; `?` in a wildcard matches one byte. On case-insensitive file systems (FAT, exFAT, NTFS, macOS-style ciopfs) exact matching already works and the resolver adds nothing.

## Game mode, power and scheduling

`nue_power_apply` maps the request: `HIGH/SUSTAINED` hold the `performance` profile and register with GameMode, `POWER_SAVER` holds `power-saver`, `MENU/PAUSED` leave GameMode,
`keep_awake` takes the ScreenSaver / PowerManagement inhibitors, `gpu` sets the PRIME environment for the next device creation, `cpu` pins the process to performance or efficiency cores
on hybrid CPUs, `timer_resolution_us` sets the timer slack of the calling thread to 1 us (restored when released). `target_frame_time_us`, `disable_power_throttling` and `prefer_low_latency_audio`
have no Linux meaning and are reported as not applied. D-Bus calls have a 1.2-1.5 s timeout and run on the caller's thread: call `nue_power_apply` from a worker thread.

Thread priority: `LOW` = `SCHED_BATCH` + best-effort I/O 6; `ABOVE_NORMAL` / `HIGH` = nice -3 / -8 when `RLIMIT_NICE` allows, else rtkit `MakeThreadHighPriority`; `REALTIME` = `SCHED_RR` 10
when `RLIMIT_RTPRIO` allows, else rtkit, else the HIGH boost (`NUE_OK_PARTIAL`). Setting `NORMAL` undoes only our own boost (Linux does not let an unprivileged thread lower its nice value again
below its original). A pthread handle without a tid cannot be mapped to a thread on Linux and gets `NUE_ERR_UNSUPPORTED`; pass the tid.

Hybrid CPUs: pinning is recommended only when the topology is really hybrid (Intel P/E via `/sys/devices/cpu_core|cpu_atom`, arm big.LITTLE via `cpu_capacity`, or a >25 % max-frequency gap). On uniform CPUs, including
the Steam Deck's 4-core/8-thread Zen 2, the kernel scheduler is left alone.

## Graphics policy

`plan_renderer` produces: the API (Vulkan, else OpenGL / GLES by what is installed and whether the machine is an SoC), the bgfx `vendorId/deviceId`, fallbacks, warnings
(software-only driver, Vulkan < 1.1, Wayland session without `VK_KHR_wayland_surface`) and the environment for hybrid machines. Choice order: explicit request, discrete GPU, integrated GPU, anything;
`prefer_low_power` flips discrete and integrated. Under X11 PRIME setups the non-boot GPU additionally gets `DRI_PRIME=1`; NVIDIA gets the render-offload variables. Steam Deck: a single AMD APU, nothing to select.

## Crash reports

`nue_crash_sink` protocol (see `REQUESTS_linux.md` 6): the core creates the report directory and `report.txt`, the Linux layer adds `stack.txt` (signal, `si_code`, fault address, all general registers,
glibc backtrace, `/proc/self/maps`). Core dumps still happen: the handler re-raises the signal with the default action unless the callback asked to terminate. The alternate stack is per thread:
`crash_handler_attach_thread()` must run in each engine thread (the installing thread is covered). `describe_core_dumps` explains `core_pattern`/`RLIMIT_CORE` (systemd-coredump: `coredumpctl`).
No minidump is written on Linux; a Breakpad/Crashpad hook point exists (`CrashOptions::hook` receives the report fd and the `ucontext`).

## Building and testing

Anywhere (portable logic and the ops table against fixtures; this is what has been run):

```
cmake -S nue_engine/src/platform/linux -B build-lx -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-lx
ctest --test-dir build-lx --output-on-failure        # nue_linux_tests, nue_linux_ops_tests: no window, no sound, no GPU
build-lx/make_fixture steam_deck fixtures/deck && build-lx/linux_probe --root fixtures/deck --env SteamDeck=1 --env HOME=/home/deck
```

On Linux (everything, including the runtime tests): `cmake --preset linux-x64 && cmake --build --preset linux-x64 && ctest --preset linux-x64`, or the standalone command above.
The tests set `SDL_VIDEODRIVER=dummy` / `SDL_AUDIODRIVER=dummy` (core's ctest) and the Linux tests never open a window, a sound device or the GPU. `linux_probe --live` opens libvulkan and enumerates devices only.

Cross-compiling from Windows (exercised: builds `nue_linux_tests`, `nue_linux_ops_tests`, `linux_probe`, `make_fixture` for both targets, and the integrated `nue` library with the core tests linked for x86-64 with SDL3 disabled; the build with SDL3 was not tried): `toolchain-linux-zig.cmake` (needs `zig` on `PATH`; `-DNUE_ZIG_TARGET=x86_64-linux-gnu.2.31` or `aarch64-linux-gnu.2.31`; the suffix pins glibc)
or `toolchain-linux-clang.cmake` with `-DNUE_LINUX_SYSROOT=...`. Both give "compiled", neither gives "run"; running needs WSL2 or a Linux machine.

## Packaging notes (untested)

* **Tarball** (primary): `bin/<game>` + `lib/` only for libraries we cannot expect on the host (SDL3 is linked statically). Build on the oldest supported glibc (Steam Runtime `soldier` = 2.31, `sniper` = 2.31+) so it runs everywhere newer. Ship no Vulkan/GL/audio libraries: they are `dlopen`ed from the host.
* **Steam Linux Runtime**: build inside the `sniper` SDK container; the layer detects `pressure-vessel` and `/etc/os-release ID=steamrt`. Do not set `LD_LIBRARY_PATH` for system libraries; the runtime already provides libvulkan, libdbus, PipeWire/Pulse client libraries.
* **SteamOS / Steam Deck**: the root file system is read-only and is replaced on updates: install under `$HOME` (`~/.local/share/<org>/<app>` is already where saves go). In Gaming Mode the game runs under gamescope: borderless fullscreen at the reported size, no cursor grab, controller through Steam Input (the layer leaves HIDAPI alone when the Steam virtual pad is present). Shader/pipeline caches go to `$XDG_CACHE_HOME` (survive updates).
* **AppImage**: use `linuxdeploy`; the `APPIMAGE` variable is detected by the layer; exclude libvulkan, libGL, libEGL, libdbus, libasound, libpipewire, libpulse from the bundle (host drivers must match the host GPU). Updates via `zsync`.
* **Flatpak**: runtime `org.freedesktop.Platform` (>= 23.08) with the Vulkan and GL extensions. `finish-args`: `--socket=wayland`, `--socket=fallback-x11`, `--share=ipc`, `--socket=pulseaudio` (also gives PipeWire clients), `--device=dri`, `--device=input` (gamepads; `--device=all` on older Flatpak), `--filesystem=<game data>:ro`, `--talk-name=org.freedesktop.ScreenSaver`, `--talk-name=org.freedesktop.PowerManagement`, `--talk-name=com.feralinteractive.GameMode` (or rely on the GameMode portal), `--system-talk-name=net.hadess.PowerProfiles`, `--system-talk-name=org.freedesktop.RealtimeKit1`. `XDG_*` are redirected by Flatpak to `~/.var/app/<id>`; the layer follows them without changes. `/.flatpak-info` is detected and the audio policy warns if neither a sound socket nor `/dev/snd` exists.
* **Snap**: plugs `opengl`, `audio-playback`, `joystick`, `x11`, `wayland`, `desktop`, `gamemode` (where available), `network-bind` not needed. The layer follows the `SNAP_*` / `HOME` redirection.

## SteamOS and Steam Deck (native)

Native here means: the game is a Linux build that runs on SteamOS (or a SteamOS-like system) with the same code path as on any distribution, plus a layer that
recognises the device and cooperates with gamescope, Steam Input and the OS services. Code: `lx_steamos.*` (portable logic, tested on the host against a captured Deck tree),
`lx_steamos_runtime.*` (logind, steamos-manager, process spawn, Steamworks `dlopen`; Linux only). Everything is reachable from C through `nue_ext_linux_v1`
(`include/nue_ext_linux.h`) and the standard hooks: `nue_system_info.platform_flags` / `platform_variant` / `runtime_name`, `nue_power_info.tdp_*` and `suspend_supported`,
`gpu_probe`, `memory_info_ex`, `screen_keyboard`, `pre_init`.

**State of the evidence.** No Steam Deck, Steam Machine or Steam Frame was available. Detection and policies are verified on the host against fixtures that reproduce the files a Deck exposes
(DMI, `os-release`, `/proc/self/mountinfo`, amdgpu sysfs, `/proc/bus/input/devices`, `libraryfolders.vdf`). Panel numbers, frame-limit steps and the steamos-manager property names are
**believed** from Valve's public information and are marked as such in the code; the first run on a real Deck must confirm them. The Linux-only services compile for x86-64 and arm64 and have never been run.

### Capability table (SteamOS)

| Area | Feature | Implemented | Compiled | Run | Notes |
|---|---|---|---|---|---|
| Detection | Steam Deck LCD / OLED from DMI (`Jupiter` / `Galileo`) | yes | host + Linux x64/arm64 | host (fixture) | `SteamDeck=1` alone gives "LCD, inferred" |
| Detection | Steam Machine, Steam Frame (arm64) | **inferred only** | yes | host (fixture) | Valve vendor + unknown product name + architecture; flagged `hardware_is_heuristic`. No real product ids known |
| Detection | SteamOS, Bazzite, ChimeraOS, HoloISO, SteamFork, Nobara (gaming session), Jovian-NixOS, Universal Blue images, generic gamescope-session images | yes | yes | host (fixture) | by `os-release` ID / ID_LIKE and session binaries; Jovian and Universal Blue are heuristics |
| Detection | Gaming Mode vs Desktop Mode | yes | yes | host (fixture) | `SteamGamepadUI=1`, `GAMESCOPE_WAYLAND_DISPLAY`, desktop environment name |
| Detection | Steam launch: `SteamGameId`/`SteamAppId`, Non-Steam shortcut, `STEAM_COMPAT_*`, Steam in Flatpak, Steam Runtime container | yes | yes | host (fixture) | shortcut = id above 32 bits or high bit set |
| Detection | immutable root (ostree / read-only `/`), SD cards from the mount table (`/run/media/...`) | yes | yes | host (fixture) | |
| gamescope | session recognition (env + desktop name) | yes | yes | host (fixture) | the X root-window atoms (`GAMESCOPE_*`) are **not** read |
| gamescope | fullscreen: borderless at the compositor's size, no mode switch, no cursor grab on a Deck | yes | yes | host (unit) | `plan_video` |
| gamescope | frame pacing plan: vsync present, own limiter only when needed, judder warning, suggested Steam frame limit | yes | yes | host (unit) | `plan_frame_pacing`; the real limiter is the core's `nue_frame_limiter_wait` |
| gamescope | request a refresh rate / frame limit from the compositor | **not possible** | - | - | gamescope has no application API for it (`window_request_refresh` is NULL, the core limits in software); the player picks the limit in the Steam quick access menu, the layer tells the game which one to suggest |
| gamescope | HDR, FSR / NIS scaling | reported only | yes | host (unit) | panel/session capability is reported (`hdr_available`, internal scaling allowed); the renderer is SDR |
| Display | Deck panel table (1280x800; LCD 40-60 Hz; OLED 45-90 Hz, VRR, HDR; frame-limit steps) | yes | yes | host (unit) | believed values |
| Controls | Steam Input detection (virtual pad), hidden physical pads (`SDL_GAMECONTROLLER_IGNORE_DEVICES[_EXCEPT]`), visible-pad count | yes | yes | host (unit) | |
| Controls | Deck pad through HIDAPI without Steam, gyro and back buttons flagged available through SDL | yes | yes | host (unit) | SDL itself reads them; this layer only decides and reports |
| Controls | glyph family: Steam for the Deck pad (core `nue_input_set_glyph_override(NUE_GLYPHS_STEAM)`), Xbox / PlayStation / Nintendo by vendor | yes | yes | host (stub core) | installed on the first pump and re-evaluated every 240 pumps |
| Controls | gyro, trackpads, back buttons, action sets **under Steam Input** | **documentation only** | - | - | they reach a game only through a Steam Input template; no template is shipped yet |
| Controls | on-screen keyboard (`steam://open/keyboard` via `steam -ifrunning`, Flatpak Steam, `xdg-open`) | yes | yes | no | `screen_keyboard` hook + `show_onscreen_keyboard` |
| System | suspend / resume from logind `PrepareForSleep` with a delay inhibitor lock; app state BACKGROUND / FOREGROUND | yes | yes (x64, arm64) | no | `NUE_CAP_LIFECYCLE_PAUSE` is set only when logind answers |
| System | battery / AC (`BAT1`), thermal, amdgpu busy / clocks / performance level / power cap | yes | yes | host (fixture) | |
| System | TDP limit and range (`tdp_watts_*`): steamos-manager properties, else the amdgpu power cap | read only | yes | host (fixture for the cap) | the steamos-manager interface names are discovered by introspection (names **unverified**) |
| System | setting TDP / GPU clocks / charge limit | **not implemented, by design** | - | - | those are the player's controls; `NUE_POWER_APPLIED_TDP` is never reported as applied |
| System | GameMode, power profiles, idle inhibition (also under gamescope) | yes | yes | no | as for any Linux |
| Files | read-only root policy: data below `$HOME` / Steam library, saves on internal storage, SD card library lookup (`libraryfolders.vdf`, `appmanifest_*.acf`) | yes | yes | host (fixture) | `find_steam_app` |
| Files | shader cache location: inside Steam's per-app cache when Steam provides `MESA_SHADER_CACHE_DIR`, else `$XDG_CACHE_HOME/<org>/<app>/shaders` | yes | yes | host (unit) | `shader_cache_dir` |
| Files | case-insensitive Windows paths | yes | yes | host (unit) | as on every Linux |
| Graphics | `gpu_probe`: Vulkan physical device features, texture limits and format support without a device; conservative table otherwise | yes | yes | table: host; Vulkan: no | anisotropy, render targets and MSAA are Vulkan minimums or common values, not read |
| Memory | `memory_info_ex`: RSS, cgroup limit, amdgpu GTT / VRAM, PSI pressure | yes | yes | host (fixture) | pressure thresholds (3 / 15 / 40 % stall) are a heuristic |
| Steamworks | optional `libsteam_api.so` through `dlopen` (running check, init, callbacks) | yes | yes | no | never linked; init needs a Steam launch or `steam_appid.txt` |
| Distribution | launcher script, `.desktop` file, packaging steps | templates | - | no | `src/platform/linux/dist/steamos/` |

### Frame pacing on the Deck (what the layer decides)

* Gamescope owns the real refresh rate. The game presents with FIFO / vsync and never tries to tear.
* Display equals the game's rate (60 on 60): vsync alone paces. Display is an exact multiple (30 on 60): the engine limiter holds the lower rate.
* Display is **not** a multiple (a 60 fps game on the OLED panel at 90 Hz, or a 45 Hz LCD mode): judder is expected and the layer returns the Steam frame limit to recommend (60). The game should show that hint once.
* Low battery or thermal pressure: `suggest_fps_cap` steps through the panel's limit list (60, 45, 40, 30), never below 30.

### Steam Input and controllers

Steam launches the game with a virtual Xbox-style pad and hides the physical ones through `SDL_GAMECONTROLLER_IGNORE_DEVICES`. The layer honours that list when it counts pads, never opens the physical Deck pad
while Steam Input is active, and only reads it directly (gyro, back buttons) when the game is started outside Steam. Non-Steam shortcuts get Valve's default "Gamepad" layout automatically; anything beyond it
(gyro aiming, trackpad menus) needs a Steam Input configuration shipped with the game, which does not exist yet.

### "Steam Deck Verified" checklist (what the engine provides and what the game must still do)

| Requirement | Engine side | Game side |
|---|---|---|
| Controller only, default layout works | Steam Input detection, glyph override, on-screen keyboard hook | menus navigable with the default "Gamepad" layout |
| Legible text at 1280x800 | panel and DPI reported (`panel_width/height`) | UI scale option; keep text readable at 800p (check Valve's current minimum size) |
| Default graphics settings suit the Deck | `platform_flags`, `suggest_fps_cap`, `gpu_probe` | pick 800p and a 30 / 40 / 45 / 60 cap preset when `NUE_PLATFORM_STEAM_DECK` is set |
| No launcher dialogs, no anti-cheat prompts | n/a | start straight into the game |
| Suspend / resume works | BACKGROUND / FOREGROUND events, audio and GPU quiet | pause on BACKGROUND, restore on FOREGROUND, recreate the swapchain on `SURFACE_LOST` |
| Saves survive OS updates and card removal | XDG roots below `$HOME` | do not write beside the executable |
| Runs without Proton | native build | launch through Steam Linux Runtime 3.0 (sniper) |

None of this has been checked by Valve's process; it is the engineering list, not a certification.

### Shipping a native game on SteamOS

1. Build on the oldest glibc you support (`-DNUE_ZIG_TARGET=x86_64-linux-gnu.2.31` or the sniper SDK container). Ship `bin/game`, the game data, `nue-launch.sh` (`dist/steamos/`). No system libraries.
2. Players add `nue-launch.sh` as a Non-Steam game and force Steam Linux Runtime 3.0 (sniper) in Compatibility; a Steam page would instead upload the depot with a Linux launch option.
3. Steamworks is optional: link nothing, ship `libsteam_api.so` beside the executable and let `SteamworksLite` find it.
4. Saves go to `~/.local/share/<org>/<app>`, settings to `~/.config`, caches to `~/.cache` (or Steam's shader-cache directory).

## Known gaps and next steps

1. Run the Linux-only code (needs WSL2 or a Linux machine): every "-" in the "R" column is an open risk. The CI Linux job of the new repository will run the host tests of this directory.
2. SDL3 integration points in the core: pre-init hook and window policy (REQUESTS_linux.md 1 and 2).
3. Dark-mode query via `org.freedesktop.portal.Settings` is planned (`system_info.dark_mode` stays 0).
4. `NUE_CAP_AUDIO_LOW_LATENCY` and the audio device list belong to the SDL3 backend; the layer only provides the driver/latency policy.
5. Wayland idle-inhibit protocol and portals are covered by SDL3 itself; ours is an addition for desktops where SDL's inhibition is ineffective (gamescope).
6. Raspberry-class boards: policies recognise SoC GPUs (no PCI ids) and prefer GLES when no Vulkan driver exists; nothing has been tried on such hardware.
