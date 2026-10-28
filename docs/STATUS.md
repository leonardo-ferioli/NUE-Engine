# STATUS (kept honest; updated with every commit that changes it)

Last update: core block 4 (consumable SDK, ABI guard, real file mapping). Legend: **tested** = implemented and covered by passing automated tests on this machine (Windows x64 and x86, clang; null backend and SDL3 with its dummy drivers) or by CI; **written** = code exists and compiles, NOT exercised by a test; **skeleton** = declarations or stubs only; **-** = nothing yet. Nothing has been run on a real window, sound card, GPU, Linux, Android or iOS device by the core session.

## Pinned third-party

| Component | Version | Status |
|---|---|---|
| SDL3 | release-3.4.18 (commit 829a65d) | submodule, built static inside libnue, used by the SDL3 backend (tested with the dummy drivers) |

## Core + backends

| Module | Header | null backend | SDL3 backend | Tests |
|---|---|---|---|---|
| version, result names, last error, struct/ABI layout | base/core | tested | shared | test_base (x64 + x86) |
| init/shutdown (refcount, version check), allocator hooks, build info | core | tested | shared | test_core |
| log: levels, tag levels, stderr/file(rotating)/ring/callback sinks | core | tested | shared | test_core |
| config: typed store, INI load/save, declare/clamp, watchers | core | tested | shared | test_core |
| hashing (crc32, sha256), clocks, precise sleep, threads, mutex/cond/semaphore, TLS | core | tested (deterministic + real clock) | shared | test_core |
| events: queue, capacity policy, watchers, enable/flush/peek, lifecycle, engine loop `nue_app_run` | app | tested | - | test_events |
| displays, windows (modes, DPI, attach to native handle, snapshots, cursor ops) | window | tested | - | test_events |
| input state: keyboard, mouse, touch, gamepad (null devices), last-used device + glyph family, deadzone | input | tested | - | test_events |
| input not covered: sensors, haptics, text input/IME, mapping DB, rumble to real devices, joystick raw | input | written (forwarding) | - | - |
| audio devices + streams (push, callback, volume, pause on background, latency query/change, position, stats) | audio | tested | - | test_audio |
| audio conversion (format, channels, linear resampling, streaming resampler) | audio | tested | shared | test_audio |
| audio stream conversion path (device format != app format) | audio | written (null device always accepts the app format, so untested) | - | only via nue_audio_convert |
| mixer engine: static / ring / callback voices, pan law, gain ramps, pitch + resampling, buses, effects (low/high-pass, echo, reverb, compressor, callback), 3D (attenuation models, cone, doppler, left-handed), voice limit + priority stealing, ring cursors + notification positions, deterministic output, stream-attached mode | mixer | tested (15 cases, 350 checks) | n/a | test_mixer |
| mixer locking | mixer | one short mutex per mixer (render takes it per block), NOT lock-free; no allocation while mixing | n/a | - |
| VFS: roots, overlay mounts, normalisation, case-insensitive resolution, `\`->`/` | storage | tested | n/a | test_storage |
| files: open/read/write/seek/append/truncate/atomic save/load, 32-bit generation-checked ids, list/glob/stat/rename/remove | storage | tested | n/a | test_storage |
| async reads (worker pool, priority, events, cancel), manifest verification, hashes | storage | tested | n/a | test_storage |
| data import (directory source, resumable, verified, markers, progress/finish events) | storage | tested for PATH sources | n/a | test_storage |
| file mapping (`nue_file_map`): real read-only mapping (MapViewOfFile / mmap), unaligned offsets, outlives the file handle, heap-copy fallback for empty ranges / unmappable files, leaks released at shutdown; `NUE_CAP_MMAP_FILES` set | storage | tested (3 cases; Windows x64 + x86 run, POSIX path compiled for Linux x64 and macOS arm64 only) | n/a | test_storage |
| installed SDK: `cmake --install` of every preset, `find_package(nue)` (nue::nue, nue::entry, nue::assets, SDL3 for static SDKs, tools, docs), external consumer `samples/consume_sdk` | cmake | tested on Windows x64 and x86 (static CRT) by ctest `nue_sdk_install`: install tree complete, consumer configured, built and its 3 tests run; shared SDK (DLL) installed and consumed by hand on Windows x64 (3 consumer tests passed; not in ctest); Linux, Android, iOS: **not tried by the core session** | n/a | tests/sdk_install.cmake |
| ABI guard: struct sizes/offsets, enum values, constants and function names of `include/nue/*.h` against committed references (`ptr64`, `ptr32`), version-bump rules per group (core, texture, assets) | all headers | tested (Windows x64 + x86 runs; the `ptr64` table also satisfied by Zig/clang static asserts for Linux x64/arm64, Android arm64 headers and macOS arm64; `ptr32` for Windows x86, Linux armhf; i386 System V has other alignments and no table) | n/a | test_abi, tools/abi |
| import from URI / picker (SAF, iOS) | storage | written (needs the platform `import` ops, none exist yet) | n/a | - |
| power: info, thermal/power events, request bookkeeping, thread helpers + perf hints (platform ops forwarded) | power | tested without a platform layer (reports honestly "not honoured") | n/a | test_events |
| system info + capability flags | system | tested (generic values) | n/a | test_diag |
| gfx: renderer list, PREPARE mode lifecycle (resize/suspend/resume/listeners/destroy), NOOP renderer | gfx | tested | n/a | test_events |
| SDL3 backend: displays + modes, windows (create, size, mode, DPI, safe area, native handle, attach HWND/X11 where SDL allows), cursor clip/relative, text input, keyboard/mouse/touch/gamepad event translation, gamepad state/rumble/LED/sensors/mappings, device sensors, audio devices + streams (SDL converts formats), lifecycle events, fullscreen policy under gamescope | all | - | tested with the dummy video/audio drivers (windows, events, audio callback, mixer on a real SDL stream, gamepad API without devices) | test_sdl3 |
| SDL3 backend: real displays/GPU surfaces/devices, hot-plugged gamepads, IME, touch, sensors, X11/Wayland/Android/iOS behaviour | all | - | **written, not run** on any real device | - |
| gfx MANAGED mode (bgfx init/reset/frame/shutdown, renderer choice + config, suspend/resume, device lost/reset, capture, caps, tex caps, GPU budget, shader cache dir, bgfx log routing), `-DNUE_WITH_GFX=ON` | gfx | **implemented**; tested on NOOP (bgfx Noop renderer, null backend) and on **real Direct3D 11, Vulkan, Direct3D 12 and OpenGL** (Windows x64, hidden windows, pixels read back); `GFX.md` section 10 | Windows x64 on the dev machine (AMD iGPU); Linux/Android/iOS **not run** (Android/iOS built by CI only) | nue_gfx_test_lifecycle, nue_gfx_smoke, nue_gfx_local_renderers_hidden (label gpu, manual) |
| memory info + budgets (policies, config overrides, eviction hooks, pressure events, per-class defaults) | memory | tested (simulated machines) | n/a | test_memory |
| memory info from a real machine | memory | needs `memory_info`/`memory_info_ex` from the platform layers (generic layer reports nothing) | n/a | test_memory (sanity only) |
| timers (`nue_timer_*`), frame limiter | core | tested (virtual clock + a real-time smoke test) | shared | test_hooks |
| refresh-rate / frame-limit requests, screen keyboard, glyph override, TDP fields, platform class flags, suspend/audio-interruption events | window/input/power/system/app | tested at API level; real effect needs the platform layers (`window_request_refresh`, `screen_keyboard`, `system_info`, `power_*`) | - | test_hooks |
| GPU capability probe (`nue_gpu_probe`, `nue_gfx_get_gpu_caps`) | gfx | platform `gpu_probe` (all four layers have one) or the conservative table (`probed = false`); `nue_gfx_get_gpu_caps` merges the live bgfx caps per format family (incl. `NUE_FMTSUP_EMULATED`) | live merge run on D3D11 only | test_hooks, nue_gfx_local_renderers_hidden |
| init order (pre_init -> storage -> settings -> backend -> platform), appended platform-ops fields guarded by struct_size | core | tested | n/a | test_hooks, all suites |
| diag: reports, notes, breadcrumbs, asserts, counters, frame stats, Chrome trace | diag | tested | n/a | test_diag |
| diag: crash handler installation | diag | core side written; needs a platform layer (`crash_install`), none exist yet | n/a | test_diag (reports "unsupported") |
| C++ wrapper (`nuepp`: Result/Expected, Context, Mutex/Thread/Timer, Window, Gfx, AudioStream, Mixer, File/vfs, config, Budget; builds without exceptions on non-Windows) | nuepp | tested (79 checks; -fno-exceptions only enforced off Windows) | n/a | test_wrapper |
| samples (hello_window via `nue::entry`, hello_audio, hello_input) | - | built by CI and by me, **never run** (they open windows / make sound) | - | - |
| `nue::entry` stub (`nue_main` -> `main`/SDL_main) | entry | built and linked into hello_window; **not run** | - | - |

## Platforms

| Platform | Layer | Status |
|---|---|---|
| Windows x64 / x86 | `src/platform/windows` | owned by nue-windows; core builds and tests pass with the generic layer on both |
| Linux x64 | `src/platform/linux` | owned by nue-linux; not built by the core session |
| Android arm64 | `src/platform/android`, `android/` | owned by nue-android. **Verified**: the engine AAR and the sample APK build with the real NDK (r28c, AGP 9.4.1, Gradle 9.8.2; arm64-v8a and x86_64), 16 KB alignment and the exported JNI entry points checked on the binaries, 12 host ctest suites green (`src/platform/android/tests`). **Not verified**: nothing has run on any emulator or device (this machine has no hardware acceleration for the emulator; the user decided not to enable it). `android/scripts/run_emulator_tests.ps1` is written, never run end to end. See `PLATFORM_ANDROID.md` |
| iOS arm64 | `src/platform/ios` | owned by nue-ios; not built by the core session |

## Known gaps / decisions pending

* `docs/ABI.md` (native + legacy ABI) and `docs/REPLIES_core.md` (answers to the platform/texture sessions) are written; `src/legacy` and `include/nue_legacy` are reserved, empty.
* Texture hook: `nue_texture_obj` target and `NUE_HAS_TEXTURE`; the module itself belongs to nue-texture.

* The SDL3 backend has no raw-joystick support for devices SDL cannot map as gamepads, no capture streams, no haptics (platform ops own phone vibration), no real-device validation.
* `nue_gfx` MANAGED mode needs `-DNUE_WITH_GFX=ON` (default OFF; bgfx is a recursive submodule). See `GFX.md`.
* The i386 System V layout (`alignof(uint64_t) == 4`) has no ABI table; nothing in the supported matrix builds for it.
* The null backend's audio device accepts any format; conversion inside streams is only exercised when a real backend negotiates another format.
* Timestamp domain: the null backend in deterministic mode replaces the engine clock; the SDL3 backend will use the OS monotonic clock.
