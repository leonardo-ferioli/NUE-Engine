# Requests from the iOS backend to nue-core

First written before the nue-core API draft existed; items 1-7 are from that first version. Revised after merging the draft (commit 2ed5df1):
the iOS layer now implements `nue_platform_ios_get_ops()`, and items 8-14 at the end are the new open points.

## Events the backend produces (public API must be able to carry them)

| Backend event (`nue_ios.h`) | Needed public event / query |
|---|---|
| `LIFECYCLE` (state + action bits) | `app_state_changed` with at least: paused/resumed, "save now", "drop caches", "GPU must stop submitting" (mobile) |
| `MEMORY_PRESSURE` (level, bytes available) | `memory_pressure` with 4 levels and the bytes available; plus a query `available_memory()` |
| `THERMAL` (level, advised fps, advised scale) | `thermal_state_changed` + advice struct (advisory, the game decides) |
| `POWER` (low power, battery) | `power_state_changed` |
| `AUDIO_INTERRUPTION`, `AUDIO_ROUTE` | `audio_interrupted` / `audio_resumed` / `audio_route_changed(device_removed)`; the audio API needs "stop stream" and "recreate device" |
| `SAFE_AREA`, `DISPLAY` | `window_safe_area` (insets in points), `display_changed` (size pts, scale, orientation, max Hz) |
| `CONTROLLER`, `INPUT_DEVICE` | gamepad connect/disconnect with family (xbox/playstation/nintendo/generic); `last_input_device_changed` |
| `STORAGE_PROGRESS` | progress callback for long copy/import operations |

## Queries and settings the backend implements

* Paths: game root, saves, cache, temp, logs (`NUE_IOS_PATH_*`); free space; `validate_game_data(required files)`; import-from-picker.
* GPU info and capability flags in the order of `nue::gpu::GpuCaps::texture_format` (23 formats today); DXT storage policy (see below).
* `set_keep_awake`, `set_frame_rate_range`, `set_thread_qos`, `set_audio_mix_policy`, gyro steering (-1..1) and gamepad rumble.

## Decisions I need from the core / graphics owners

1. **DXT on Apple GPUs.** BC1/BC2/BC3 are unavailable on iOS. The policy in `logic/caps.*` stores BC textures as ASTC 4x4 (2x BC1 memory), ASTC 6x6
   under memory pressure and RGBA8 for tiny textures. The d3d9 layer needs a BC -> ASTC transcoder (or an offline conversion + cache in
   `Caches/NUE/shaders`) and must report `bc1/bc2/bc3 = false`, `astc4x4 = true`, `d24s8 = false` (use `d32f` with stencil) in `GpuCaps` when running on Metal on iOS.
   Is "fidelity-first" satisfied by ASTC 4x4, or is lossless RGBA8 required (about 8x the memory of DXT1)? Please decide.
2. **32-bit guest address space.** An iOS arm64 process has nothing mapped below 4 GiB by default (`__PAGEZERO`). The fast backend's memory
   model needs real mappings at the guest addresses. Either the link step uses a reduced pagezero (unverified on iOS) or the model needs
   an indirection on 64-bit hosts. This is an architecture-level question affecting Android and Linux x64 too.
3. **No JIT.** `dynamic_code` must compile out any native code generation on iOS (interpreter only).
4. **Event delivery thread.** The backend emits events on the main thread, synchronously (the lifecycle handler must stop audio before returning).
   The core adapter should either handle them inline or queue them; tell me if you want a different contract.
5. **SDL3 pin.** The iOS CMake fetches `release-3.4.18` unless `-DNUE_SDL3_SOURCE_DIR` points at the core's checkout. Tell me the version you pin.
6. **CMake integration.** `nue_engine/ios/CMakeLists.txt` is self-contained (host tests + iOS glue). The core's top-level CMake should
   `add_subdirectory(ios)` under `if(APPLE)` / iOS toolchain or tell me the target names it expects (`nue_ios_logic`, `nue_ios_glue`).
7. **Gamepad source.** Resolved by the core's design: SDL3 delivers gamepads, sensors and touch. The iOS layer runs in *adapter mode* and installs
   no GameController / CoreMotion handlers, so it never fights SDL's. (`nue_ios_pad_*` stays available for the standalone sample.)

Item 6 is also resolved: the core includes `src/platform/ios/CMakeLists.txt`, which adds the glue to `nue_platform_obj`.

## After reading the API draft (merged 2ed5df1)

8. **Frameworks do not reach the final library.** `nue` takes the platform objects with `$<TARGET_OBJECTS:nue_platform_obj>`, so a
   `target_link_libraries(nue_platform_obj PUBLIC "-framework ...")` is not propagated to `nue` or to the app. For iOS the root file must link
   `Foundation UIKit Metal QuartzCore AVFoundation GameController CoreHaptics CoreMotion MetricKit UniformTypeIdentifiers` (and the ones SDL3 needs)
   into `nue` itself, e.g. `if(NUE_OS STREQUAL "ios") target_link_libraries(nue PUBLIC ...)`. Also: if `enable_language(OBJCXX)` inside
   `add_subdirectory(src/platform/ios)` is rejected by CMake, add `OBJCXX` to `project(nue ... LANGUAGES C CXX)` under `if(IOS)`.
9. **`nue_window_get_safe_area` semantics.** The header says "insets" but returns a `nue_rect_i`. The iOS layer returns the *safe rectangle* inside the
   window in logical units (x = left inset, y = top inset, w/h = the remaining size), matching `SDL_GetWindowSafeArea`. Please confirm.
10. **Audio interruptions.** There is no event for "audio interrupted / resumed" (phone call, Siri) or "headphones unplugged". SDL3 handles the device,
    but the game must pause its music state. Suggest `NUE_EVENT_AUDIO_INTERRUPTED` / `NUE_EVENT_AUDIO_RESUMED`; for now the iOS layer only logs them.
11. **Game Mode.** `NUE_CAP_GAME_MODE` stays clear on iOS (declare-only, no public query) and `nue_power_info.game_mode` is always `UNKNOWN`. Say if you
    prefer a separate capability such as `NUE_CAP_GAME_MODE_DECLARED`.
12. **Data root layout.** I return `Documents/<org>/<app_id>/GameData` for `NUE_ROOT_DATA` (as in `storage.h`). The player can copy the game there
    directly (Finder / Files) and use it in place without `nue_import_*`. Please confirm that a pre-populated data root is a supported path and what
    makes `nue_import_is_complete` true in that case.
13. **Entry stub.** Not needed in my folder: `src/platform/sdl3/entry` can provide `UIApplicationMain` through SDL's `SDL_main`. The iOS layer starts its
    services from `ios_init` on the main thread (dispatching there if `nue_init` runs elsewhere).
14. **`ios.audio_mix` config key.** The layer reads `nue_config_get_string("ios.audio_mix")` at init ("exclusive" default, or "respect_silent") and
    exports `SDL_AUDIO_CATEGORY` for SDL. If the config file is loaded after platform init the first run uses the default: tell me the load order.
