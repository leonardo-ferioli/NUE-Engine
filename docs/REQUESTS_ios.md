# Requests from the iOS backend to nue-core

Written before nue-core published its API draft (the `nue-core` branch had no commits when this was written). This lists what the iOS backend
needs the public API and the architecture to provide; names are placeholders for the core to choose.

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
7. **Gamepad source.** The core may use SDL's `SDL_Gamepad` or `nue_ios_pad_*`. I provide both; pick one so the mapping tables live in one place.
