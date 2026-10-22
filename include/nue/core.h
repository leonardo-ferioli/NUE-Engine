/*
 * NUE engine - core: init/shutdown, version and build info, error text, logging, configuration store,
 * allocation, hashing, threads and synchronisation, clocks and precise sleep, extension lookup.
 */
#ifndef NUE_CORE_H
#define NUE_CORE_H

#include "nue/base.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== init / shutdown */

/* Subsystems. nue_init() starts the ones requested (0 = NUE_SUBSYSTEM_ALL_DEFAULT). Core, log, config, time, threads,
 * storage (VFS) and diagnostics are always available. */
typedef uint32_t nue_subsystem_mask;
enum {
    NUE_SUBSYSTEM_EVENTS  = 1u << 0,   /* event queue and app lifecycle */
    NUE_SUBSYSTEM_VIDEO   = 1u << 1,   /* windows and displays */
    NUE_SUBSYSTEM_INPUT   = 1u << 2,
    NUE_SUBSYSTEM_AUDIO   = 1u << 3,   /* devices, streams and the software mixer */
    NUE_SUBSYSTEM_POWER   = 1u << 4,   /* power, thermal, game mode */
    NUE_SUBSYSTEM_GFX     = 1u << 5    /* graphics context (bgfx / Direct3D 9 layer) */
};
#define NUE_SUBSYSTEM_ALL_DEFAULT 0xFFFFFFFFu

typedef enum nue_backend_kind {
    NUE_BACKEND_AUTO = 0,   /* SDL3 when compiled in, otherwise null */
    NUE_BACKEND_NULL,       /* headless: no window, no audio device, scripted input; always compiled in; used by tests */
    NUE_BACKEND_SDL3        /* SDL3 (with SDL's own drivers; the "dummy"/"offscreen" video and "dummy"/"disk" audio drivers allow headless runs) */
} nue_backend_kind;

typedef enum nue_loop_model {
    NUE_LOOP_HOST = 0,      /* the caller owns the main loop and calls nue_pump_events()/nue_poll_event() itself (desktop, recompiled games) */
    NUE_LOOP_ENGINE         /* the engine owns the loop through nue_app_run() (mobile; also fine on desktop) */
} nue_loop_model;

typedef struct nue_init_desc {
    uint32_t struct_size;
    uint32_t api_version;               /* NUE_API_VERSION the caller compiled against; major must match, minor <= library minor */
    const char* app_name;               /* display name, e.g. "NFS Underground 2" */
    const char* app_id;                 /* short id used for folders: "nfsu2" (letters, digits, '-', '_') */
    const char* org_name;               /* "NUE" - used in per-OS user data paths */
    const char* app_version;            /* free text, shown in crash reports */
    nue_backend_kind backend;
    nue_loop_model loop_model;
    nue_subsystem_mask subsystems;
    uint32_t flags;                     /* NUE_INIT_* */
    const nue_allocator* allocator;     /* may be NULL; copied */
    const char* game_data_path;         /* directory mounted read-only as root "data" (NULL: platform default / import location) */
    const char* user_data_path;         /* overrides the per-OS writable user data folder (NULL: platform rule) */
    const char* config_file;            /* VFS path of the settings file loaded at start and saved by nue_config_save(NULL) ("settings:nue.ini" if NULL) */
    const char* log_file;               /* NULL: "user:logs/<app_id>.log" (rotated); "" disables the file sink */
    uint32_t log_level;                 /* nue_log_level; 0 = INFO */
    uint32_t event_queue_capacity;      /* 0 = 4096 events */
    uint32_t reserved[8];
} nue_init_desc;

enum {
    NUE_INIT_HEADLESS         = 1u << 0,   /* never touch real displays/audio devices (implies SDL dummy drivers or the null backend) */
    NUE_INIT_NO_LOG_FILE      = 1u << 1,
    NUE_INIT_NO_CRASH_HANDLER = 1u << 2,   /* do not install the crash reporter at init (diag.h) */
    NUE_INIT_NO_CONFIG_LOAD   = 1u << 3,
    NUE_INIT_DETERMINISTIC    = 1u << 4    /* null backend only: fixed clock stepped by nue_null_advance_time(), scripted input */
};

/* Initialise the engine. Reference counted: further calls with a compatible descriptor only increase the count and
 * ignore the descriptor. Returns NUE_ERR_VERSION_MISMATCH if api_version is incompatible.
 * Thread: [any] but the calling thread becomes the "event thread" (see app.h) unless loop_model is ENGINE. */
NUE_API nue_result NUE_CALL nue_init(const nue_init_desc* desc);
/* Release one reference; the last one stops every subsystem, closes files/windows/streams still open and frees all
 * engine memory. [any, but not from a callback of the engine] */
NUE_API void NUE_CALL nue_shutdown(void);
NUE_API nue_bool NUE_CALL nue_is_initialized(void);

/* ============================================================================================== version / build */

NUE_API uint32_t NUE_CALL nue_get_version(void);               /* NUE_MAKE_VERSION of the library; compare with NUE_API_VERSION */
NUE_API const char* NUE_CALL nue_get_version_string(void);     /* "0.1.1 (abcdef1)" */

typedef struct nue_build_info {
    uint32_t struct_size;
    uint32_t version;
    const char* version_string;
    const char* git_commit;             /* "unknown" when built outside git */
    const char* compiler;               /* "clang 21.1", "msvc 19.4" ... */
    const char* build_type;             /* "Release", "Debug" ... */
    const char* backends;               /* comma separated: "null,sdl3" */
    const char* platform_layer;         /* "windows", "linux", "android", "ios", "generic" */
    const char* sdl_version;            /* "3.2.x" or "" when SDL3 is not compiled in */
    uint32_t pointer_size;              /* 4 or 8 */
    nue_bool shared_library;
    nue_bool has_gfx;                   /* bgfx integration compiled in */
    nue_bool has_d3d9;                  /* nue_d3d9 translator linked */
    nue_bool debug_build;
    uint32_t reserved[4];
} nue_build_info;
NUE_API nue_result NUE_CALL nue_get_build_info(nue_build_info* out);   /* [any] strings are static */

/* ============================================================================================== errors */

NUE_API const char* NUE_CALL nue_result_name(nue_result r);             /* "NUE_ERR_NOT_FOUND"; static; "NUE_RESULT_UNKNOWN" for unknown codes */
/* Detail text of the last failure on the calling thread ("" if none). Valid until the next NUE call on this thread. */
NUE_API const char* NUE_CALL nue_get_last_error_message(void);
NUE_API void NUE_CALL nue_clear_last_error(void);
/* Set by backends and by hosts that want their own text attached to a failure. */
NUE_API void NUE_CALL nue_set_last_error(const char* fmt, ...) NUE_PRINTF_LIKE(1, 2);

/* ============================================================================================== logging */

typedef enum nue_log_level {
    NUE_LOG_TRACE = 1, NUE_LOG_DEBUG, NUE_LOG_INFO, NUE_LOG_WARN, NUE_LOG_ERROR, NUE_LOG_FATAL, NUE_LOG_OFF = 100
} nue_log_level;

typedef struct nue_log_record {
    uint32_t struct_size;
    nue_log_level level;
    const char* tag;                    /* subsystem, "audio", "game"; never NULL */
    const char* message;                /* formatted, no trailing newline */
    int64_t time_ns;                    /* nue_time_ns() */
    int64_t unix_time_ms;
    uint64_t thread_id;
    const char* file;                   /* may be NULL */
    int32_t line;
} nue_log_record;

typedef void (NUE_CALL *nue_log_callback)(void* userdata, const nue_log_record* record);

typedef enum nue_log_sink_kind {
    NUE_LOG_SINK_STDERR = 1,
    NUE_LOG_SINK_FILE,                  /* path (VFS or native); rotated at max_bytes keeping keep_files old files */
    NUE_LOG_SINK_PLATFORM,              /* debugger output (Windows), logcat (Android), os_log (iOS), journald/syslog (Linux) */
    NUE_LOG_SINK_CALLBACK,
    NUE_LOG_SINK_MEMORY_RING            /* keeps the last `max_bytes` of text for nue_log_get_recent() and crash reports */
} nue_log_sink_kind;

typedef struct nue_log_sink_desc {
    uint32_t struct_size;
    nue_log_sink_kind kind;
    nue_log_level min_level;
    const char* path;                   /* FILE */
    uint32_t max_bytes;                 /* FILE: rotation size (0 = 8 MiB); RING: capacity (0 = 64 KiB) */
    uint32_t keep_files;                /* FILE: number of rotated files kept (0 = 3) */
    nue_log_callback callback;          /* CALLBACK; called with the sink lock held, on the logging thread: do not log from it */
    void* userdata;
} nue_log_sink_desc;

typedef uint32_t nue_log_sink_id;       /* 0 = invalid */

/* [any] All logging functions are thread safe and may be called before nue_init (messages go to stderr/platform log). */
NUE_API void NUE_CALL nue_log_write(nue_log_level level, const char* tag, const char* file, int line, const char* fmt, ...) NUE_PRINTF_LIKE(5, 6);
NUE_API void NUE_CALL nue_log_writev(nue_log_level level, const char* tag, const char* file, int line, const char* fmt, va_list args);
NUE_API void NUE_CALL nue_log_set_level(nue_log_level level);                         /* global minimum */
NUE_API nue_log_level NUE_CALL nue_log_get_level(void);
NUE_API void NUE_CALL nue_log_set_tag_level(const char* tag, nue_log_level level);    /* per-tag override; level 0 removes the override */
NUE_API nue_result NUE_CALL nue_log_add_sink(const nue_log_sink_desc* desc, nue_log_sink_id* out_id);
NUE_API nue_result NUE_CALL nue_log_remove_sink(nue_log_sink_id id);
NUE_API void NUE_CALL nue_log_flush(void);
/* Text of the memory-ring sink(s) (NUE_STRING_OUT). Engine installs a 64 KiB ring at init. */
NUE_API nue_result NUE_CALL nue_log_get_recent(char* buffer, size_t capacity, size_t* out_size);

#define NUE_LOG(level, tag, ...) nue_log_write((level), (tag), __FILE__, __LINE__, __VA_ARGS__)
#define NUE_LOGT(tag, ...) NUE_LOG(NUE_LOG_TRACE, tag, __VA_ARGS__)
#define NUE_LOGD(tag, ...) NUE_LOG(NUE_LOG_DEBUG, tag, __VA_ARGS__)
#define NUE_LOGI(tag, ...) NUE_LOG(NUE_LOG_INFO,  tag, __VA_ARGS__)
#define NUE_LOGW(tag, ...) NUE_LOG(NUE_LOG_WARN,  tag, __VA_ARGS__)
#define NUE_LOGE(tag, ...) NUE_LOG(NUE_LOG_ERROR, tag, __VA_ARGS__)
#define NUE_LOGF(tag, ...) NUE_LOG(NUE_LOG_FATAL, tag, __VA_ARGS__)

/* ============================================================================================== configuration store */

/* A typed key/value store with dotted keys ("video.vsync", "audio.master_volume"). Stored as INI on disk:
 *   [video]            ; comments start with ';' or '#'
 *   vsync = true
 * Keys before the first section have no prefix. Values: bool (true/false/1/0/yes/no), int64, double, string
 * (optionally "quoted"; \n \t \\ \" escapes). Unknown keys are preserved on save. The file lives in the "settings" root.
 * Thread: [any]; change callbacks run on the thread that changed the value, after the lock is released. */

typedef enum nue_config_type { NUE_CONFIG_NONE = 0, NUE_CONFIG_BOOL, NUE_CONFIG_INT, NUE_CONFIG_DOUBLE, NUE_CONFIG_STRING } nue_config_type;

typedef struct nue_config_decl {            /* optional: declares a key with default and range, for validation and tools */
    uint32_t struct_size;
    const char* key;
    nue_config_type type;
    int64_t default_int;                    /* BOOL (0/1) and INT */
    double default_double;
    const char* default_string;
    double min_value, max_value;            /* INT/DOUBLE; min >= max means unbounded */
    const char* description;
} nue_config_decl;

typedef void (NUE_CALL *nue_config_callback)(void* userdata, const char* key);
typedef uint32_t nue_config_watch_id;

NUE_API nue_result NUE_CALL nue_config_declare(const nue_config_decl* decl);
NUE_API nue_bool NUE_CALL nue_config_has(const char* key);
NUE_API nue_config_type NUE_CALL nue_config_type_of(const char* key);
NUE_API nue_bool NUE_CALL nue_config_get_bool(const char* key, nue_bool default_value);
NUE_API int64_t NUE_CALL nue_config_get_int(const char* key, int64_t default_value);
NUE_API double NUE_CALL nue_config_get_double(const char* key, double default_value);
/* NUE_STRING_OUT; returns NUE_ERR_NOT_FOUND (and writes the default_value, if not NULL) when absent. */
NUE_API nue_result NUE_CALL nue_config_get_string(const char* key, const char* default_value, char* buffer, size_t capacity, size_t* out_size);
NUE_API nue_result NUE_CALL nue_config_set_bool(const char* key, nue_bool value);
NUE_API nue_result NUE_CALL nue_config_set_int(const char* key, int64_t value);
NUE_API nue_result NUE_CALL nue_config_set_double(const char* key, double value);
NUE_API nue_result NUE_CALL nue_config_set_string(const char* key, const char* value);
NUE_API nue_result NUE_CALL nue_config_remove(const char* key);
/* Load/merge a file (VFS path, e.g. "settings:nue.ini", or NULL for the init_desc.config_file). */
NUE_API nue_result NUE_CALL nue_config_load(const char* path);
NUE_API nue_result NUE_CALL nue_config_save(const char* path);          /* writes atomically (temp file + rename) */
/* Parse INI text directly (tests, embedded defaults). */
NUE_API nue_result NUE_CALL nue_config_load_text(const char* text, size_t length);
/* Watch every key starting with `prefix` ("" = all). */
NUE_API nue_result NUE_CALL nue_config_watch(const char* prefix, nue_config_callback cb, void* userdata, nue_config_watch_id* out_id);
NUE_API nue_result NUE_CALL nue_config_unwatch(nue_config_watch_id id);

/* ============================================================================================== memory */

/* [any] Allocation through the engine allocator (init_desc.allocator). Memory returned to the caller by the engine
 * (documented per function) is released with nue_free. alignment 0 = pointer size. */
NUE_API void* NUE_CALL nue_alloc(size_t size, size_t alignment);
NUE_API void* NUE_CALL nue_realloc(void* ptr, size_t new_size, size_t alignment);
NUE_API void NUE_CALL nue_free(void* ptr);

/* ============================================================================================== hashing */

typedef struct nue_sha256_state { uint32_t opaque[28]; } nue_sha256_state;   /* streaming SHA-256 */
NUE_API uint32_t NUE_CALL nue_crc32(uint32_t crc, const void* data, size_t size);         /* crc = 0 to start; continues */
NUE_API void NUE_CALL nue_sha256_begin(nue_sha256_state* state);
NUE_API void NUE_CALL nue_sha256_update(nue_sha256_state* state, const void* data, size_t size);
NUE_API void NUE_CALL nue_sha256_end(nue_sha256_state* state, uint8_t out_digest[32]);
NUE_API void NUE_CALL nue_sha256(const void* data, size_t size, uint8_t out_digest[32]);
NUE_API void NUE_CALL nue_hex_encode(const uint8_t* data, size_t size, char* out, size_t capacity);   /* lowercase, NUL terminated */

/* ============================================================================================== time */

/* [any] Monotonic nanoseconds since an unspecified start; not comparable across processes and never jumps with wall clock changes.
 * Whether suspended time counts depends on the OS clock: Windows (performance counter) counts it, Linux CLOCK_MONOTONIC does not;
 * code that must survive device suspend should watch NUE_EVENT_SYSTEM_RESUMED. All event timestamps use this domain. */
NUE_API int64_t NUE_CALL nue_time_ns(void);
NUE_API uint64_t NUE_CALL nue_time_counter(void);                  /* raw high-resolution counter (QueryPerformanceCounter-like) */
NUE_API uint64_t NUE_CALL nue_time_counter_frequency(void);        /* ticks per second */
NUE_API int64_t NUE_CALL nue_time_unix_ms(void);                   /* wall clock, ms since 1970-01-01 UTC */

typedef enum nue_sleep_mode {
    NUE_SLEEP_COARSE = 0,   /* OS sleep: cheapest; accuracy is the OS timer granularity (up to ~15 ms on Windows without timer resolution) */
    NUE_SLEEP_PRECISE       /* OS sleep for most of the interval, then yield/spin: wakes within ~100 us; costs a little CPU in the last ~1 ms */
} nue_sleep_mode;
NUE_API void NUE_CALL nue_sleep_ns(int64_t ns, nue_sleep_mode mode);
NUE_API void NUE_CALL nue_sleep_until_ns(int64_t deadline_ns, nue_sleep_mode mode);   /* deadline in nue_time_ns() domain */

/* Frame limiter: call once per frame; sleeps (precisely) until 1/target_fps has passed since the previous call and returns the time waited in ns.
 * target_fps <= 0 only records the frame. A big gap (pause, breakpoint) resets the schedule instead of bursting to catch up. [any] */
NUE_API int64_t NUE_CALL nue_frame_limiter_wait(double target_fps);
NUE_API void NUE_CALL nue_frame_limiter_reset(void);

/* ============================================================================================== threads and sync */

NUE_DEFINE_HANDLE(nue_thread);
NUE_DEFINE_HANDLE(nue_mutex);
NUE_DEFINE_HANDLE(nue_cond);
NUE_DEFINE_HANDLE(nue_semaphore);

typedef int32_t (NUE_CALL *nue_thread_fn)(void* userdata);

typedef enum nue_thread_priority {
    NUE_THREAD_PRIORITY_DEFAULT = 0,
    NUE_THREAD_PRIORITY_LOW,
    NUE_THREAD_PRIORITY_NORMAL,
    NUE_THREAD_PRIORITY_ABOVE_NORMAL,
    NUE_THREAD_PRIORITY_HIGH,
    NUE_THREAD_PRIORITY_REALTIME        /* audio-class: may be silently downgraded; never starves the system on Windows (capped) */
} nue_thread_priority;

typedef struct nue_thread_desc {
    uint32_t struct_size;
    const char* name;                   /* shown in debuggers/profilers; truncated to the OS limit */
    uint32_t stack_size;                /* bytes, 0 = default */
    nue_thread_priority priority;
    uint64_t affinity_mask;             /* logical CPUs, 0 = any */
} nue_thread_desc;

/* desc may be NULL. [any] */
NUE_API nue_result NUE_CALL nue_thread_create(const nue_thread_desc* desc, nue_thread_fn fn, void* userdata, nue_thread* out_thread);
NUE_API nue_result NUE_CALL nue_thread_join(nue_thread thread, int32_t* out_exit_code);   /* destroys the handle */
NUE_API nue_result NUE_CALL nue_thread_detach(nue_thread thread);                         /* destroys the handle */
NUE_API uint64_t NUE_CALL nue_thread_current_id(void);                                    /* stable per thread; native thread id where the OS has one */
NUE_API uint64_t NUE_CALL nue_thread_get_id(nue_thread thread);
NUE_API void NUE_CALL nue_thread_set_current_name(const char* name);
NUE_API void NUE_CALL nue_thread_yield(void);
/* Priority/affinity of existing threads: see power.h (nue_thread_set_priority & friends). */

NUE_API nue_result NUE_CALL nue_mutex_create(nue_bool recursive, nue_mutex* out_mutex);
NUE_API void NUE_CALL nue_mutex_destroy(nue_mutex mutex);
NUE_API void NUE_CALL nue_mutex_lock(nue_mutex mutex);
NUE_API nue_bool NUE_CALL nue_mutex_trylock(nue_mutex mutex);
NUE_API void NUE_CALL nue_mutex_unlock(nue_mutex mutex);

NUE_API nue_result NUE_CALL nue_cond_create(nue_cond* out_cond);
NUE_API void NUE_CALL nue_cond_destroy(nue_cond cond);
/* timeout_ns < 0 waits forever. Returns NUE_OK, or NUE_ERR_TIMEOUT. The mutex must be held (non-recursive). */
NUE_API nue_result NUE_CALL nue_cond_wait(nue_cond cond, nue_mutex mutex, int64_t timeout_ns);
NUE_API void NUE_CALL nue_cond_signal(nue_cond cond);
NUE_API void NUE_CALL nue_cond_broadcast(nue_cond cond);

NUE_API nue_result NUE_CALL nue_semaphore_create(uint32_t initial_value, nue_semaphore* out_sem);
NUE_API void NUE_CALL nue_semaphore_destroy(nue_semaphore sem);
NUE_API nue_result NUE_CALL nue_semaphore_wait(nue_semaphore sem, int64_t timeout_ns);   /* < 0 forever; 0 = try */
NUE_API void NUE_CALL nue_semaphore_post(nue_semaphore sem);

/* Thread local storage. Slots are process wide; the destructor runs at thread exit for non-NULL values. */
typedef uint32_t nue_tls_id;                                                              /* 0 = invalid */
NUE_API nue_result NUE_CALL nue_tls_create(void (NUE_CALL *destructor)(void* value), nue_tls_id* out_id);
NUE_API void NUE_CALL nue_tls_destroy(nue_tls_id id);
NUE_API void* NUE_CALL nue_tls_get(nue_tls_id id);
NUE_API nue_result NUE_CALL nue_tls_set(nue_tls_id id, void* value);

/* ============================================================================================== timers */

/* Periodic and one-shot timers with callbacks on an engine timer thread (what timeSetEvent / SetTimer style APIs need). Period and delay are in
 * nanoseconds; ticks that were missed because the callback ran long are coalesced, never replayed in a burst. In deterministic mode (null backend)
 * timers fire from nue_null_advance_time instead of a thread. The callback runs on the timer thread: keep it short, do not destroy its own timer
 * (use nue_timer_cancel). */
NUE_DEFINE_HANDLE(nue_timer);
typedef void (NUE_CALL *nue_timer_fn)(void* userdata, nue_timer timer, uint64_t tick);

typedef struct nue_timer_desc {
    uint32_t struct_size;
    int64_t period_ns;                  /* > 0; the interval for periodic timers and the delay for one-shots */
    int64_t first_delay_ns;             /* 0 = one period */
    nue_bool one_shot;
    nue_timer_fn callback;
    void* userdata;
    const char* name;                   /* thread name */
    nue_thread_priority priority;       /* timer thread priority (DEFAULT = normal) */
} nue_timer_desc;

NUE_API nue_result NUE_CALL nue_timer_create(const nue_timer_desc* desc, nue_timer* out_timer);
NUE_API nue_result NUE_CALL nue_timer_cancel(nue_timer timer);      /* no further callbacks start; a callback in flight finishes. Safe from the callback. */
NUE_API void NUE_CALL nue_timer_destroy(nue_timer timer);           /* cancels and WAITS for a callback in flight. Not from the callback itself. */
NUE_API nue_result NUE_CALL nue_timer_set_period(nue_timer timer, int64_t period_ns);

/* ============================================================================================== extensions */

/* Platform layers and optional modules publish extra functionality as named function tables that are NOT part of the
 * core ABI (e.g. "nue.windows.v1": Win32 message hook, "nue.android.v1": SAF picker). The table's first member is
 * uint32_t struct_size. Returns NULL when the extension is unavailable. Header of an extension: src/platform/<os>/include. */
NUE_API const void* NUE_CALL nue_get_extension(const char* name);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_CORE_H */
