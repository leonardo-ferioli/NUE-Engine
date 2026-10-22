# Requests from the Android area

To **core** (owner of `include/`, `src/core`, `src/platform/sdl3`, CMake, docs):

1. **App state ownership on Android.** The platform layer's `pump` calls `nue_internal_set_app_state` from the Activity callbacks
   (`onResume` FOREGROUND, `onPause` INACTIVE, `onStop` BACKGROUND, `onDestroy` TERMINATING), which is more precise than SDL's
   WILL/DID_ENTER_BACKGROUND events. The SDL3 backend should therefore **not** change the app state on Android (or ignore duplicates).
2. **Window id on SURFACE events.** `NUE_EVENT_SURFACE_CREATED/LOST` are pushed by the platform layer with `window_id = 0` (it has no
   access to window ids). Please fill the main window's id in the core when it is 0, or expose a lookup `nue_internal_main_window_id()`.
3. **SURFACE_LOST must be handled synchronously.** `pump` pushes it and then releases Android's `surfaceDestroyed` (the UI thread waits up
   to 750 ms). `nue_gfx` (and any raw-bgfx host) must destroy its swapchain inside a watcher, i.e. while `nue_internal_push_event`
   runs. Please confirm watchers run before the push returns for this event type (the app.h text says so for PAUSING/LOW_MEMORY/TERMINATING).
4. **Capabilities are read after platform `init()`**: the Android table fills `capabilities[]` inside `init` (needs the device facts and the
   GPU probe). If the core reads them earlier, add a second hook or call order change.
5. **Audio focus events.** Android can take audio away (calls, other apps). Needed: `NUE_EVENT_AUDIO_FOCUS_CHANGED` (payload: paused / ducked /
   full) and `nue_mixer`/audio device pause-resume + duck gain applied by the core. Until then the state is readable through the extension
   (`nue.android.v1`: `audio_focus_state`).
6. **Memory budget event/query.** `NUE_EVENT_APP_LOW_MEMORY` is sent for trim levels >= RUNNING_LOW. A portable `nue_system_get_memory_budget`
   (hard/soft/texture budget, pressure) would let `src/texture` size its cache from it; today the numbers are in the Android extension
   table (`get_memory_budget`, `set_memory_cap`). Defaults: hard cap 2 GiB (a policy default, configurable), 55 % of RAM.
7. **Entry/SDL_main.** `nue::entry` must define `SDL_main` (via `SDL_main.h`) in the game library that `NueActivity` starts (`main` by
   default, meta-data `org.nue.engine.main_library`); `libnue.so` is loaded first. SDL's own `JNI_OnLoad` is in `libnue.so`: no other
   library may define one.
8. **Preset.** `android-arm64` uses `ANDROID_PLATFORM=android-26`; the Android area builds with 29 (Gradle sets it from `minSdk`). Please align
   the preset to 29 or tell me why 26 is needed.
9. **CI.** Add a host step for the Android logic tests: `cmake -S src/platform/android/tests -B build_android_host && ctest --test-dir build_android_host`.
   (They are independent of the engine build and need no Android SDK.) The Android CI job also needs `git submodule update --init
   --depth 1 third_party/SDL` before configuring.
10. **Import.** The platform's `import` ops list files with forward-slash relative paths and unreliable sizes for some providers (virtual
    files report 0/-1). The core importer should treat a size mismatch after reading as an error only if the manifest says so, and use
    `NUE_IMPORT_FLATTEN_ROOT` (players often pick the parent folder). `open(URI)` may take seconds (tree enumeration): call it on the worker thread.
11. **Thread priorities.** Android maps `HIGH` to nice -8 and `REALTIME` to -16 (`THREAD_PRIORITY_AUDIO`); some OEM builds refuse negative
    nice for app threads; the call then falls back to normal and returns `NUE_OK_PARTIAL`.

To **texture**: read `nue.android.v1` -> `get_gpu_info` / `recommend_dxt` (native BC, ETC2, ASTC or RGBA8 decision) and `get_memory_budget` for the
cache size. ETC2 transcoding of DXT1/DXT3/DXT5 (RGB, RGB+1bit alpha, RGBA8/EAC) is the expected default on Mali/PowerVR.
