/*
 * NUE engine - INTERNAL platform operations interface (not installed, not part of the ABI).
 *
 * This is the contract between the core and the per-OS folders src/platform/{windows,linux,android,ios}. Each platform session
 * implements ONE function, nue_platform_<os>_get_ops(), returning a static nue_platform_ops table. Every entry is optional: a NULL
 * entry (or NUE_ERR_UNSUPPORTED from it) makes the core fall back to a portable default (SDL3 where it covers the feature, otherwise
 * "unsupported" or a conservative guess) and the matching capability bit stays clear. Implement what the OS really offers; do not fake.
 *
 * Selection: CMake defines NUE_HAS_PLATFORM_<OS> (WINDOWS/LINUX/ANDROID/IOS/MACOS) when src/platform/<os>/CMakeLists.txt exists and adds
 * its sources to the target `nue_platform`; src/core/platform_select.cpp then calls that function. Without it the generic table
 * (src/core/platform_generic.cpp) is used, so the core builds and the tests run on any host with only the null backend.
 *
 * Calling context: init/shutdown/pump on the event thread. Everything else on any thread unless stated; the platform layer does its
 * own locking. Never call back into public nue_* functions that take the engine lock from inside crash_* entries.
 * Platform layers may also publish extensions (named function tables, nue_internal_register_extension) for OS-only features
 * that are not part of the portable API (Win32 message hook, Android SAF picker, iOS Documents helpers...).
 */
#ifndef NUE_INTERNAL_PLATFORM_OPS_H
#define NUE_INTERNAL_PLATFORM_OPS_H

#include "nue/nue.h"
#include "internal/backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ crash handling */

/* Called by the platform handler inside the crash context: the core writes report.txt (reason, version, system info, notes,
 * breadcrumbs, log tail) into a new directory under the report root and returns its native path (static storage, NUL terminated).
 * The platform then adds crash.dmp (or a stack dump) to that directory and calls crash_end. Returns NULL if nothing could be written. */
typedef struct nue_crash_sink {
    uint32_t struct_size;
    const char* (*begin)(const nue_crash_info* info, void* userdata);
    nue_bool (*end)(const nue_crash_info* info, void* userdata);   /* runs the user callback; returns NUE_TRUE if the process should now terminate */
    void* userdata;
    nue_crash_flags flags;
} nue_crash_sink;

/* ------------------------------------------------------------------------------------------------ import source */

typedef struct nue_import_entry {
    uint32_t struct_size;
    char path[512];                         /* relative to the import source root, '/' separated, UTF-8 */
    uint64_t size;
} nue_import_entry;

typedef struct nue_import_source_ops {
    /* Open a source described by (kind, source string). `out_source` is private to the platform. */
    nue_result (*open)(nue_import_source_kind kind, const char* source, void** out_source);
    /* Next file entry (directories are not reported). NUE_ERR_END_OF_DATA when finished. */
    nue_result (*next)(void* source, nue_import_entry* out_entry);
    /* Read bytes of the CURRENT entry sequentially; *out_read = 0 at end of entry. */
    nue_result (*read)(void* source, void* buffer, size_t capacity, size_t* out_read);
    void (*close)(void* source);
    /* Ask the user to choose a folder (kind PICKER). Completes asynchronously: done(uri) with a string `open` accepts as NUE_IMPORT_SOURCE_URI. */
    nue_result (*pick_folder)(void (*done)(void* userdata, nue_result result, const char* uri), void* userdata);
} nue_import_source_ops;

/* ------------------------------------------------------------------------------------------------ the table */

typedef struct nue_platform_ops {
    uint32_t struct_size;
    const char* name;                       /* "windows", "linux", "android", "ios", "generic" */
    uint64_t capabilities[2];               /* bit i = nue_capability i that this layer provides (OR-ed with the backend's) */

    nue_result (*init)(const nue_backend_init* init);      /* after the backend is up */
    void (*shutdown)(void);                                /* before the backend shuts down; MUST release every OS-wide state it changed */
    void (*pump)(void);                                    /* each nue_pump_events: poll thermal/power/game mode and push events on change */

    /* ---- system -------------------------------------------------------------------------- */
    nue_result (*system_info)(nue_system_info* out);       /* fill everything you know; the core pre-fills pointer size/arch/headless fields */
    nue_result (*memory_info)(uint64_t* total, uint64_t* available);
    nue_result (*system_locale)(char* buffer, size_t capacity);

    /* ---- storage ------------------------------------------------------------------------- */
    /* Native directory for a root kind following the OS rules (see storage.h). `desc` carries app_id/org/overrides; the core creates it. */
    nue_result (*storage_root)(nue_root_kind kind, const nue_init_desc* desc, char* buffer, size_t capacity, size_t* out_size);
    const nue_import_source_ops* import;

    /* ---- power, thermal, game mode ----------------------------------------------------------- */
    nue_result (*power_info)(nue_power_info* out);
    nue_result (*power_apply)(const nue_power_request* request, nue_power_applied* out_applied);   /* full wanted state; see power.h */
    nue_result (*timer_resolution_get)(uint32_t* out_us);

    /* ---- threads ------------------------------------------------------------------------- */
    nue_result (*thread_set_priority)(const nue_native_thread_ref* thread, nue_thread_priority priority);   /* thread->handle/id both 0 = calling thread */
    nue_result (*thread_get_priority)(const nue_native_thread_ref* thread, nue_thread_priority* out);
    nue_result (*thread_set_affinity)(const nue_native_thread_ref* thread, uint64_t mask);
    nue_result (*thread_set_core_class)(const nue_native_thread_ref* thread, nue_cpu_preference preference);
    nue_result (*thread_set_name)(const char* name);                    /* calling thread */
    nue_result (*perf_hint_create)(const nue_native_thread_ref* threads, uint32_t count, int64_t target_ns, void** out_impl);
    void (*perf_hint_destroy)(void* impl);
    nue_result (*perf_hint_update_target)(void* impl, int64_t target_ns);
    nue_result (*perf_hint_report)(void* impl, int64_t actual_ns);
    /* Sleep until the deadline (monotonic ns) with the best OS primitive (Windows: high resolution waitable timer). Return
     * NUE_ERR_UNSUPPORTED to use the core's loop. */
    nue_result (*sleep_until)(int64_t deadline_ns);

    /* ---- display extras ------------------------------------------------------------------ */
    nue_result (*window_safe_area)(const nue_native_window* window, nue_rect_i* out_rect);   /* overrides the backend's value when it succeeds */
    nue_result (*window_set_gpu_preference)(nue_gpu_preference preference);

    /* ---- input extras -------------------------------------------------------------------- */
    nue_bool (*haptics_available)(void);
    nue_result (*haptics_vibrate)(uint32_t duration_ms, float intensity);

    /* ---- diagnostics --------------------------------------------------------------------- */
    nue_result (*crash_install)(const nue_crash_desc* desc, const nue_crash_sink* sink);   /* chain to the previous handler */
    void (*crash_uninstall)(void);
    /* Write the platform-specific dump for a NON-fatal report (diag_write_report): minidump of the live process, or a stack text. */
    nue_result (*crash_write_dump)(const char* directory, nue_crash_flags flags);
    /* Release pointer/cursor capture and show the pointer: called first thing in a crash. Must be async-signal-safe. */
    void (*crash_release_input)(void);

    /* ---- logging ------------------------------------------------------------------------- */
    /* Debugger output / logcat / os_log / syslog. Called under the log lock; keep it fast and do not log. */
    void (*log_write)(nue_log_level level, const char* tag, const char* message);

    /* ---- appended in 0.1.1 (a platform table compiled against 0.1.0 has these as zero via struct_size; always check struct_size before reading) ----- */
    /* Called FIRST, before storage roots, settings and the backend exist: set environment variables, SDL hints and process-wide policy that must
     * precede SDL_Init (audio category, video driver choice, gamescope/Wayland policy). storage_root must also work at this point. */
    nue_result (*pre_init)(const nue_backend_init* init);
    /* NUE_TRUE: this layer pushes the NUE_EVENT_APP_* lifecycle events itself (Android Activity callbacks, iOS UIApplicationDelegate) and the
     * backend must NOT derive app state from its own window/focus events. */
    nue_bool owns_lifecycle;
    /* Process / GPU memory (limit, used, VRAM). The core has already filled physical_* from memory_info; fill what you know, leave the rest. */
    nue_result (*memory_info_ex)(nue_memory_info* inout);
    /* GPU format support and limits without a device (DXGI, Vulkan physical device, EGL, Metal). Unknown families stay 0. Set probed = NUE_TRUE only for real answers. */
    nue_result (*gpu_probe)(nue_gpu_caps* out);
    /* Refresh-rate / frame-limit cooperation with the compositor (gamescope, Android frame rate API, ProMotion). 0 releases. */
    nue_result (*window_request_refresh)(const nue_native_window* window, float refresh_hz, float frame_limit_fps);
    /* On-screen keyboard (Steam Deck/gamescope, TV UIs). */
    nue_result (*screen_keyboard)(const nue_native_window* window, nue_bool show);
} nue_platform_ops;

/* Implemented by the platform sessions (one of these exists per OS): */
const nue_platform_ops* nue_platform_windows_get_ops(void);
const nue_platform_ops* nue_platform_linux_get_ops(void);
const nue_platform_ops* nue_platform_android_get_ops(void);
const nue_platform_ops* nue_platform_ios_get_ops(void);
const nue_platform_ops* nue_platform_macos_get_ops(void);
/* Implemented by the core: all-NULL table with name "generic". */
const nue_platform_ops* nue_platform_generic_get_ops(void);
/* The table in use (never NULL). */
const nue_platform_ops* nue_platform_get_ops(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_INTERNAL_PLATFORM_OPS_H */
