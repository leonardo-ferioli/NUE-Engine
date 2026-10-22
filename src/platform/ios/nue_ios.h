/* NUE iOS backend: the C surface between the core engine and the Objective-C++ glue in apple/.
 *
 * This is an internal header of the iOS backend (not the public NUE API: that lives in nue_engine/include/nue and is owned by nue-core).
 * The core's iOS platform adapter calls these functions and translates NueIosEvent into public nue_ events. Everything here is C so the
 * adapter and the sample app can be plain C or C++.
 *
 * Threading: every function must be called on the main thread unless its comment says otherwise. Events are delivered on the main thread.
 *
 * STATUS: written without access to macOS/Xcode; never compiled. See nue_engine/docs/PLATFORM_IOS.md.
 */
#ifndef NUE_IOS_H
#define NUE_IOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- events ---------------------------------------------------------------------------------------------------------------------------- */

typedef enum NueIosEventKind {
    NUE_IOS_EVENT_LIFECYCLE = 1,        /* a = LifecycleState (0 not_running, 1 inactive, 2 active, 3 background), b = LifecycleAction bits */
    NUE_IOS_EVENT_MEMORY_PRESSURE,      /* a = MemoryPressure (0 normal .. 3 critical), b = available bytes (os_proc_available_memory) */
    NUE_IOS_EVENT_THERMAL,              /* a = ThermalLevel (0 nominal .. 3 critical); f[0] = suggested max fps, f[1] = suggested resolution scale (0..1) */
    NUE_IOS_EVENT_POWER,                /* a = low power mode (0/1), b = battery state (0 unknown, 1 unplugged, 2 charging, 3 full), f[0] = level 0..1 or -1 */
    NUE_IOS_EVENT_AUDIO_INTERRUPTION,   /* a = 1 began / 0 ended, b = 1 if the system says audio may resume */
    NUE_IOS_EVENT_AUDIO_ROUTE,          /* a = route kind (RouteKind), b = 1 if the previous device was removed (headphones unplugged) */
    NUE_IOS_EVENT_SAFE_AREA,            /* f = top, left, bottom, right insets in points */
    NUE_IOS_EVENT_DISPLAY,              /* f[0] = width pts, f[1] = height pts, f[2] = scale, f[3] = max refresh Hz; a = interface orientation */
    NUE_IOS_EVENT_CONTROLLER,           /* a = 1 connected / 0 disconnected, b = pad index, f unused; family via nue_ios_pad_info */
    NUE_IOS_EVENT_INPUT_DEVICE,         /* a = InputDevice (1 touch, 2 keyboard, 3 mouse, 4 gamepad, 5 motion) */
    NUE_IOS_EVENT_STORAGE_PROGRESS      /* a = percent 0..100, b = bytes done; sent during nue_ios_import_* */
} NueIosEventKind;

typedef struct NueIosEvent {
    uint32_t kind;       /* NueIosEventKind */
    int64_t a, b;
    float f[4];
} NueIosEvent;

typedef void (*NueIosEventFn)(const NueIosEvent* event, void* user);

/* ---- init ------------------------------------------------------------------------------------------------------------------------------ */

typedef struct NueIosConfig {
    const char* game_dir_name;     /* folder under Documents holding the game files; NULL = "NFSU2" */
    int audio_mix_policy;          /* 0 exclusive, 1 respect silent switch, 2 mix with others, 3 duck others */
    int enable_motion;             /* start CoreMotion for gyro steering */
    int enable_crash_handler;      /* install signal handlers + MetricKit subscriber */
    int enable_memory_poll;        /* poll os_proc_available_memory about every second while active */
    uint16_t target_fps;           /* game's wanted frame rate (60); 0 = display maximum */
    int adapter_mode;              /* 1 when running under the NUE core (SDL3 owns audio, input and display events): the backend then only OBSERVES audio,
                                      does not install GameController/CoreMotion handlers and does not publish SAFE_AREA/DISPLAY events. 0 = standalone (sample app) */
    NueIosEventFn on_event;
    void* user;
} NueIosConfig;

/* Installs notification observers and starts the services. Safe to call once, from the main thread, after UIApplication exists
 * (for example from the SDL main function, SDL_EVENT_DID_FINISH_LAUNCHING time, or the sample's didFinishLaunching). Returns 0 on success. */
int nue_ios_init(const NueIosConfig* config);
void nue_ios_shutdown(void);

/* Tells the backend which UIWindow the game uses (SDL: SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER)).
 * Needed for safe-area and orientation queries. */
void nue_ios_set_window(void* ui_window);
/* Re-publishes NUE_IOS_EVENT_SAFE_AREA and NUE_IOS_EVENT_DISPLAY (call from SDL window resize / safe-area / orientation events). */
void nue_ios_refresh_display(void);

/* ---- paths and storage ----------------------------------------------------------------------------------------------------------------- */

typedef enum NueIosPathKind {
    NUE_IOS_PATH_GAME_ROOT = 1,    /* Documents/<game_dir_name>: the player's game files */
    NUE_IOS_PATH_SAVES,            /* Application Support/NUE/saves (created, excluded from backup = no) */
    NUE_IOS_PATH_SHADER_CACHE,     /* Caches/NUE/shaders (created, excluded from backup) */
    NUE_IOS_PATH_TEMP,             /* tmp/NUE */
    NUE_IOS_PATH_LOGS              /* Caches/NUE/logs */
} NueIosPathKind;

/* Copies the UTF-8 path (creating the directory) into buf. Returns the length (without NUL) or -1. */
int nue_ios_path(NueIosPathKind kind, char* buf, size_t size);

/* Bytes available for "important" usage (NSURLVolumeAvailableCapacityForImportantUsageKey) on the volume holding Documents; -1 on error. */
int64_t nue_ios_free_space(void);

/* Sets/clears NSURLIsExcludedFromBackupKey on path. Returns 0 on success. */
int nue_ios_exclude_from_backup(const char* path, int excluded);

/* Checks the game folder: returns 0 when the required files exist and are not truncated; otherwise a positive count of problems and a
 * human-readable list in `report` (UTF-8, truncated to size). `required_csv` is "relative/path:minbytes,relative/path:minbytes". */
int nue_ios_validate_game_data(const char* required_csv, char* report, size_t size);

/* Recursively copies a folder chosen with UIDocumentPicker (security-scoped URL) into the game folder, with free-space check, CRC check of
 * every file after the copy and NUE_IOS_EVENT_STORAGE_PROGRESS events. Blocks the calling thread: call it from a background thread
 * (events are then delivered on that thread; the adapter must queue them). Returns 0 on success, negative on error (see report). */
int nue_ios_import_folder(const char* source_bookmark_or_path, char* report, size_t size);

/* Presents UIDocumentPickerViewController for a folder; `callback` receives a bookmark-resolved path valid for this session (security scope
 * already started) or NULL on cancel. Main thread. */
typedef void (*NueIosPickFolderFn)(const char* path_or_null, void* user);
void nue_ios_pick_folder(NueIosPickFolderFn callback, void* user);

/* ---- power / session ------------------------------------------------------------------------------------------------------------------- */

void nue_ios_set_keep_awake(int enabled);               /* UIApplication.idleTimerDisabled */
void nue_ios_set_home_indicator_hidden(int hidden);     /* prefersHomeIndicatorAutoHidden + defer system edge gestures on the window's root VC */
int64_t nue_ios_available_memory(void);                  /* os_proc_available_memory(); -1 if unavailable. Any thread. */
void nue_ios_set_thread_qos(int qos);                    /* 0 user-interactive, 1 user-initiated, 2 utility, 3 background; applies to the calling thread. Any thread. */

/* Game Mode (iOS 18+): reports what the app declares. There is no public API to ask whether Game Mode is currently active.
 * Returns bit 0 = GCSupportsGameMode true in Info.plist, bit 1 = LSApplicationCategoryType is a games category, bit 2 = running on iOS 18+. */
int nue_ios_game_mode_info(void);

/* Sets the display-rate request (CAFrameRateRange). Also keeps a CADisplayLink alive so ProMotion honours it. 0 values = display default. */
void nue_ios_set_frame_rate(float minimum, float preferred, float maximum);

/* ---- graphics -------------------------------------------------------------------------------------------------------------------------- */

typedef struct NueIosGpuInfo {
    char name[64];
    int highest_apple_family;      /* 1..9, 0 unknown */
    int supports_bc;
    int unified_memory;
    int is_simulator;
    uint64_t recommended_max_working_set;
    uint64_t physical_memory;
    /* Derived capability flags (logic/caps.cpp), same order and meaning as nue::gpu::GpuCaps::texture_format. */
    uint8_t texture_format[32];
    uint32_t texture_format_count;
    uint32_t max_texture_size, max_anisotropy, max_samples;
    int depth24_stencil8;
    int texture_storage;           /* TextureStorage: 0 native_bc, 1 astc4x4, 2 astc6x6, 3 rgba8 (policy for DXT) */
    uint64_t hard_memory_limit, soft_memory_limit, texture_budget;
} NueIosGpuInfo;

int nue_ios_probe_gpu(NueIosGpuInfo* out);

/* Configures the CAMetalLayer bgfx will draw to (pixel format BGRA8, 3 drawables, vsync on, opaque, contentsScale = screen scale) and
 * returns the drawable size in pixels. `layer` is a CAMetalLayer* (SDL: SDL_Metal_GetLayer(SDL_Metal_CreateView(window))). */
int nue_ios_configure_metal_layer(void* layer, int framebuffer_only, uint32_t* width_px, uint32_t* height_px);

/* ---- input ----------------------------------------------------------------------------------------------------------------------------- */

typedef struct NueIosPadState {
    uint32_t buttons;              /* PadButton bits (logic/input_logic.hpp) */
    float lx, ly, rx, ry, lt, rt;  /* conditioned: radial deadzone applied */
    int family;                    /* PadFamily */
    int has_haptics;
} NueIosPadState;

/* Fills the snapshot of controller `index` (0..3, in connection order). Returns 0 if present. Any thread. */
int nue_ios_pad_state(int index, NueIosPadState* out);
/* Rumble through CoreHaptics (GCController.haptics). strengths 0..1, duration in ms. Returns 0 on success. */
int nue_ios_pad_rumble(int index, float low, float high, uint32_t duration_ms);
/* Steering from gyro: -1..1, positive = right. Returns 0 if motion is running and gravity is valid. Any thread. */
int nue_ios_motion_steering(float* out_steering);
/* Zero point of the wheel: call when the player holds the device the way they like (sets a neutral offset in degrees). */
void nue_ios_motion_recalibrate(void);

/* ---- audio ----------------------------------------------------------------------------------------------------------------------------- */

int nue_ios_audio_configure(int mix_policy);   /* sets category/mode/options (does not activate) */
int nue_ios_audio_activate(int active);        /* setActive: also applies buffer duration / sample-rate preferences */
double nue_ios_audio_output_latency(void);     /* seconds, 0 if unknown */

/* ---- queries used by the NUE platform adapter ------------------------------------------------------------------------------------------- */

/* Safe-area insets of a UIWindow in points (left, top, right, bottom) and its size in points. Main thread. Returns 0 on success. */
int nue_ios_safe_area(void* ui_window, float out_ltrb[4], float* width, float* height);

/* Device haptics (iPhone Taptic Engine through CoreHaptics). Any thread. */
int nue_ios_device_haptics_available(void);
int nue_ios_device_haptic(uint32_t duration_ms, float intensity);

typedef struct NueIosSystemInfo {
    char os_version[64];           /* "18.2" */
    char device_model[96];         /* hw.machine, e.g. "iPhone16,2" */
    char cpu_name[64];             /* best effort: "Apple arm64 (iPhone16,2)" - Apple gives no public chip name */
    int idiom;                     /* 0 unspecified, 1 phone, 2 pad, 3 tv, 5 mac(catalyst) - UIUserInterfaceIdiom */
    uint32_t logical_cores, physical_cores, performance_cores, efficiency_cores;   /* performance/efficiency 0 when unknown */
    uint32_t max_mhz;              /* 0: not exposed on iOS */
    uint64_t physical_memory;
    int is_simulator;
    int dark_mode;
    int translated;                /* Rosetta (macOS only) */
} NueIosSystemInfo;
int nue_ios_system_info(NueIosSystemInfo* out);
/* BCP-47 tag of the first preferred language, e.g. "es-ES". Returns length or -1. */
int nue_ios_locale(char* buf, size_t size);
/* Available memory in bytes: os_proc_available_memory(), or free+inactive pages when that is 0 (Simulator). -1 unknown. */
int64_t nue_ios_memory_available(void);

/* ---- diagnostics ----------------------------------------------------------------------------------------------------------------------- */

void nue_ios_log(int level, const char* tag, const char* message);   /* 0 debug, 1 info, 2 warning, 3 error; os_log subsystem "org.nue.engine". Any thread. */
uint64_t nue_ios_signpost_begin(const char* name);                    /* Instruments "Points of Interest"; returns id. Any thread. */
void nue_ios_signpost_end(const char* name, uint64_t id);
void nue_ios_signpost_event(const char* name);
/* Writes crash reports (signal, faulting address, backtrace) into the logs directory; call after nue_ios_init. Returns 0 on success. */
int nue_ios_install_crash_handler(void);

#ifdef __cplusplus
}
#endif
#endif /* NUE_IOS_H */
