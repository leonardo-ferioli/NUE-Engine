# Requests from the Windows platform layer to other areas

## To core

1. **`enable_testing()` before the platform fragment.** The root `CMakeLists.txt` calls `enable_testing()` only after
   `add_subdirectory(src/platform/<os>)`. Tests registered inside the fragment (`add_test`) are then silently lost. The Windows fragment calls
   `enable_testing()` itself as a workaround; moving the call above the platform block would make that unnecessary.
2. **STATUS.md rows for Windows** (text below, please paste).
3. **`samples/*/main.cpp` glob.** `samples/windows_probe` is built from the Windows fragment (target `nue_win_probe`); its source is called
   `probe.cpp` on purpose so that the generic samples loop does not pick it up (it needs the backend headers, not `nue::nue`).
4. **Link libraries.** The Windows objects record their Win32 import libraries with `#pragma comment(lib, ...)`; linking `nue` therefore needs no
   extra `target_link_libraries(nue PRIVATE ...)` entries beyond what the root file has. (`target_link_libraries(nue_platform_obj PUBLIC ...)` as
   described in PORTING.md does not reach `nue`, because the object library is consumed through `$<TARGET_OBJECTS:...>`.)
5. **Clock domain.** `ops->sleep_until(deadline_ns)` converts the deadline to a relative wait with the public `nue_time_ns()`, so the null/deterministic
   backend's clock is honoured as long as `nue_time_ns()` returns it. In deterministic mode `pump` does nothing and `init` starts no threads.

## STATUS.md text (Windows rows)

| Capability | Provided by | Implemented | Tested how | Notes |
|---|---|---|---|---|
| `NUE_CAP_GAME_MODE` | `power_info` (effective power mode), `power_apply` (MMCSS "Games" for the game hint) | yes | host test on Windows 11 (state read; "on" never observed) | Windows cannot be told to enter Game Mode; the OS decides |
| `NUE_CAP_BATTERY`, `NUE_CAP_LOW_POWER_MODE` | `power_info`, `pump` events | yes (only when a battery exists for BATTERY) | host test (laptop) | events by polling once per second |
| `NUE_CAP_THERMAL_STATE` | ACPI "% Passive Limit" counters on a 5 s worker thread | yes, only if the machine has thermal zones | host test (laptop: 9 zones, no throttling seen) | OS thermal throttling signal, not a temperature |
| `NUE_CAP_KEEP_AWAKE` | `PowerCreateRequest` | yes | host test (API succeeds) | |
| `NUE_CAP_TIMER_RESOLUTION` | `timeBeginPeriod` / `NtSetTimerResolution` | yes | host test: 500 us granted | + Win11 "ignore timer resolution" opt-out via `disable_power_throttling` |
| `NUE_CAP_THREAD_PRIORITY`, `NUE_CAP_THREAD_AFFINITY` | `thread_*` | yes | host test | priority capped at ABOVE_NORMAL; REALTIME = MMCSS "Games" |
| `NUE_CAP_CORE_CLASS` | CPU Sets | set only on hybrid CPUs | logic only (test CPU is not hybrid) | |
| `NUE_CAP_CRASH_HANDLER`, `NUE_CAP_MINIDUMP` | `crash_*` | yes | child-process crash test: access violation | stack overflow, divide by zero covered by the module tests |
| `NUE_CAP_GPU_PREFERENCE`, `NUE_CAP_PERF_HINT`, `NUE_CAP_HAPTICS` | - | no | - | Windows reads the GPU preference at process start; no ADPF equivalent |
| Windows, input, audio devices | SDL3 backend | - | - | the native HWND attach / WASAPI exclusive modules exist (`nue::win`) but are not wired into the backend yet |

## Added with API 0.1.1

6. **Core tests assume the generic platform.** With the Windows layer linked, `nue_test_core`, `nue_test_diag`, `nue_test_events` and `nue_test_hooks` fail
   on expectations that only hold for the generic table: `platform_layer == "generic"`, crash handler UNSUPPORTED, `!NUE_CAP_GAME_MODE`, `power_apply == OK_NOOP`,
   `thread_set_priority == UNSUPPORTED`, `gpu_probe` PARTIAL / `!probed`, `!suspend_supported`. The same will happen on every OS. Suggestion: a switch
   (CMake option or `NUE_PLATFORM=generic`) that forces the generic table for those tests, or conditional asserts on `build_info.platform_layer`.
7. **MSVC (optional toolchain):** `src/core/window.cpp:113` declares `enum_displays` inside an `extern "C"` block while returning `std::vector`; clang accepts it,
   MSVC does not (C2526). Windows CI uses clang, so this only matters if MSVC builds are wanted.
8. **Portable mode contract** (see PLATFORM_WINDOWS.md): an explicit `user_data_path` makes the platform answer UNSUPPORTED for cache/settings so the core derives them below it.
