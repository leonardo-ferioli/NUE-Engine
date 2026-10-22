/*
 * nue_android.h - Android platform extension of the NUE engine.
 *
 * This is the contract between the Android backend (Java glue + NDK code in
 * nue_engine/src/platform/android and nue_engine/android) and the portable
 * engine core.  The core backend (src/platform/sdl3) reaches Android-only
 * facilities through these functions; they are all thread safe unless noted.
 *
 * Design rules:
 *  - Java pushes state snapshots to native (no JNI on query paths); native
 *    keeps the latest values and a bounded event queue that the engine polls.
 *  - Nothing here depends on SDL; the window / GL / Vulkan surface itself is
 *    the SDL3 window, the ANativeWindow is exposed for bgfx.
 *  - C API, prefix nue_android_.  Structs are plain data, sizes are versioned
 *    through the leading `struct_size` member.
 */
#ifndef NUE_ANDROID_H
#define NUE_ANDROID_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define NUE_ANDROID_EXPORT __attribute__((visibility("default")))
#else
#define NUE_ANDROID_EXPORT
#endif

#define NUE_ANDROID_API_VERSION 1

/* ------------------------------------------------------------------ events */

typedef enum NueAndroidEventType {
    NUE_ANDROID_EVENT_NONE = 0,
    NUE_ANDROID_EVENT_LIFECYCLE,     /* i[0] = NueAndroidLifecycle */
    NUE_ANDROID_EVENT_SURFACE,       /* i[0] = NueAndroidSurface, i[1] = width, i[2] = height */
    NUE_ANDROID_EVENT_FOCUS,         /* i[0] = 1 gained, 0 lost */
    NUE_ANDROID_EVENT_MEMORY,        /* i[0] = ComponentCallbacks2 trim level (-1 = onLowMemory), i[1] = NueAndroidMemPressure */
    NUE_ANDROID_EVENT_THERMAL,       /* i[0] = NueAndroidThermalLevel, i[1] = PowerManager status, f[0] = headroom (NaN = unknown) */
    NUE_ANDROID_EVENT_GAME_MODE,     /* i[0] = GameManager mode (1 standard, 2 performance, 3 battery, 0 unsupported) */
    NUE_ANDROID_EVENT_BATTERY_SAVER, /* i[0] = 1 power save mode on */
    NUE_ANDROID_EVENT_AUDIO_FOCUS,   /* i[0] = NueAndroidAudioFocusAction */
    NUE_ANDROID_EVENT_AUDIO_ROUTE,   /* i[0] = NueAndroidAudioRoute, i[1] = 1 if the route adds large latency */
    NUE_ANDROID_EVENT_DISPLAY,       /* i[0] = width, i[1] = height, i[2] = rotation 0..3, f[0] = refresh Hz */
    NUE_ANDROID_EVENT_SAFE_AREA,     /* i[0..3] = left, top, right, bottom in pixels */
    NUE_ANDROID_EVENT_INPUT_DEVICE,  /* i[0] = Android device id, i[1] = 1 added / 0 removed, i[2] = NueAndroidInputKind */
    NUE_ANDROID_EVENT_LAST_INPUT,    /* i[0] = NueAndroidInputKind of the last used input device */
    NUE_ANDROID_EVENT_IMPORT,        /* i[0] = NueAndroidImportState, i[1] = files done, i[2] = files total, l[0] = bytes done, l[1] = bytes total */
    NUE_ANDROID_EVENT_TREE_PICKED    /* i[0] = 1 picked / 0 cancelled (storage access framework result) */
} NueAndroidEventType;

typedef enum NueAndroidLifecycle {
    NUE_ANDROID_LIFE_CREATE = 1,
    NUE_ANDROID_LIFE_START,
    NUE_ANDROID_LIFE_RESUME,
    NUE_ANDROID_LIFE_PAUSE,
    NUE_ANDROID_LIFE_STOP,
    NUE_ANDROID_LIFE_DESTROY
} NueAndroidLifecycle;

typedef enum NueAndroidSurface {
    NUE_ANDROID_SURFACE_CREATED = 1,
    NUE_ANDROID_SURFACE_CHANGED,
    NUE_ANDROID_SURFACE_DESTROYED
} NueAndroidSurface;

typedef enum NueAndroidMemPressure {
    NUE_ANDROID_MEM_NONE = 0,
    NUE_ANDROID_MEM_MODERATE,   /* running, system getting low */
    NUE_ANDROID_MEM_LOW,        /* running, system low: release caches */
    NUE_ANDROID_MEM_CRITICAL,   /* running, system critical: release everything optional */
    NUE_ANDROID_MEM_BACKGROUND  /* UI hidden / backgrounded: process may be killed */
} NueAndroidMemPressure;

typedef enum NueAndroidThermalLevel {
    NUE_ANDROID_THERMAL_NOMINAL = 0,  /* NONE */
    NUE_ANDROID_THERMAL_FAIR,         /* LIGHT */
    NUE_ANDROID_THERMAL_SERIOUS,      /* MODERATE / SEVERE */
    NUE_ANDROID_THERMAL_CRITICAL      /* CRITICAL / EMERGENCY / SHUTDOWN */
} NueAndroidThermalLevel;

typedef enum NueAndroidAudioFocusAction {
    NUE_ANDROID_AUDIO_FOCUS_NONE = 0,
    NUE_ANDROID_AUDIO_FOCUS_RESUME,   /* gained (again): resume output at full volume */
    NUE_ANDROID_AUDIO_FOCUS_PAUSE,    /* lost: pause output */
    NUE_ANDROID_AUDIO_FOCUS_DUCK,     /* lower the volume (about -14 dB) */
    NUE_ANDROID_AUDIO_FOCUS_UNDUCK
} NueAndroidAudioFocusAction;

typedef enum NueAndroidAudioRoute {
    NUE_ANDROID_ROUTE_UNKNOWN = 0,
    NUE_ANDROID_ROUTE_SPEAKER,
    NUE_ANDROID_ROUTE_WIRED,
    NUE_ANDROID_ROUTE_BLUETOOTH,
    NUE_ANDROID_ROUTE_USB,
    NUE_ANDROID_ROUTE_HDMI
} NueAndroidAudioRoute;

typedef enum NueAndroidInputKind {
    NUE_ANDROID_INPUT_UNKNOWN = 0,
    NUE_ANDROID_INPUT_TOUCH,
    NUE_ANDROID_INPUT_GAMEPAD,
    NUE_ANDROID_INPUT_KEYBOARD,
    NUE_ANDROID_INPUT_MOUSE,
    NUE_ANDROID_INPUT_STYLUS,
    NUE_ANDROID_INPUT_TOUCHPAD
} NueAndroidInputKind;

typedef enum NueAndroidImportState {
    NUE_ANDROID_IMPORT_IDLE = 0,
    NUE_ANDROID_IMPORT_SCANNING,
    NUE_ANDROID_IMPORT_COPYING,
    NUE_ANDROID_IMPORT_VERIFYING,
    NUE_ANDROID_IMPORT_DONE,
    NUE_ANDROID_IMPORT_CANCELLED,
    NUE_ANDROID_IMPORT_FAILED
} NueAndroidImportState;

typedef struct NueAndroidEvent {
    uint32_t type; /* NueAndroidEventType */
    int32_t i[4];
    int64_t l[2];
    float f[2];
} NueAndroidEvent;

/* Returns 1 and fills *out if an event was pending, 0 otherwise. Never blocks. */
NUE_ANDROID_EXPORT int nue_android_poll_event(NueAndroidEvent *out);
/* Number of events dropped because the queue was full (diagnostics). */
NUE_ANDROID_EXPORT uint32_t nue_android_dropped_events(void);

/* -------------------------------------------------------------- device info */

typedef struct NueAndroidDeviceInfo {
    uint32_t struct_size;
    int32_t sdk_int;               /* Build.VERSION.SDK_INT */
    int32_t is_64bit;
    char abi[16];                  /* primary ABI, e.g. "arm64-v8a" */
    char manufacturer[32];
    char model[48];
    char soc[48];                  /* Build.SOC_MODEL (API 31+) or Build.HARDWARE */
    uint64_t total_ram_bytes;
    int32_t memory_class_mb;       /* ActivityManager.getMemoryClass */
    int32_t large_memory_class_mb; /* ActivityManager.getLargeMemoryClass */
    int32_t low_ram_device;        /* ActivityManager.isLowRamDevice */
    int32_t cpu_cores;
    int32_t page_size;             /* 4096 or 16384 */
    int32_t has_touchscreen;
    int32_t has_gamepad_connected;
    int32_t is_tv;                 /* UI_MODE_TYPE_TELEVISION */
    int32_t is_chromeos;
    int32_t is_emulator;
} NueAndroidDeviceInfo;
NUE_ANDROID_EXPORT int nue_android_get_device_info(NueAndroidDeviceInfo *out);

typedef struct NueAndroidDisplayInfo {
    uint32_t struct_size;
    int32_t width, height;         /* current mode in pixels */
    int32_t rotation;              /* 0..3 */
    float refresh_hz;
    float dpi;
    float supported_refresh[16];   /* distinct refresh rates of the current resolution, ascending */
    int32_t supported_refresh_count;
    int32_t safe_left, safe_top, safe_right, safe_bottom; /* pixels */
    int32_t has_cutout;
    int32_t hdr;
    int32_t wide_color;
} NueAndroidDisplayInfo;
NUE_ANDROID_EXPORT int nue_android_get_display_info(NueAndroidDisplayInfo *out);

/* Texture / format capabilities as seen by the GPU probe. */
typedef struct NueAndroidGpuInfo {
    uint32_t struct_size;
    int32_t probed;                /* 0 until nue_android_probe_gpu() finished */
    int32_t vulkan_present;        /* a Vulkan loader and at least one physical device exist */
    uint32_t vulkan_api_version;   /* VK_MAKE_API_VERSION of the device, 0 if none */
    uint32_t vulkan_driver_version;
    uint32_t vendor_id;            /* PCI-style vendor id from Vulkan (0x5143 Qualcomm, 0x13B5 ARM, ...) */
    int32_t vendor;                /* NueAndroidGpuVendor */
    char device_name[96];
    uint64_t device_local_bytes;   /* Vulkan device-local heap size, 0 if unknown */
    int32_t gles_major, gles_minor; /* 0 if no ES context could be made */
    char gles_renderer[96];
    char gles_version[96];
    /* Capability bits (NUE_ANDROID_TEX_*), vulkan and gles ORed according to the preferred API */
    uint32_t vk_tex_caps;
    uint32_t gles_tex_caps;
    int32_t max_texture_size;
    int32_t max_msaa;
    int32_t prefer_vulkan;         /* engine recommendation: 1 Vulkan, 0 OpenGL ES */
    char note[160];                /* why the recommendation was made */
} NueAndroidGpuInfo;

typedef enum NueAndroidGpuVendor {
    NUE_ANDROID_GPU_UNKNOWN = 0,
    NUE_ANDROID_GPU_ADRENO,
    NUE_ANDROID_GPU_MALI,
    NUE_ANDROID_GPU_POWERVR,
    NUE_ANDROID_GPU_XCLIPSE,
    NUE_ANDROID_GPU_NVIDIA,
    NUE_ANDROID_GPU_AMD,
    NUE_ANDROID_GPU_INTEL,
    NUE_ANDROID_GPU_SOFTWARE
} NueAndroidGpuVendor;

#define NUE_ANDROID_TEX_BC1_3 (1u << 0)   /* DXT1/3/5 */
#define NUE_ANDROID_TEX_BC4_5 (1u << 1)
#define NUE_ANDROID_TEX_BC6H_7 (1u << 2)
#define NUE_ANDROID_TEX_ETC2 (1u << 3)
#define NUE_ANDROID_TEX_ASTC_LDR (1u << 4)
#define NUE_ANDROID_TEX_D24S8 (1u << 5)
#define NUE_ANDROID_TEX_D32S8 (1u << 6)
#define NUE_ANDROID_TEX_BGRA8 (1u << 7)
#define NUE_ANDROID_TEX_ANISO (1u << 8)

/* Starts the probe on a worker thread (idempotent). Returns immediately. */
NUE_ANDROID_EXPORT void nue_android_probe_gpu(void);
/* Returns 1 when the probe finished. */
NUE_ANDROID_EXPORT int nue_android_get_gpu_info(NueAndroidGpuInfo *out);

typedef enum NueAndroidDxtStrategy {
    NUE_ANDROID_DXT_NATIVE_BC = 0,   /* upload DXT blocks as they are */
    NUE_ANDROID_DXT_TRANSCODE_ETC2,  /* re-encode blocks to ETC2 (same size) */
    NUE_ANDROID_DXT_TRANSCODE_ASTC,  /* re-encode blocks to ASTC 4x4 (same size) */
    NUE_ANDROID_DXT_DECODE_RGBA8     /* decode on the CPU, 4-8x the memory */
} NueAndroidDxtStrategy;

/* Recommendation for the engine's DXT conversion policy given the probe. */
NUE_ANDROID_EXPORT int nue_android_recommend_dxt(NueAndroidDxtStrategy *out_strategy);

/* ------------------------------------------------------------ memory budget */

typedef struct NueAndroidMemoryBudget {
    uint32_t struct_size;
    uint64_t process_hard_limit;   /* never exceed (default cap 2 GiB) */
    uint64_t process_soft_limit;   /* start shedding caches above this */
    uint64_t texture_budget;       /* GPU-visible texture bytes the engine may keep */
    uint64_t rss_bytes;            /* resident set size now (/proc/self/statm) */
    uint64_t system_available;     /* ActivityManager.MemoryInfo.availMem at last refresh */
    int32_t pressure;              /* NueAndroidMemPressure */
    int32_t bytes_to_release;      /* MiB to give back to reach the soft limit, 0 if none */
} NueAndroidMemoryBudget;
NUE_ANDROID_EXPORT int nue_android_get_memory_budget(NueAndroidMemoryBudget *out);
NUE_ANDROID_EXPORT void nue_android_set_memory_cap(uint64_t bytes);

/* -------------------------------------------------------------- window/gfx */

/* ANativeWindow* of the current surface, or NULL. The reference is owned by the
 * activity; call before every bgfx::init / reset and again after SURFACE_CREATED. */
NUE_ANDROID_EXPORT void *nue_android_get_native_window(void);
/* Ask the compositor for a fixed frame rate (Surface.setFrameRate, API 30+); 0 clears. */
NUE_ANDROID_EXPORT void nue_android_set_frame_rate(float fps);
NUE_ANDROID_EXPORT void nue_android_set_keep_screen_on(int on);
NUE_ANDROID_EXPORT void nue_android_set_immersive(int on);
/* Picks the display mode that fits target_fps best (same resolution) and applies it. */
NUE_ANDROID_EXPORT void nue_android_set_preferred_refresh(float target_fps);

/* ----------------------------------------------------------- power / game  */

typedef struct NueAndroidPowerInfo {
    uint32_t struct_size;
    int32_t thermal_level;         /* NueAndroidThermalLevel */
    int32_t thermal_status;        /* PowerManager.THERMAL_STATUS_*, -1 unknown */
    float thermal_headroom;        /* 1.0 = severe throttling, NaN unknown */
    int32_t power_save;
    int32_t game_mode;             /* 0 unsupported, 1 standard, 2 performance, 3 battery */
    int32_t sustained_perf_supported;
    int32_t adpf_available;
    int32_t battery_percent;       /* -1 unknown */
    int32_t charging;
    int32_t suggested_fps_cap;     /* 0 = none; derived from the thermal governor */
    float suggested_quality;       /* 0..1 resolution/quality scale hint */
} NueAndroidPowerInfo;
NUE_ANDROID_EXPORT int nue_android_get_power_info(NueAndroidPowerInfo *out);

/* ADPF: register the threads that do the frame's work; target is the frame budget. */
NUE_ANDROID_EXPORT int nue_android_hint_session_start(const int32_t *tids, size_t count, int64_t target_ns);
NUE_ANDROID_EXPORT void nue_android_hint_update_target(int64_t target_ns);
NUE_ANDROID_EXPORT void nue_android_hint_report(int64_t actual_ns);
NUE_ANDROID_EXPORT void nue_android_hint_session_stop(void);
/* GameState API (API 33+): is_loading = 1 while loading screens are shown. */
NUE_ANDROID_EXPORT void nue_android_set_game_state(int is_loading, int game_state_mode);
NUE_ANDROID_EXPORT void nue_android_set_sustained_performance(int on);

/* ------------------------------------------------------------------ audio  */

typedef struct NueAndroidAudioInfo {
    uint32_t struct_size;
    int32_t native_sample_rate;    /* AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE */
    int32_t frames_per_buffer;     /* PROPERTY_OUTPUT_FRAMES_PER_BUFFER */
    int32_t low_latency;           /* FEATURE_AUDIO_LOW_LATENCY */
    int32_t pro;                   /* FEATURE_AUDIO_PRO */
    int32_t route;                 /* NueAndroidAudioRoute */
    int32_t route_high_latency;
    /* recommendation for a stream that wants `desired_rate` */
    int32_t plan_sample_rate;
    int32_t plan_buffer_frames;
    int32_t plan_needs_resample;
} NueAndroidAudioInfo;
NUE_ANDROID_EXPORT int nue_android_get_audio_info(int desired_rate, int target_latency_ms, NueAndroidAudioInfo *out);
NUE_ANDROID_EXPORT void nue_android_audio_focus(int request);

/* ------------------------------------------------------------------ input  */

NUE_ANDROID_EXPORT int nue_android_last_input_kind(void);
/* Vibrates the device (not a gamepad); amplitude 1..255 or -1 for default. */
NUE_ANDROID_EXPORT void nue_android_vibrate(int ms, int amplitude);

/* ----------------------------------------------------------------- storage */

typedef struct NueAndroidStoragePaths {
    uint32_t struct_size;
    char files_dir[512];           /* Context.getFilesDir()  (internal, private) */
    char external_files_dir[512];  /* Context.getExternalFilesDir(null) (may be empty) */
    char cache_dir[512];
    char game_dir[512];            /* where the game data lives once imported (files_dir/gamedata) */
    char saves_dir[512];
    char config_dir[512];
    char crash_dir[512];
    uint64_t internal_free_bytes;
    uint64_t external_free_bytes;
    int32_t game_data_complete;    /* import marker present and consistent */
} NueAndroidStoragePaths;
NUE_ANDROID_EXPORT int nue_android_get_storage_paths(NueAndroidStoragePaths *out);

/* Opens the system folder picker (ACTION_OPEN_DOCUMENT_TREE). Result arrives as
 * NUE_ANDROID_EVENT_TREE_PICKED. */
NUE_ANDROID_EXPORT void nue_android_pick_game_folder(void);
/* Starts importing the picked tree; `manifest` (text, may be NULL) lists the required
 * files (see docs/PLATFORM_ANDROID.md), `use_external` selects externalFilesDir as target. */
NUE_ANDROID_EXPORT int nue_android_import_start(const char *manifest, int use_external);
NUE_ANDROID_EXPORT void nue_android_import_cancel(void);
/* Import of an already accessible directory (drop folder in Android/data/<pkg>/files). */
NUE_ANDROID_EXPORT int nue_android_import_from_directory(const char *dir, const char *manifest, int use_external);

/* ------------------------------------------------------------- diagnostics */

NUE_ANDROID_EXPORT void nue_android_log(int priority, const char *tag, const char *message);
/* Installs signal handlers writing crash_dir/crash-<time>.txt then chaining to debuggerd. */
NUE_ANDROID_EXPORT void nue_android_install_crash_handler(const char *crash_dir, const char *app_version);
NUE_ANDROID_EXPORT void nue_android_trace_begin(const char *name);
NUE_ANDROID_EXPORT void nue_android_trace_end(void);
NUE_ANDROID_EXPORT void nue_android_trace_counter(const char *name, int64_t value);
/* Prints device / display / GPU / memory / audio info to logcat (tag "NUE"). */
NUE_ANDROID_EXPORT void nue_android_dump_capabilities(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NUE_ANDROID_H */
