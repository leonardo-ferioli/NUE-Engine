# NUE on Android: backend, packaging, decisions, status

Owner: the `android` area (`src/platform/android/`, `android/`, this file, `docs/REQUESTS_android.md`).
Written against core API 0.1 (`include/nue/*.h`, `src/core/internal/platform_ops.h`) and SDL3 `release-3.4.18`; API 0.1.1 hooks implemented.

## 0. Honest status (read this first)

| Level | What |
|---|---|
| **Compiled and run on the host (12 ctest suites, all green)** | everything in `src/platform/android/logic/` (lifecycle state machine, memory budget, display mode choice, thermal governor, ADPF target planner, GPU/texture-format policy, input/key/axis tables, audio plan + focus state machine, CPU big/little classification, storage path sanitising, manifest/journal/marker formats, **the import engine** (copy + CRC-32 + journal + resume + cancel + disk-full handling, run against real directories), crash-report text, event queue incl. a threaded test) |
| **Built for Android (2026-10-22, NDK r28c 28.2.13676358, AGP 9.4.1, Gradle 9.8.2, JDK 21)** | `gradle :nue-engine:assembleDebug` -> `nue-engine-debug.aar` (libnue.so for arm64-v8a = core + SDL3 backend + this platform layer, all Java compiled against the real `android.jar`, prefab package `nue-engine`); `gradle :sample:assembleDebug` -> `sample-debug.apk` (libmain.so, libnue.so, libc++_shared.so). Checked on the binaries: `zipalign -c -P 16` ok, every `LOAD` segment aligned 0x4000, the 30 `Java_org_nue_engine_NueBridge_*` entry points are exported, exactly one `JNI_OnLoad` (SDL's) |
| **Also built** | the sample for x86_64 (`-Pnue.abis=arm64-v8a,x86_64`): `libnue.so`, `libmain.so`, `libc++_shared.so` for both ABIs in one APK |
| **Not built / not checked** | release build with R8 (`assembleRelease`), armeabi-v7a, `lint`; Android Studio itself (its installer download failed twice, not needed for building) |
| **Run on a device or emulator** | **nothing**. The emulator (37.2.12) and the Android 16 x86_64 image are installed and the AVD `nue_test` exists, but this machine has **no hardware acceleration for the emulator** (`emulator -accel-check`: "Android Emulator hypervisor driver is not installed", no hypervisor active; BIOS virtualisation and SLAT are on). A software-only start (`-no-accel`) crashed right after launch (exit 255). Enabling "Windows Hypervisor Platform" (or the AEHD driver) needs administrator rights and a reboot and was not done. `scripts/run_emulator_tests.ps1` is written and parse-checked but has **never run end to end** |
| **Not possible / deliberately absent** | see section 9 |

Errors found and fixed by the first real build: Kotlin DSL import in the sample script, a namespace slip in `android_crash.cpp`, `c++_static` vs prefab (a prefab library built with a static STL cannot be used by another library: both modules use `c++_shared` now), the prefab package is named after the module (`find_package(nue-engine)`, target `nue-engine::nue`). Section 8 has the commands.

## 1. Decisions and reasons

### Activity model: `SDLActivity` (not GameActivity)
The portable core is SDL3 on every platform. SDL3's Android backend (surface, touch, mouse/keyboard, joysticks incl. HID, sensors,
audio devices, text input, clipboard, locale) assumes SDL owns the activity. GameActivity (AGDK) would add a second input/lifecycle path
next to SDL and we would re-implement what SDL already does. The parts of AGDK that do not need the activity (ADPF, frame-rate hints,
Game Mode/GameState) are used directly. `NueActivity extends SDLActivity` adds: lifecycle events to native, `NueSurface` (surface
events + insets), memory/power/audio/input/display/storage/diagnostics helpers.

### Main loop ownership
SDL starts the game's `SDL_main` on its own thread ("SDLThread"); that is the **event thread** of the engine.
* *Engine loop* (`nue_app_run`): the engine loop runs on that thread.
* *Host loop* (recompiled game): the game's loop runs on that thread and calls `nue_pump_events()` / `nue_poll_event()` itself.
Nothing blocks the Android UI thread, so a slow frame cannot cause an ANR (input dispatch timeout) by itself; see section 7.

### SDK levels
* `minSdk 29` (Android 10): thermal status API, multi-resume (a visible window in split screen stays RESUMED), scoped storage
  rules uniform for all apps, Vulkan 1.1 on essentially all GPUs shipping with it, 64-bit only phones are common, `AudioFocusRequest`
  (26), `setFrameRate` hooks (30, guarded). Android 9 and older would need a second code path for each of these.
* `targetSdk 35` (Android 15): edge-to-edge enforced (handled: insets + cutout mode), 16 KB page size aware, foreground-service and
  background-start rules irrelevant to a game. `compileSdk 36`. Raise targetSdk to 36 after testing; Play Store is not a target
  (sideload), so the Play deadline does not apply.
* Features: `glEsVersion 3.0` required; `android.hardware.vulkan.version 1.1` **optional** so GLES-only devices can install.

### Toolchain (checked against the AGP 9.4 compatibility table of 2026-10-21)
AGP 9.4.1, Gradle >= 9.6 (wrapper file pins 9.8.2), JDK 17 (21 is fine; the machine's JDK 25 is untested with AGP: use Android Studio's
bundled JBR or install Temurin 21), SDK build tools 36.0.0, NDK 28.2.13676358 (r28c, AGP's default and SDL's own pin), CMake 3.22.1
(SDK managed). arm64-v8a only (`nue.abis`); armeabi-v7a/x86_64 can be added in `gradle.properties` (x86_64 is useful for the emulator).

### 16 KB page size
ELF segments are linked with `-z max-page-size=16384` (and `ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON`); NDK r28 does it by default.
`jniLibs.useLegacyPackaging=false` keeps libraries uncompressed and page aligned in the APK (AGP zip-aligns to 16 KB). Verify with
`llvm-readelf -lW libnue.so` (all `LOAD` align `0x4000`) and `zipalign -c -P 16 -v 4 app.apk`.

### R8 / ProGuard
`nue-engine/consumer-rules.pro` keeps `NueBridge` (native code calls its static methods by name and the JNI symbols are named after
it), the activity/surface classes and SDL's Java classes. Release builds of the sample use `isMinifyEnabled = true`.

### Signing for sideload
`scripts/make_keystore.ps1` creates `sideload.jks` + `keystore.properties` (git-ignored). Without them the release APK is signed with
the debug key, fine for `adb install`.

### JNI without `JNI_OnLoad`
SDL3 (static inside `libnue.so`) defines `JNI_OnLoad`. Ours would be a duplicate symbol, so the natives are exported with their
long JNI names (`Java_org_nue_engine_NueBridge_*`) and the first call (`NueActivity.onCreate`) caches the VM and the class. The object
file with them is kept in a static `libnue.a` by an anchor reference from `platform_android.cpp`.

## 2. Architecture

```
NueActivity (SDLActivity)  --lifecycle/trim/focus-->  NueBridge (static natives, snapshots + events)
  NueSurface (SDLSurface)  --surface/insets------->        |                            ^ static callbacks (frame rate,
  NuePower/Display/Audio/Input/Memory/Storage/Sensors/Diagnostics (Java, Android APIs)  |  game state, picker, tree fds ...)
                                                           v                            |
                       jni/android_jni.cpp -> State (mutex) + EventQueue  <-- jni/android_api.cpp (nue_android_* C API)
                                                           v
        platform_android.cpp: nue_platform_ops (init/pump/system/storage/import/power/threads/ADPF/haptics/crash/log)
                                                           v
                                    core (events, power API, storage, diag)  +  SDL3 backend (window, input, audio)
```
Java pushes **snapshots** (device, display, storage, memory, thermal, battery, audio route) and **events** to native; native queries
never call JNI. Native-to-Java calls exist only for actions (frame-rate vote, keep-screen-on, picker, tree access, haptics...).

### Events produced by the platform layer (`pump`)
| Android | NUE |
|---|---|
| `onResume` / `onPause` / `onStop` / `onDestroy` | `nue_internal_set_app_state` FOREGROUND / INACTIVE / BACKGROUND / TERMINATING (-> PAUSING/PAUSED/RESUMING/RESUMED/TERMINATING) |
| `surfaceCreated` | `NUE_EVENT_SURFACE_CREATED` |
| `surfaceDestroyed` | `NUE_EVENT_SURFACE_LOST` pushed from `pump`; watchers run synchronously (nue_gfx destroys its swapchain), then the UI thread is released (it waits max 750 ms, only if an engine is polling) |
| `onTrimMemory` >= RUNNING_LOW, `onLowMemory` | `NUE_EVENT_APP_LOW_MEMORY` |
| thermal status / headroom | `NUE_EVENT_THERMAL_CHANGED` (hysteresis in `ThermalGovernor`) |
| battery saver, Game Mode, battery level | `LOW_POWER_MODE_CHANGED`, `GAME_MODE_CHANGED`, `POWER_CHANGED` |
| document picker result | completion of `import.pick_folder` |
Window focus, size, display, safe area, input devices, audio devices stay with the SDL3 backend. Audio focus and memory budget are
available through the extension table until core defines events for them (REQUESTS_android.md).

### Lifecycle (`logic/lifecycle_model.*`)
Pure state machine, tested: rendering runs only while *resumed and a surface exists*; `ReleaseSurface` is emitted before anything else when
the surface goes away; pause saves state first; rotation/fold changes become `ResizeSurface`; focus loss (split screen, shade) pauses the
simulation but keeps rendering and audio; audio-focus loss silences and pauses; a focus loss that happened while paused does not blip on
resume. State is saved on pause/stop (the process can be killed afterwards); the activity is not recreated on config changes
(`configChanges` lists them all), so there is no `onSaveInstanceState` flow.

## 3. Performance, power, Game Mode

* **Game Mode** (API 31): `game_mode_config.xml` in the app declares `supportsPerformance/BatteryGameMode`, `allowGameDownscaling=false`,
  `allowGameFpsOverride=false`; `appCategory=game`. There is no public listener for mode changes: polled every 5 s and on resume.
  `GameState` (API 33) is driven by `nue_power_request.game_hint` (gameplay / menu / loading).
* **ADPF** (API 33): `APerformanceHint_*` loaded with `dlsym` (no hard dependency); `nue_perf_hint_*` creates one session per call. The
  session target is `0.85 x frame time` (`adpf_target_ns`), re-sent only when it changes by > 1 %.
* **Thermal**: status listener (API 29) + `getThermalHeadroom(10)` polled every 5 s (API 30; documented limit 1 call/s). `ThermalGovernor`
  acts on the headroom before the status flips and relaxes one step per 20 s.
* **Sustained performance**: `NUE_PERF_SUSTAINED` -> `Window.setSustainedPerformanceMode` where supported. `NUE_PERF_HIGH` and
  `POWER_SAVER` have no Android lever and are reported as not applied.
* **Frame pacing**: `target_frame_time_us` -> `Surface.setFrameRate(fps, FIXED_SOURCE, ONLY_IF_SEAMLESS)` (API 30/31) and
  `preferredDisplayModeId` (choice by `choose_display_mode`: same resolution, exact rate > smallest integer multiple within 1 %, e.g.
  60 on 120 Hz -> present interval 2). **Swappy is not used**: it needs to own the swap call (`SwappyGL_swap` / `SwappyVk_queuePresent`)
  and bgfx presents internally; integrating it means patching bgfx. FIFO + frame-rate vote + ADPF is the supported combination.
* **CPU affinity**: `nue_thread_set_affinity` / `set_core_class` work through `sched_setaffinity` using cpufreq classes; discouraged
  on Android (use ADPF), may be refused by some devices. GPU affinity does not exist.
* **Keep screen on**: `FLAG_KEEP_SCREEN_ON` (no permission). A `PARTIAL_WAKE_LOCK` is held only during the data import.
* **Battery saver**: event + the extension power info halves the quality hint and caps to 30 fps.

## 4. Storage and game data import

The game data (~3.7 GB on the PC, ~1.2 GB needed) is copied once to app storage and read with plain file access afterwards.

Flow: `import.pick_folder` -> `ACTION_OPEN_DOCUMENT_TREE` (initial URI = Downloads) -> persistable URI permission -> URI string.
`import.open(URI, uri)` -> Java enumerates the tree with `DocumentsContract` queries (about 10x faster than `DocumentFile`) -> core calls
`next/read/close`; each `read` of an entry uses a detached `ParcelFileDescriptor` (no `ContentResolver` stream overhead).
`import.open(PATH, dir)` reads a directory with ordinary file access (the **drop folder** `Android/data/<pkg>/files/...`, which
a PC can fill over USB/MTP, needs no picker at all).

Rules this respects: scoped storage (Android 10+, no `READ_EXTERNAL_STORAGE`, no `MANAGE_EXTERNAL_STORAGE`); from Android 11 the picker
refuses the storage root, `Download` itself and `Android/data|obb`, so the player picks the game folder (a sub folder); OBB is
deprecated and not used; the data root is `filesDir/gamedata` (private, fast, survives updates, deleted on uninstall); optional second
location `getExternalFilesDir` (can be on an SD card). Saves/settings live in `filesDir/{saves,config}`; backup rules include only those
(never the game data). Free-space check: needed bytes + max(64 MiB, 2 %); on `ENOSPC` while writing the engine reports NoSpace.

`logic/import_engine.*` is a complete, tested importer (manifest filter, CRC-32 check, `.part` + fsync + rename, journal, resume,
cancel, collision and traversal protection). Core owns the portable importer used by `nue_import_start`; this one backs the
Android-specific `nue_android_import_*` API used by the sample and can be the reference for core's. File names are folded to ASCII lower
case (the game uses Windows names): the VFS must resolve case-insensitively (`NUE_MOUNT_CASE_SENSITIVE` off).

## 5. Audio
SDL3 opens the stream (AAudio, OpenSL ES fallback; usage "game" via the stream-role hint). Our part: `AudioManager` native rate and burst
size, `FEATURE_AUDIO_LOW_LATENCY/PRO`, route tracking (`AudioDeviceCallback`: speaker / wired / USB / HDMI / Bluetooth, flagged
high-latency for Bluetooth), **audio focus** (`USAGE_GAME` request, transient loss = pause, duck = quieter, permanent loss = pause until
gained, state machine tested), and `plan_audio`: open at the device's native rate (keeps AAudio on the fast path) with a buffer of whole
bursts covering the wanted latency; the engine resamples (44.1 kHz game -> 48 kHz device).

## 6. Input
SDL3 delivers touch (multi-touch), mouse/keyboard (pointer capture through `SDL_SetWindowRelativeMouseMode`), joysticks/gamepads (InputDevice
+ HIDAPI, rumble) and raw accelerometer/gyro. Ours: input-device hot-plug classification (`classify_sources`), the **last used device**
signal inputs (events watched in `dispatch*Event`, JNI only when the source changes), key/axis tables for the non-SDL path
(`pad_button_from_keycode`, Xbox-vs-generic right stick detection, radial dead zone), `TYPE_GAME_ROTATION_VECTOR` (tilt-to-steer, not in
SDL3), device vibration (`VibratorManager`, API 31, with fallback).

## 7. Graphics support, memory, diagnostics
* **GPU probe** (`android_gpu_probe.cpp`, own thread, <= a few hundred ms): Vulkan loaded with `dlopen`; device name, vendor id, API
  version, driver version, device-local heap, `framebufferColorSampleCounts`, and for BC1-3/4-5/6H-7, ETC2, ASTC 4x4, D24S8, D32S8,
  BGRA8 the **format properties** (sampled + linear filter), not only the feature bits. EGL pbuffer + ES 3 context: renderer, version, extension
  list. `pick_graphics_api` recommends Vulkan on Vulkan >= 1.1 with a non-software, non-Adreno-5xx device, else OpenGL ES; **the player
  can override** (the choice is the engine's setting, not hard-wired).
* **DXT policy** (`plan_dxt`): native BC if supported (typically Adreno), else ETC2 transcode (same size, mandatory in ES 3 / most Vulkan
  devices; Mali), else ASTC transcode, else RGBA8 decode (4x-8x memory). Only the decision lives here; the converter is `src/texture`.
* **Memory**: ActivityManager info every 3 s + `onTrimMemory`. `compute_budget`: hard = min(configurable cap, 55 % of RAM, rss + free
  above the kill threshold); soft = 85 % of hard, scaled down by pressure (moderate 0.9, low 0.75, critical 0.6, background 0.5);
  texture budget = soft - engine reserve. **The 2 GiB value is a default of `BudgetConfig`/`nue_android_set_memory_cap`, not a limit** of the
  platform layer.
* **Diagnostics**: logcat sink (`log_write`), signal handler writing raw records into the core's report directory (async-signal-safe,
  then chaining to debuggerd so the tombstone still exists), converted to `stack.txt` at the next start (module + offset, symbolicate with
  `ndk-stack`/`llvm-addr2line`), Java uncaught-exception writer, `ApplicationExitInfo` of the previous run logged (ANR, low-memory kill, crash),
  systrace/Perfetto markers (`nue_android_trace_*`, `NUE_TRACE_SCOPE`). ANR guidance: never block the UI thread (the game thread is SDL's, not
  the UI thread); the surface release hand-shake is bounded to 750 ms for that reason; import runs on its own thread.

## 8. Build and test (instrucciones en español)

### 8.1 Pruebas en el PC (no necesitan Android)
```
cmake -S src/platform/android/tests -B build_android_host -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build_android_host
ctest --test-dir build_android_host --output-on-failure
```
Resultado esperado: 12 pruebas (`android_storage`, `android_import`, ...) en verde.

### 8.2 Compilar el AAR y la app de ejemplo (necesita instalar cosas; no se ha hecho)
Qué instalar y para qué (todo gratis, unos 6-8 GB en total):
1. **Android Studio** (incluye el JDK 21 "JBR", el SDK Manager y el emulador). Es la forma más sencilla; con él no hace falta instalar Gradle.
2. Desde *SDK Manager*: **Android SDK Platform 36**, **SDK Build-Tools 36.0.0**, **NDK (Side by side) 28.2.13676358**, **CMake 3.22.1**,
   **Platform-Tools** (trae `adb`). Opcional: *Android Emulator* + una imagen "Android 15 / 16 KB page size" para probar.
3. En el teléfono: *Opciones de desarrollador* -> *Depuración USB* (o *Depuración inalámbrica*).

Pasos:
```
git submodule update --init --depth 1 third_party/SDL      # SDL3 fijado (clases Java + código C)
cd android
# Opción A (recomendada): abrir la carpeta android/ con Android Studio y pulsar Run en el móvil/emulador.
# Opción B (línea de comandos): crear el wrapper una vez (necesita Gradle instalado: scoop install gradle)
gradle wrapper --gradle-version 9.8.2
gradlew assembleDebug            # sample/build/outputs/apk/debug/sample-debug.apk
gradlew :nue-engine:assembleRelease   # AAR: nue-engine/build/outputs/aar/
```
Instalar y ver el informe de capacidades:
```
adb install -r sample/build/outputs/apk/debug/sample-debug.apk
adb logcat -s NUE
```
(Pedir permiso al usuario antes de instalar nada en su móvil.) Firma para sideload: `powershell -File scripts/make_keystore.ps1` y luego
`gradlew assembleRelease`.

Importar datos de prueba: copiar una carpeta pequeña al móvil (p. ej. `Download/prueba`), abrir la app, tocar *PICK FOLDER* y luego *IMPORT*.
Un manifiesto opcional va en `Android/data/org.nue.sample/files/import_manifest.txt` (formato en `logic/storage_paths.hpp`).

## 9. Capability table

Legend: **host** = compiled and unit-tested on the PC; **syntax** = parse/type checked against stubs only; **written** = never compiled; **device** = ran on a phone (none yet).

| Capability | How | host | syntax | written | device | Notes |
|---|---|---|---|---|---|---|
| Activity/surface lifecycle events + decisions | `LifecycleModel`, Java hooks, `pump` | yes | yes | Java | no | |
| Surface-release hand-shake | `n_onSurface`, `p_pump` | - | yes | Java | no | bounded 750 ms |
| Immersive, cutout, safe area, refresh choice | `NueDisplay`, `choose_display_mode` | choice logic | JNI | Java | no | fold postures: configuration only (no Jetpack WindowManager) |
| `Surface.setFrameRate` | `NueDisplay` | - | - | Java | no | API 30+ |
| Game Mode query, GameState | `NuePower` | mapping | JNI | Java | no | no change listener exists; polled |
| Thermal status/headroom, governor | `NuePower`, `ThermalGovernor` | yes | JNI | Java | no | |
| ADPF hint sessions | `android_adpf.cpp` | target planner | yes | - | no | API 33, dlsym |
| Sustained performance, keep screen on | `NuePower`, `NueDisplay` | - | - | Java | no | |
| Thread priority / affinity / core class | `platform_android.cpp` | classification | yes | - | no | discouraged on Android |
| SAF picker + tree import source | `NueStorage`, `platform_android.cpp` | importer on dirs | yes | Java | no | |
| Import engine (CRC, journal, resume) | `import_engine.*` | yes | JNI part | - | no | |
| Storage roots, free space | `p_storage_root`, `StatFs` | space maths | yes | Java | no | |
| Audio focus, route, native rate/burst | `NueAudio`, `AudioFocusModel`, `plan_audio` | yes | JNI | Java | no | stream itself: SDL3 |
| AAudio performance mode | SDL3 | - | - | - | no | not under our control |
| Touch, mouse, keyboard, gamepad, rumble | SDL3 | - | - | - | no | |
| Last used device, device kind, key/axis tables | `input_classify`, `NueInput` | yes | JNI | Java | no | |
| Game rotation vector | `NueSensors` | - | JNI | Java | no | |
| Vibration (device) | `NueInput`, `p_haptics_*` | - | yes | Java | no | |
| GPU probe (Vulkan + GLES), API recommendation | `android_gpu_probe.cpp`, `gpu_caps` | policy | no (needs Vulkan/EGL headers) | - | no | |
| DXT conversion policy | `plan_dxt` | yes | - | - | no | converter lives in `src/texture` |
| Memory budget, trim levels | `memory_budget`, `NueMemory` | yes | JNI | Java | no | cap is configurable |
| Crash handler + report conversion | `android_crash.cpp`, `crash_report` | text only | no | - | no | handler path never exercised |
| logcat, systrace markers, exit-info | `p_log_write`, `android_trace.cpp`, `NueDiagnostics` | - | yes | Java | no | |
| 16 KB page compatibility | link flags, packaging | - | - | yes | no | verify with readelf/zipalign |
| Gradle AAR + sample APK + prefab | `android/` | - | - | yes | no | AGP DSL details unverified |
| **Not possible**: Swappy with bgfx without patching bgfx; GPU affinity; Game Mode change callback; forcing HIGH/POWER_SAVER profiles; OBB; reading `Android/data` of other apps or the storage root through SAF (Android 11+); `MediaStore` bulk access for arbitrary files | | | | | | |
| **Not done**: Jetpack WindowManager fold posture, Play Games/Asset Delivery, `armeabi-v7a`, on-screen touch controls (engine/game level), Perfetto SDK custom tracks (only ATrace) | | | | | | |
