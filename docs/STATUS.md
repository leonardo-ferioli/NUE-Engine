# STATUS (kept honest; updated with every commit that changes it)

Last update: core block 1 (core + null backend + tests). Legend: **tested** = implemented and covered by passing automated tests on this machine
(Windows x64 and x86/-m32, clang, null backend); **written** = code exists, compiled, NOT exercised by a test; **skeleton** = declarations or stubs only;
**-** = nothing. Nothing has been run on a real window, sound card, GPU, Linux, Android or iOS device by the core session.

## Pinned third-party

| Component | Version | Status |
|---|---|---|
| SDL3 | release-3.4.18 (commit 829a65d) | submodule added (shallow); **not built or used yet** (the SDL3 backend is not written) |

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
| **mixer engine** (voices, buses, effects, 3D, ring voices, render) | mixer | **skeleton** (only unit helpers dB/centibel/pan are implemented and tested) | n/a | test_diag (helpers) |
| VFS: roots, overlay mounts, normalisation, case-insensitive resolution, `\`->`/` | storage | tested | n/a | test_storage |
| files: open/read/write/seek/append/truncate/atomic save/load, 32-bit generation-checked ids, list/glob/stat/rename/remove | storage | tested | n/a | test_storage |
| async reads (worker pool, priority, events, cancel), manifest verification, hashes | storage | tested | n/a | test_storage |
| data import (directory source, resumable, verified, markers, progress/finish events) | storage | tested for PATH sources | n/a | test_storage |
| file mapping | storage | **emulated by a heap copy** (no real mmap yet) | n/a | test_storage |
| import from URI / picker (SAF, iOS) | storage | written (needs the platform `import` ops, none exist yet) | n/a | - |
| power: info, thermal/power events, request bookkeeping, thread helpers + perf hints (platform ops forwarded) | power | tested without a platform layer (reports honestly "not honoured") | n/a | test_events |
| system info + capability flags | system | tested (generic values) | n/a | test_diag |
| gfx: renderer list, PREPARE mode lifecycle (resize/suspend/resume/listeners/destroy), NOOP renderer | gfx | tested | n/a | test_events |
| gfx MANAGED mode (bgfx init/reset/frame), caps | gfx | **not implemented** (returns NUE_ERR_UNSUPPORTED) | n/a | test_events |
| diag: reports, notes, breadcrumbs, asserts, counters, frame stats, Chrome trace | diag | tested | n/a | test_diag |
| diag: crash handler installation | diag | core side written; needs a platform layer (`crash_install`), none exist yet | n/a | test_diag (reports "unsupported") |
| C++ wrapper (`nuepp`) | nuepp | **skeleton** (umbrella include only) | n/a | - |
| samples (hello_window, hello_audio, hello_input) | - | **not written** | - | - |
| `nue::entry` stub | entry | **not written** | - | - |

## Platforms

| Platform | Layer | Status |
|---|---|---|
| Windows x64 / x86 | `src/platform/windows` | owned by nue-windows; core builds and tests pass with the generic layer on both |
| Linux x64 | `src/platform/linux` | owned by nue-linux; not built by the core session |
| Android arm64 | `src/platform/android` | owned by nue-android; not built by the core session |
| iOS arm64 | `src/platform/ios` | owned by nue-ios; not built by the core session |

## Known gaps / decisions pending

* SDL3 backend skeleton (window, input, audio, gamepad mappings, text input, sensors) is the next block, then the mixer engine, then the C++ wrapper and samples.
* Real memory mapping, `NUE_CAP_MMAP_FILES` stays clear until then.
* The null backend's audio device accepts any format; conversion inside streams is only exercised when a real backend negotiates another format.
* Timestamp domain: the null backend in deterministic mode replaces the engine clock; the SDL3 backend will use the OS monotonic clock.
