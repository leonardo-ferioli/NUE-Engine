# NUE Windows backend

Owner: the nue-windows session. Code: `src/platform/windows/` (library `nue_platform_windows`, namespace `nue::win`, headers in
`include/nue_win/`). Builds for x86 and x64 with clang and MSVC; every test suite below passes in all four configurations.

The backend has **no dependency on SDL3 or on the core API headers**. It is the "own code beside SDL3" part for Windows: things SDL3 does not
give a recompiled game that owns its HWND. The thin glue that exposes it through the core `nue_` C API is the next block (see "Not done").

## How to build and test

```
pwsh src/platform/windows/tests/build_and_test.ps1                              # clang x64 + clang x86
pwsh src/platform/windows/tests/build_and_test.ps1 -Configs msvc-x64,msvc-x86   # MSVC (Visual Studio Build Tools via vswhere)
```

Tests are console programs. They show no window (hidden or message-only windows only), play no sound, create no GPU device, never confine the
real pointer (clip goes through a test hook) and never start the game. The script imports the Visual Studio environment of the target
architecture for clang as well: `clang -m32` cannot find the x86 import libraries otherwise.

Opt-in tests (environment variables, all verified once on the development machine, see table):

| Variable | What it does |
|---|---|
| `NUE_TEST_WASAPI=1` | opens the default render device in shared mode, session volume 0 **before** Start, checks cadence and statistics |
| `NUE_TEST_WASAPI_LOOPBACK=1` | renders a 997 Hz tone at -90 dBFS (below the 16-bit LSB, inaudible) and finds it again through WASAPI loopback capture |
| `NUE_TEST_WASAPI_EXCLUSIVE=1` | takes the default device exclusively for 0.3 s with silent data (interrupts other apps' audio for that moment) |
| `NUE_TEST_GPU_PROBES=1` | creates D3D11/D3D12 devices per adapter (can wake a sleeping discrete GPU) |

`samples/windows_probe` prints what the machine supports (console only, `--probe-gpu` for feature-level probes).

## Capability table

Legend: **Tested** = executed by an automated test or the probe on the development machine (Surface laptop, Windows 11 build 28000, Ryzen 7
with integrated Radeon, one internal 2496x1664 display, Realtek audio, no gamepad, no touch input during tests). **Written** = implemented and compiled
in all four configurations, but its effect could not be observed here (needs other hardware or would disturb the desktop). **No** = not possible
or deliberately not done, with the reason.

### 1. Game Mode and gaming integration

| Item | State | Notes |
|---|---|---|
| Detect Game Mode active | Tested (reads) | `EFFECTIVE_POWER_MODE` = GameMode via `PowerRegisterForEffectivePowerModeNotifications`. It was *off* during tests (no game running), so the "on" value was never seen. |
| Switch Game Mode on/off | **No** | Windows decides (foreground full-screen game + user setting). The app cannot set it. What it can do is be a good candidate: foreground, borderless covering the monitor or exclusive, flip-model swap chain. |
| User Game Mode / Game Bar / capture settings | Tested (reads) | HKCU `GameBar`, `GameDVR`, policy `AllowGameDVR`. On this machine most values are absent (Windows default applies); reported as `unknown`. |
| Windows' own opinion of our window | Tested (reads) | `SHQueryUserNotificationState`, `QUNS_RUNNING_D3D_FULL_SCREEN` once a D3D full-screen window exists (not exercised). |
| Game Bar process present | Tested | process snapshot. The WinRT `Windows.Gaming.UI.GameBar` API (input redirection events) is **not** used. |
| Exclude window from capture | Tested | `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` set/read back on a hidden window. Whether Game Bar's recorder honours it was not checked. |
| Fullscreen optimizations (read / write own opt-out) | Tested against a test registry key | `AppCompatFlags\Layers` flag `DISABLEDXMAXIMIZEDWINDOWEDMODE`. The real key is only read. |
| Borderless vs exclusive full screen | Tested (window side) | planning is pure and unit-tested; applied to a hidden window (style, rectangle, topmost). The display-mode change itself (`set_display_mode`, `CDS_FULLSCREEN`) is **Written**, not run: it would flicker the user's screen. |
| Tearing / variable refresh present | Tested | `DXGI_FEATURE_PRESENT_ALLOW_TEARING` = yes here. Whether a monitor really does VRR **cannot be queried** by a normal application; tearing support is only a proxy. |
| HDR / advanced colour query, SDR white level | Tested | DisplayConfig, per monitor; this panel is SDR (`no`), the HDR "yes" path was not seen. Enabling HDR output on a swap chain belongs to the renderer. |
| Exact refresh rate | Tested | rational from DisplayConfig (60/1), plus `EnumDisplaySettings`. |
| Windows 11 "optimizations for windowed games" / windowed VRR | Tested (reads) | `DirectXUserGlobalSettings` parsed (`unknown`/`no` here). Never written. |

### 2. Power and scheduling

| Item | State | Notes |
|---|---|---|
| EcoQoS opt-out (process, thread) | Tested | `SetProcessInformation(ProcessPowerThrottling)`, read back as "forced off". Reset also tested. |
| Windows 11 timer-resolution rule | Tested | `PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION` set in the same call (control bit read back); `timeBeginPeriod(1)` measured to 1.000 ms. That a **minimised** window keeps the 1 ms was not measured (needs a visible window). |
| Process priority class / thread priority policy | Tested | `REALTIME` is not offered; thread priorities are clamped to `ABOVE_NORMAL` (project policy from PLATFORM.md; TIME_CRITICAL caused the unresponsive-desktop incident). |
| MMCSS ("Games", "Pro Audio") | Tested | scope object; audio threads use "Pro Audio" then "Audio". |
| Hybrid CPUs (P/E cores) | Tested on a non-hybrid CPU + synthetic hybrid data | `GetSystemCpuSetInformation`, efficiency classes, `SetThreadSelectedCpuSets`, process default sets, ideal processor, affinity. The real "E-core" path could not be observed (this CPU is not hybrid). |
| High-resolution sleep | Tested | `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` + spin tail: 1 ms sleeps average 1.09 ms, never early. |
| Keep display awake | Tested (API succeeds) | `PowerCreateRequest/PowerSetRequest`, fallback `SetThreadExecutionState`. That Windows really keeps the screen on was not waited out. |
| Battery / AC / energy saver / power mode | Tested (reads) + notifications registered | `GetSystemPowerStatus`, `PowerRegisterSuspendResumeNotification`, `RegisterPowerSettingNotification`, effective-power-mode callbacks (all six sources registered). Real events (unplugging, suspend) were not provoked; delivery path tested by injection. |
| Thermal | Partly | CPU frequency vs limit (`CallNtPowerInformation`) and the ACPI "Thermal Zone Information" counters (9 zones here, one is a placeholder at -273 C). No public per-app thermal-pressure API exists on desktop Windows. |

### 3. GPU

| Item | State | Notes |
|---|---|---|
| Discrete GPU request exports | Tested | `NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance` linked into the executable (`nue_win_request_discrete_gpu(target)`), found with `GetProcAddress` on the exe. Effect on a hybrid laptop **not observed** (this machine has one GPU). |
| Per-app GPU preference (Settings > Graphics) | Tested against a test key | `UserGpuPreferences`, exact value format `GpuPreference=2;`. Takes effect at next process start. |
| Adapter enumeration | Tested | name, vendor/device ids, VRAM, shared memory, UMD driver version, outputs, memory budget, high-performance and power-saving pick. Integrated/discrete is a **heuristic** (vendor, name, VRAM) unless `--probe-gpu` (D3D11 UMA flag); the heuristic is unit-tested on a table of real names, not verified on other machines. |
| Device-loss classification and recovery state machine | Tested (unit) | D3D9 (`DEVICELOST`, `NOTRESET`, `HUNG`, `REMOVED`) and DXGI codes; wait / reset / recreate / give up with back-off. **Not tested against a real driver reset**: the code is a decision engine the renderer must drive. |
| Adapter change detection (driver reinstall, GPU added/removed) | Tested (registration, injection) | `IDXGIFactory7::RegisterAdaptersChangedEvent`, `IsCurrent()` fallback. A real change was not provoked. |
| HAGS, TDR settings | Tested (reads) | HAGS on here. |

### 4. Window and display

| Item | State | Notes |
|---|---|---|
| Attach to an existing HWND | Tested | `SetWindowSubclass`; never replaces the game's window procedure and swallows only `WM_SETCURSOR` (when the cursor is hidden) and `WM_CLOSE` (when the close callback refuses). One attachment per window; must run on the window's thread. Tested with synthetic messages on a hidden window, including destruction while attached. Not yet tried inside the real game. |
| Per-monitor DPI v2 | Tested | process-wide call with fall-back; the manifest is the proper way for an exe (the launcher should adopt it). `WM_DPICHANGED` forwarded. |
| Windowed / borderless / exclusive planning and switching | Tested on hidden windows | styles remembered and restored exactly; DPI-aware frame; multi-monitor with negative coordinates (synthetic). |
| Work-area fit with bars | Tested | `fit_aspect` (same integer arithmetic as nue_d3d9's `window_fit.hpp`, copied, no cross-dependency) and "window scaled to the screen" client sizing. |
| Cursor hide / clip / relative | Tested without confining the pointer | hiding via `WM_SETCURSOR` (no global counter, nothing to leak after a crash); clip released on focus loss, minimise, suspend, session lock; re-applied on return; relative mode = hidden + clip + Raw Input deltas. `emergency_restore_desktop()` (clip, pointer counter, display modes) is also called by the crash handler, a response to the earlier invisible-pointer incident. |
| Multi-monitor, display-mode list | Tested | one monitor here; two-monitor layouts only synthetic. `CDS_TEST` of the current mode succeeds. |
| Suspend / resume, session lock | Tested by synthetic messages and injection | window messages and window-less power callbacks. A real sleep/resume cycle was not done. |

### 5. Input

| Item | State | Notes |
|---|---|---|
| Raw Input (mouse deltas, keyboard, HID list) | Tested (decoder with synthetic packets, registration, device list) | no physical input was injected. |
| XInput (state, dead zones, rumble, battery, Guide button) | Tested without a pad | dead-zone maths tested; polling of absent slots throttled. **Rumble/battery never exercised** (no controller). Trigger motors (Xbox One/Series) are not reachable through XInput. |
| GameInput | Probe only | `GameInput.dll` loads and `GameInputCreate` succeeds here. No wrapper yet; SDL3 remains the gamepad path. |
| Last-used device (keyboard+mouse / gamepad) | Tested (unit) | hysteresis and cooldown. |

### 6. Audio

| Item | State | Notes |
|---|---|---|
| WASAPI shared, event driven | Tested live, silent | callback cadence matches wall time (0.98 s -> 47616 frames at 48 kHz), 0 glitches, callback 0.03 ms average. |
| Low-latency shared (`IAudioClient3`) | Tested | used on this device, but its minimum engine period equals the default (10.67 ms), so there is no gain here. Devices with smaller minima were not available. |
| Exclusive mode | Tested live, silent data | 24-bit/48 kHz, 5.33 ms period (device minimum 3 ms; alignment retry path ran). |
| Buffer/period negotiation, format fallbacks | Tested live for the paths above | candidate chain for exclusive (float, 32/24/16-bit, device default format). Other chains by reading only. |
| Format conversion (sample type, channels, rate) | Tested (unit, Null stream) | linear resampler without anti-aliasing filter (fine for 44.1->48 kHz, will alias on large down-conversions), ITU downmix. One real bug found and fixed by the tests (downsampling consumed one frame too many). |
| Default-device change, hot-plug | **Written** | `IMMNotificationClient` + restart loop; never exercised (no second device). |
| Null stream (no device, same pipeline) | Tested | tone through three format combinations, level and frequency verified, paced in real time. |
| Loopback verification | Tested | tone found through WASAPI loopback. Note: on this machine the loopback tap is **after the endpoint volume** (level 0.4x at endpoint volume 0.12); the test asserts presence and frequency, not absolute level. |

### 7. Storage

| Item | State | Notes |
|---|---|---|
| Known folders (Saved Games, AppData, Documents, ProgramData, Temp...) | Tested | redirected (OneDrive) folders reported; per-app directory layout, with a portable `root_override`. |
| Long paths | Tested | create/write/read/delete at > 400 characters through `\\?\`. The process-level opt-in (manifest) is reported (`no` for the test executables). |
| Case semantics | Tested | NTFS case-insensitive access verified; per-directory case sensitivity (WSL) read via `FileCaseSensitiveInformation`. A **case-sensitive** directory was not created, so `resolve_case_insensitive` was tested only on a normal directory. |
| Atomic save (temp + `MoveFileEx` replace/write-through) | Tested | including a failure case that keeps the old file and removes the temp. A power cut cannot be tested. |
| Cloud placeholders, free space | Tested (negative case, disk space) | |

### 8. Diagnostics

| Item | State | Notes |
|---|---|---|
| Minidump on crash | Tested for real | child processes crash with access violation, divide by zero, a custom exception and a **stack overflow**; each leaves a valid dump (exception stream, modules, threads, handle data, thread info) and a text report (registers, module+offset, memory, last first-chance exceptions, module list). Heavy work runs on a fresh thread and static storage so the stack-overflow case works. Rotation tested. Chains the previous filter. |
| On-demand dump (hang report path) | Tested | |
| First-chance exception log | Tested | passive vectored handler, lock-free ring, ignores C++ exceptions by default. Registered last so the game's own handlers run first. |
| ETW | Tested (registration, no-op when unlistened) | TraceLogging provider `NUE.Engine` `{c8e1a4b2-3d57-4f6a-9b0e-71d2f5a8c3e4}`. A real trace session consuming the events was **not** run. |
| Windows performance counters (PerfLib provider of our own) | **No (not done)** | ETW is the supported route; a PerfLib v2 provider would be extra work with little use. |
| CPU timing | Tested | process/thread times, thread cycle counter (frequency independent). |
| GPU timing | Partly | per-process GPU engine utilisation and memory through the "GPU Engine" PDH counters (available here, ~ms per call: sample at 1 Hz at most). Per-frame GPU time needs D3D timestamp queries, which belong to the renderer. |
| Frame statistics (avg, p50/p95/p99, 1% low) | Tested | |

## Findings worth knowing (outside this backend too)

- **clang 23 and `__try`/`__except`:** a hardware fault (not a call) inside `__try` is **not caught** unless `-fasync-exceptions` is used; on x86 that option
  corrupted callee-saved registers in the caller in our test (vector returned through a garbage `sret` pointer). The backend therefore never relies on
  `__try`; the tests use `RaiseException` and exercise real faults in child processes. The game's guest SEH/VEH code in the launcher is unaffected (it uses vectored handlers).
- **`clang -m32` needs the Visual Studio environment** (LIB/INCLUDE of the x86 toolchain), otherwise lld cannot find `kernel32.lib` etc. The first x86 run of this
  backend was silently an x64 build because of a script bug; the x86 results above are from the corrected script.
- Windows 11: `Sleep(10)` lasts about 15.6 ms at the default timer resolution; use `PreciseSleeper`.

## Not done / next block

1. Glue to the core API (`include/nue/*.h` is committed by nue-core): implement the platform interface for Windows on top of these modules, and decide with
   nue-core how the SDL3 backend and this one share the window (the engine will either create the window through SDL3 or attach to the game's HWND).
2. Real-hardware checks that need other machines: hybrid-graphics laptop (discrete GPU request), hybrid CPU (P/E cores), gamepad (rumble, battery), second
   monitor, HDR display, Game Mode "on" state, device hot-plug and driver reset.
3. GameInput wrapper (trigger rumble), WinRT Game Bar events, a real ETW capture test, measuring that a minimised window keeps its 1 ms timer.
4. The launcher manifest (per-monitor DPI v2, long paths) and adopting the backend in `launcher.cpp`/`native32.cpp` are out of scope for now by instruction.
