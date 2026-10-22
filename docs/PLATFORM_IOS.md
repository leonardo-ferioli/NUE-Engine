# NUE on iOS (iPhone and iPad)

Backend notes for the iOS platform of the NUE engine: what exists, how it is organised, the policies it implements and, above all,
**what has and has not been verified**. macOS 13+ shares most of this code (UIKit pieces would be replaced by AppKit ones later).
Step-by-step build instructions in Spanish: [PLATFORM_IOS.es.md](PLATFORM_IOS.es.md).

## 1. Verification status (read this first)

The author works on Windows. There is no macOS, Xcode or Apple SDK on that machine, so **no Apple code in this backend has ever been compiled
or run**.

| Piece | Where | Status |
|---|---|---|
| Pure logic (lifecycle state machine, thermal/memory/frame-rate policy, Metal capability flags and the DXT texture policy, path/import/validation logic, CRC32, audio-session policy, gamepad conditioning, gyro steering math, last-used-device tracker) | `src/platform/ios/logic/` | **Compiled with clang on Windows and unit-tested** (`ctest`, 235 checks) |
| `Info.plist` required keys | `ios/sample/Info.plist`, `ios/tests/plist_check.cpp` | Text-level check passes on the host; whether iOS accepts the plist is **not** verified |
| Objective-C++ glue (UIKit, AVFoundation, GameController, CoreHaptics, CoreMotion, Metal, MetricKit, os_log) | `src/platform/ios/apple/*.mm`, `nue_ios.h` | **Written, never compiled.** Expect compile errors on the first Mac/CI run (API spellings, nullability, ARC details) |
| Sample app (SDL3 window + CAMetalLayer clear + status overlay) | `ios/sample/main.mm` | Written, never compiled, never run |
| CMake iOS build, presets, toolchain file | `ios/CMakeLists.txt`, `ios/cmake/`, `ios/CMakePresets.json` | Host configure of the test part verified; the iOS branch **never configured** |
| XcodeGen project | `ios/project.yml` | Never generated |
| Scripts (build, xcframework, IPA) | `ios/scripts/*.sh` | Never run |
| GitHub Actions workflow | `.github/workflows/nue-ios.yml` | Never run; its first run is the first compile of the glue |
| Behaviour on a device (Game Mode effect, ProMotion pacing, audio interruptions, gyro signs in landscape, controller mapping, memory numbers) | | **Unverified**; every such claim below is a design intention |

The glue is kept thin on purpose: decisions live in `logic/` (tested), the `.mm` files translate between UIKit/AV/GC notifications and
those functions, so most of the glue can be checked by reading.

## 2. Layout

```
nue_engine/src/platform/ios/
  nue_ios.h            C surface between the core's iOS adapter and the glue (events, paths, storage, power, GPU info, input, audio, diagnostics)
  logic/               pure C++17, no Apple headers (host-testable)
  apple/*.mm           Objective-C++ glue, one file per concern: core (init, lifecycle, power), storage, audio, input, display (Metal/safe area), diag
nue_engine/ios/
  CMakeLists.txt       host tests everywhere; glue + SDL3 + sample app when CMAKE_SYSTEM_NAME is iOS
  CMakePresets.json    host-tests, ios-device, ios-simulator
  project.yml          XcodeGen alternative (SDL3 through Swift Package Manager)
  sample/              Info.plist, entitlements, main.mm
  scripts/             build_ios.sh, package_xcframework.sh, make_ipa.sh, run_host_tests.sh
  tests/               ios_logic_tests.cpp, plist_check.cpp
```

`nue_ios.h` is **internal** to the backend. The public API is owned by nue-core (`nue_engine/include/nue`); the core's iOS platform adapter
calls `nue_ios_*` and converts `NueIosEvent` into public `nue_` events. What the backend needs from the public API is listed in
[REQUESTS_ios.md](REQUESTS_ios.md).

### SDL3 and our own code

SDL3 is the base: `UIApplicationMain`, the `UIWindow`, the Metal view (`SDL_Metal_CreateView`), touch (`SDL_EVENT_FINGER_*`), keyboard,
mouse and the audio stream (`SDL_AudioStream` over CoreAudio) all come from SDL3. The backend does not replace SDL's app delegate. It
adds what SDL3 does not provide, through `NSNotificationCenter` observers and direct framework calls:

| Concern | SDL3 | Own code |
|---|---|---|
| Window, Metal view, touch, keyboard, mouse | yes | `nue_ios_configure_metal_layer` (pixel format, drawable count, scale) |
| Lifecycle | `SDL_EVENT_WILL_ENTER_BACKGROUND` etc. | state machine with explicit actions (stop GPU, save, drop caches), memory pressure, thermal |
| Audio | stream/device | `AVAudioSession` category, interruptions, route changes, buffer duration |
| Gamepad | `SDL_Gamepad` (GameController or HIDAPI) | GameController snapshot with our conditioning, CoreHaptics rumble, product family |
| Gyro | sensor API | CoreMotion gravity -> wheel angle in the active interface orientation |
| Storage | none | sandbox layout, import, validation |
| Diagnostics | log callback | os_log, signposts, signal crash handler, MetricKit |

The core may choose to take gamepads from SDL instead of `nue_ios_pad_*`; both are provided so the choice stays open. SDL hints the sample
sets before creating the window: `SDL_HINT_ORIENTATIONS`, `SDL_HINT_IOS_HIDE_HOME_INDICATOR` (the supported way to defer the home indicator).

UIScene: SDL3 is used without scene manifest, i.e. the classic app-delegate lifecycle. Newer iOS releases log that UIScene adoption will be
required in the future. The backend observes `UIApplication` notifications, which are posted in both models, so adopting scenes later should not
change it; this is unverified.

## 3. Build and packaging

* **Targets**: arm64 only (device and Apple-silicon simulator), iOS 16+, `MinimumOSVersion 16.0`, `UIRequiredDeviceCapabilities: arm64, metal`.
  Static libraries only (iOS apps link everything statically). No bitcode. MoltenVK is not used: bgfx talks to Metal directly.
* **Two ways to build** (same sources): CMake presets with the Xcode generator, or XcodeGen. Both produce the sample `.app`; the glue also
  packages as `NueIos.xcframework` (`scripts/package_xcframework.sh`).
* **SDL3** pinned to `release-3.4.18` (same pin in `nue_ios_app.cmake` and `project.yml`; keep it in sync with nue-core). Override with
  `-DNUE_SDL3_SOURCE_DIR=<path>` to use the core's checkout.
* **Signing**:
  * Simulator and CI: no signing.
  * Device, free Apple ID (Xcode "Personal Team"): works for development; the profile expires after 7 days and a free account can have a
    handful of sideloaded apps at a time. Capabilities needing a paid profile must stay out of the entitlements (the sample's file is empty).
  * Device, paid Developer Program: one-year profiles, ad-hoc distribution to registered devices, TestFlight.
  * Sideloading tools (AltStore, Sideloadly): `scripts/make_ipa.sh` wraps the built `.app` in an unsigned IPA; the tool re-signs it with the
    player's own Apple ID. Same 7-day rule for free accounts.
  * App Store / TestFlight: a recompilation of a commercial game that needs the player's own game files is not something to expect to pass
    review or to be distributable; the realistic path is development signing and sideloading. This is a constraint to keep in mind, not a
    verified review outcome.
* **JIT / generated code is impossible** on iOS for third-party apps without the debugger-only dynamic-codesigning exceptions: the engine must
  never allocate memory it later makes executable. Everything the game executes must be ahead-of-time translated code or run by an interpreter.
  This affects the P-code interpreter used for the audio modules (`dynamic_code.hpp`): its interpretation loop is fine, its optional per-block
  native code generation must be compiled out on iOS (core/translated-code owners).
* **Open architectural risk (not mine to solve, but iOS-specific)**: the translated code relies on a 32-bit guest address layout with real
  mappings at fixed low addresses. An iOS arm64 process has no memory below 4 GiB by default (`__PAGEZERO`). Reducing it needs a linker option
  (`-Wl,-pagezero_size,...`) whose behaviour on iOS has not been checked, otherwise the guest memory model must go through an indirection.
  Not attempted here.

## 4. Lifecycle (`logic/lifecycle.*`, `apple/nue_ios_core.mm`)

`LifecycleState`: not_running, inactive, active, background. Inputs are UIApplication notifications (launch, become active, resign active,
enter background, enter foreground, terminate, memory warning). Each transition returns **actions**, which the glue performs and forwards to
the engine in one `NUE_IOS_EVENT_LIFECYCLE` event (`a` = new state, `b` = action bits):

| Transition | Actions |
|---|---|
| inactive -> active | resume game, audio, GPU submission |
| active -> inactive (call, Control Center, app switcher) | pause game |
| -> background | pause game and audio, **stop GPU submission**, save state, release caches, flush logs |
| background -> inactive (foreground) | refresh drawable size and safe area |
| memory warning (any running state) | release caches, emit low-memory |
| terminate | pause, stop GPU, save, flush logs (not guaranteed to be delivered: save when backgrounded) |

* **Never commit Metal work in the background**: iOS may terminate an app that does. The action bit is the contract; the sample honours it.
* **Suspend/resume without losing the GPU**: the `CAMetalLayer`, `MTLDevice` and resources survive a background trip as long as no work is
  submitted while away; nothing is destroyed on the background transition. Only a drawable-size refresh is needed on return (rotation or
  Stage Manager resize may have happened). Verified by design only.
* **Memory**: three sources feed `NUE_IOS_EVENT_MEMORY_PRESSURE`: `UIApplicationDidReceiveMemoryWarning` (also a lifecycle action), a
  `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE` source, and an optional 1 s poll of `os_proc_available_memory()` classified by `classify_memory`
  (moderate < 400 MB, high < 250 MB, critical < 120 MB remaining). `os_proc_available_memory()` returns 0 on the simulator: treat as unknown.
* **Background audio**: `UIBackgroundModes` is deliberately absent (checked by `plist_check`). The game is suspended in the background, the
  audio session is deactivated with `NotifyOthersOnDeactivation` so other apps resume at once, and re-activated on return. The engine must
  have stopped its audio stream by the time it receives the lifecycle event (`setActive:NO` fails while I/O runs).
* **Safe area / notch / Dynamic Island / home indicator**: `NUE_IOS_EVENT_SAFE_AREA` carries `UIWindow.safeAreaInsets` in points; the home
  indicator is deferred through the SDL hint. `nue_ios_refresh_display()` must be called from SDL's resize / safe-area / display events.
* **Orientation and iPad multitasking**: the iPhone plist is landscape only; iPad allows all four and `UIRequiresFullScreen` is false, so
  Split View, Slide Over and Stage Manager windows are possible. The engine must treat window size as variable (core: window-fit logic) and
  re-read `NUE_IOS_EVENT_DISPLAY` (size in points, scale, orientation, max Hz) after every change.
* **ProMotion**: `nue_ios_set_frame_rate` keeps an otherwise idle `CADisplayLink` with `preferredFrameRateRange` so the display runs at the
  requested rate. For a fixed 60 fps target the range is exactly 60/60/60 so the display does not drift to 48 or 80 Hz. Rates above 60 Hz on
  iPhone need `CADisableMinimumFrameDurationOnPhone` (set in the plist). Whether an idle link really pins the rate for a Metal layer on every
  OS version is **unverified**; if not, frame pacing must come from the present path.

## 5. Game Mode and performance (`apple/nue_ios_core.mm`, `logic/power.*`)

* **Game Mode** (iOS 18+, macOS 14+): the app declares `GCSupportsGameMode = true` and `LSApplicationCategoryType = public.app-category.racing-games`
  (any games category works). This makes the app *eligible*; the system decides at run time (per Apple developer forum answers, not
  formal documentation). While active it prioritises CPU/GPU and lowers Bluetooth latency. **There is no public API to ask whether Game Mode is on**,
  so the engine cooperates passively: full-screen, steady frame rate, no needless background work. `nue_ios_game_mode_info()` only reports what the
  app declares and whether the OS is 18+. On macOS 26 Apple moved to `LSSupportsGameMode`; add it when the macOS target appears.
* **Thermal state**: `NSProcessInfoThermalStateDidChange` -> `NUE_IOS_EVENT_THERMAL` with advice from `advise_quality`: nominal/fair unchanged;
  serious = 30 fps, 85 % resolution scale, reduced effects; critical = 30 fps, 70 %, warn the player. Advice only ever lowers quality as
  conditions worsen (tested). The engine decides whether to follow it (fidelity-first: it is a recommendation, not a forced downgrade).
* **Low Power Mode**: same notification family; caps at 30 fps and reduces effects.
* **Battery**: level and state in `NUE_IOS_EVENT_POWER`.
* **Keep awake**: `nue_ios_set_keep_awake` -> `idleTimerDisabled` (the engine should enable it during play and release it in menus).
* **Thread QoS**: `nue_ios_set_thread_qos(0..3)` = user-interactive (game/render thread), user-initiated, utility (streaming/IO), background.
* **Memory budget**: `plan_memory_budget` plans against the 2 GB goal, capped to 55 % of physical RAM and to what `os_proc_available_memory`
  reports; soft limit 85 % of that, texture share 25 %. A 4 GB device plans for the full 2 GB goal (55 % of 4 GB is 2.2 GB), but the real jetsam limit for a normal app is
  lower on many models: the increased-memory-limit entitlement (see `NueSample.entitlements`) may be needed and needs a paid profile. The figures
  are heuristics until measured on hardware.

## 6. Storage (`logic/storage.*`, `apple/nue_ios_storage.mm`)

| Data | Location | Backup |
|---|---|---|
| Game files (about 1.2 GB) | `Documents/NFSU2/` | excluded (`NSURLIsExcludedFromBackupKey`), the player can recover them |
| Saves, settings | `Application Support/NUE/saves/` | **included** (small) |
| Shader cache, transcoded textures | `Caches/NUE/shaders/` | excluded; may be purged by the OS and must never be required |
| Logs, crash reports | `Caches/NUE/logs/` | excluded |
| Import staging | `tmp/NUE/` | n/a |

* **Delivery of game data**: `UIFileSharingEnabled` + `LSSupportsOpeningDocumentsInPlace` make `Documents` visible in Finder (device > Files tab)
  and in the Files app, where the player copies the NFSU2 folder. Case: the game was written for Windows (case-insensitive); the iOS file system
  may be case-sensitive, so lookups go through `CaseFoldIndex` (tested; collisions are reported, the lexicographically first wins).
* **Validation**: `nue_ios_validate_game_data("SPEED2.EXE:1000000,...")` returns missing/truncated files (a partial copy shows as truncated).
  A folder copied one level too deep is detected by `find_game_root`.
* **Import** (`nue_ios_pick_folder` + `nue_ios_import_folder`): `UIDocumentPickerViewController` for a folder, security-scoped access kept while
  copying, free-space check against `NSURLVolumeAvailableCapacityForImportantUsageKey` with a 5 %/64 MiB margin, 1 MiB chunked copy with progress
  events, CRC32 of the source stream compared with a re-read of the destination, unsafe paths (`..`, absolute, drive letters) rejected. Not done:
  resume of an interrupted import, iCloud-only files (would need `NSFileCoordinator`, the read fails and is reported). Run it on a background thread.
* Free space is checked before an import; a bookmark persisted across launches is not implemented (the pick must be repeated).

## 7. Audio (`logic/audio_policy.*`, `apple/nue_ios_audio.mm`)

* Policies: exclusive (Playback, the default, ignores the silent switch), respect-silent (SoloAmbient), mix-with-others (Playback + MixWithOthers),
  duck-others (Playback + MixWithOthers + DuckOthers). The category is set in `nue_ios_init`; `nue_ios_audio_activate(1)` activates and applies buffer preferences.
* Interruptions (calls, Siri, alarms): began -> pause event; ended with `ShouldResume` and the app active -> resume event and re-activation.
  Media-services-reset re-applies the session and tells the engine to recreate its stream (modelled as interruption began+ended).
* Route changes: headphones unplugged (`OldDeviceUnavailable`) asks the engine to pause; other changes only re-apply preferences.
* Latency: preferred IO buffer 5 ms wired/USB, 8 ms speaker, 20 ms Bluetooth (advisory: the OS decides), 40 ms AirPlay; at least 10 ms in Low Power
  Mode. Preferred sample rate 48 kHz except on Bluetooth/AirPlay (codec decides). These numbers are starting points, not measurements.
* The audio stream itself is SDL3's. The game's DirectSound ring buffer is the core/audio owner's concern.

## 8. Input (`logic/input_logic.*`, `apple/nue_ios_input.mm`)

* **Touch**: SDL3 delivers it; `touch_phase_from_raw` and `normalize_point` map `UITouchPhase` and points to scale-independent coordinates if the core wants them.
* **Controllers**: GameController framework (MFi, Xbox, DualShock/DualSense, Switch Pro). `nue_ios_pad_state` returns Xbox-layout buttons by position,
  radial deadzone 0.12 with rescale, trigger deadzone 0.04 and a digital threshold of 0.5. `GCSupportsControllerUserInteraction` lets the system
  show its controller UI. Rumble through `GCController.haptics` + CoreHaptics (one engine per pad, restarted by its reset handler).
* **Keyboard and mouse** (iPad): only used as "last used device" signals; SDL3 delivers events. `UIApplicationSupportsIndirectInputEvents` is set.
* **Gyro steering**: CoreMotion gravity -> wheel angle, deadzone 4 degrees, full lock 35 degrees, flat-on-table detection, `nue_ios_motion_recalibrate`
  stores the neutral angle. The two landscape bases are derived from the geometry and **their signs must be checked on a device** (note that
  `UIInterfaceOrientation` landscapeLeft/Right are swapped relative to `UIDeviceOrientation`; the code uses interface orientation 3 = landscapeRight,
  4 = landscapeLeft). Motion pauses with the game so the player cannot steer while suspended.
* **Last used device**: `NUE_IOS_EVENT_INPUT_DEVICE` (touch, keyboard, mouse, gamepad, motion). Stick noise and mouse jitter do not count; motion
  only takes over after 2 s without any other device, because a held phone is always moving.

## 9. Graphics support for bgfx (`logic/caps.*`, `apple/nue_ios_display.mm`)

* bgfx uses the Metal renderer. Hand it `SDL_Metal_GetLayer(SDL_Metal_CreateView(window))` as native window handle, after
  `nue_ios_configure_metal_layer` (BGRA8, 3 drawables, vsync, opaque, `contentsScale = nativeScale`, `framebufferOnly` configurable: off if the
  layer is blitted or read back). Resize: call it again and re-read the drawable size.
* `nue_ios_probe_gpu` reports the Apple GPU family (highest `MTLGPUFamilyAppleN`), `supportsBCTextureCompression`, unified memory, simulator flag,
  working-set size and derived caps in the order of `nue::gpu::GpuCaps::texture_format` (`TexFormat` mirrors `nue::gpu::Format`; a test pins the count
  at 23 entries for the current enum, update both together).

| Capability on Apple GPUs (iOS) | Flag |
|---|---|
| BC1/BC2/BC3 (DXT) | **no** (only `supportsBCTextureCompression` devices, i.e. Macs) |
| ASTC 4x4, ETC2 | yes from Apple2 (A8) |
| D24S8 | **no**: emulate with D32F + stencil |
| 16-bit packed colour (B5G6R5, ABGR4, A1BGR5) | yes from Apple2 |
| Max texture size | 8192 (Apple1/2), 16384 (Apple3+) |
| MSAA | up to 4x assumed (conservative) |
| Instancing, MRT | yes |
| Clip depth | [0, 1], top-left origin |

* **DXT policy** (`choose_texture_policy`, `storage_for_texture`): NFSU2 textures are DXT1/3/5. On devices without BC support a BC texture is
  stored as **ASTC 4x4** (8 bits/pixel: 2x the memory of DXT1, equal to DXT3/5), as **ASTC 6x6** (3.56 bpp, softer) when 2x the estimated BC size does not fit the
  texture budget, and as **RGBA8** (exact, 8x DXT1) only for textures of 32x32 pixels or fewer. The decision is pure arithmetic; the
  transcoder (BC -> ASTC without visible loss) belongs to the graphics layer. This is the one point where fidelity and memory conflict, so it is
  reported to nue-core / d3d9 rather than decided silently (see REQUESTS_ios.md).
* Memory: whole process inside about 2 GB (see section 5); the shader cache is a candidate for `MTLBinaryArchive` (iOS 14+, flag provided).

## 10. Diagnostics (`apple/nue_ios_diag.mm`)

* `nue_ios_log` -> `os_log` (subsystem `org.nue.engine`), visible in Console.app and `log stream`; dynamic strings are marked public.
* Signposts in the Points of Interest category: `nue_ios_signpost_begin/end/event`; they show up in Instruments (Time Profiler timelines).
* Crash handler: SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS on an alternate stack of the installing thread; writes
  `Caches/NUE/logs/last-crash.txt` (signal, fault address, pc/lr/sp on arm64, `backtrace_symbols_fd`), then restores the previous handler and
  re-raises so Apple's own report still gets produced. Only the installing thread has the alternate stack. `backtrace_symbols_fd` is not formally
  async-signal-safe. A guest-code fault is the translated-code owners' job (their vectored handler is Windows-specific).
* MetricKit `MXMetricManagerSubscriber` writes diagnostic payload JSON next to it (iOS 14+).

## 11. Host tests

```
cmake -S nue_engine/ios -B build_ios_host -G Ninja && cmake --build build_ios_host && ctest --test-dir build_ios_host --output-on-failure
```

Covers the lifecycle transition table (including exhaustive invariants), thermal/memory/frame-rate policy, capability flags and texture policy
arithmetic, path normalisation (traversal, drive letters, NUL), case-insensitive resolution, game-data validation, game-root detection, import
planning and free-space margins, CRC32 check value, audio policy, controller conditioning, steering geometry in all four orientations, last-used
input hysteresis, and the plist keys. No game data and no Apple headers are involved.
