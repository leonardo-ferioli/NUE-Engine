/*
 * NUE engine - power, thermal state and "game mode" integration, plus thread priority/affinity helpers.
 *
 * One request object (nue_power_request) describes everything the game wants from the OS scheduler and power manager; the
 * platform layer applies what it can and reports, field by field, what was honoured (nue_power_applied). Nothing here is
 * required for correctness: every call may answer NUE_OK_NOOP / NUE_ERR_UNSUPPORTED and the game runs the same.
 *
 *   Windows  Game Mode (GameMode API hints / process priority class capped), timer resolution (NtSetTimerResolution /
 *            timeBeginPeriod, process-wide timer request on Win11), power throttling opt-out, GPU preference (per-app
 *            GPU preference), SetThreadExecutionState, MMCSS ("Games" task) for the audio/render threads.
 *   Linux    Feral GameMode (libgamemode via dlopen), power-profiles-daemon / systemd inhibit for keep-awake, sched policy.
 *   Android  GameManager (game mode query, GAME_STATE), ADPF PerformanceHintManager (nue_perf_hint), thermal API, keep-screen-on.
 *   iOS      Game Mode (queried only: the OS decides), ProcessInfo thermalState / lowPowerMode, idle timer disabled, QoS classes.
 *
 * Thread: [any] unless noted. Events: NUE_EVENT_POWER_CHANGED, THERMAL_CHANGED, LOW_POWER_MODE_CHANGED, GAME_MODE_CHANGED.
 */
#ifndef NUE_POWER_H
#define NUE_POWER_H

#include "nue/base.h"
#include "nue/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== state queries */

typedef enum nue_thermal_state {
    NUE_THERMAL_UNKNOWN = 0,
    NUE_THERMAL_NOMINAL,
    NUE_THERMAL_FAIR,                       /* slightly elevated: avoid extra work */
    NUE_THERMAL_SERIOUS,                    /* throttling imminent/active: reduce quality/frame rate */
    NUE_THERMAL_CRITICAL                    /* shed load now */
} nue_thermal_state;

typedef enum nue_power_state {
    NUE_POWER_UNKNOWN = 0, NUE_POWER_ON_BATTERY, NUE_POWER_NO_BATTERY, NUE_POWER_CHARGING, NUE_POWER_CHARGED
} nue_power_state;

typedef enum nue_game_mode_state { NUE_GAME_MODE_UNKNOWN = 0, NUE_GAME_MODE_OFF, NUE_GAME_MODE_ON, NUE_GAME_MODE_PERFORMANCE, NUE_GAME_MODE_BATTERY } nue_game_mode_state;

typedef struct nue_power_info {
    uint32_t struct_size;
    nue_power_state state;
    int32_t battery_percent;                /* -1 unknown */
    int32_t seconds_left;                   /* -1 unknown */
    nue_bool on_ac_power;
    nue_bool low_power_mode;                /* OS battery saver / low power mode is on */
    nue_thermal_state thermal;
    nue_game_mode_state game_mode;          /* OS-level Game Mode as seen by this app */
    float thermal_headroom;                 /* Android: forecast 0..1+ (1 = throttling threshold); < 0 unknown */
    nue_bool display_keep_awake_active;     /* our own keep-awake request is in force */
    uint32_t reserved[6];
} nue_power_info;

NUE_API nue_result NUE_CALL nue_power_get_info(nue_power_info* out);
NUE_API nue_thermal_state NUE_CALL nue_power_get_thermal_state(void);
NUE_API const char* NUE_CALL nue_thermal_state_name(nue_thermal_state state);

/* ============================================================================================== requests */

typedef enum nue_performance_profile {
    NUE_PERF_DEFAULT = 0,                   /* do not request anything */
    NUE_PERF_BALANCED,
    NUE_PERF_HIGH,                          /* maximum throughput for short sessions (Game Mode ON, high frequency, high priority class) */
    NUE_PERF_SUSTAINED,                     /* stable clocks for long sessions (avoid thermal throttling cliffs) */
    NUE_PERF_POWER_SAVER                    /* lowest power: lower clocks, allow efficiency cores */
} nue_performance_profile;

typedef enum nue_game_hint {
    NUE_GAME_HINT_NONE = 0,
    NUE_GAME_HINT_GAMEPLAY,                 /* actively playing: Game Mode on */
    NUE_GAME_HINT_MENU,                     /* in menus/loading: lower priority, allow dimming policies */
    NUE_GAME_HINT_LOADING,                  /* burst of I/O+CPU: boost */
    NUE_GAME_HINT_PAUSED                    /* paused/background */
} nue_game_hint;

typedef enum nue_gpu_preference { NUE_GPU_DEFAULT = 0, NUE_GPU_HIGH_PERFORMANCE, NUE_GPU_LOW_POWER } nue_gpu_preference;
typedef enum nue_cpu_preference { NUE_CPU_DEFAULT = 0, NUE_CPU_PERFORMANCE_CORES, NUE_CPU_EFFICIENCY_CORES } nue_cpu_preference;

typedef uint32_t nue_keep_awake;
enum { NUE_KEEP_AWAKE_NONE = 0, NUE_KEEP_AWAKE_DISPLAY = 1u << 0, NUE_KEEP_AWAKE_SYSTEM = 1u << 1 };

typedef struct nue_power_request {
    uint32_t struct_size;
    nue_performance_profile profile;
    nue_game_hint game_hint;
    nue_keep_awake keep_awake;              /* display on / no system sleep */
    nue_gpu_preference gpu;                 /* hybrid-GPU laptops: takes effect at the next device creation, set it BEFORE nue_gfx_create */
    nue_cpu_preference cpu;
    uint32_t timer_resolution_us;           /* 0 = leave alone, otherwise request this system timer period (Windows: 500..15600); released when set back to 0 */
    uint32_t target_frame_time_us;          /* hint for ADPF/QoS governors (e.g. 16666), 0 = unknown */
    nue_bool disable_power_throttling;      /* Windows: opt out of EcoQoS/power throttling for the process */
    nue_bool prefer_low_latency_audio;      /* Android: request the performance mode; others: raise audio thread class */
    uint32_t reserved[6];
} nue_power_request;

typedef uint32_t nue_power_applied;     /* bit mask: which parts of the request the platform honoured */
enum {
    NUE_POWER_APPLIED_PROFILE = 1u << 0, NUE_POWER_APPLIED_GAME_HINT = 1u << 1, NUE_POWER_APPLIED_KEEP_AWAKE = 1u << 2,
    NUE_POWER_APPLIED_GPU = 1u << 3, NUE_POWER_APPLIED_CPU = 1u << 4, NUE_POWER_APPLIED_TIMER = 1u << 5,
    NUE_POWER_APPLIED_FRAME_TIME = 1u << 6, NUE_POWER_APPLIED_THROTTLING = 1u << 7, NUE_POWER_APPLIED_AUDIO = 1u << 8
};

/* Replace the active request with this one (state-based: pass the full wanted state; an all-zero request releases everything).
 * *out_applied may be NULL. Returns NUE_OK_PARTIAL when only some fields were honoured. Everything is released by nue_shutdown
 * and on crash exit where the OS does not do it itself. */
NUE_API nue_result NUE_CALL nue_power_apply(const nue_power_request* request, nue_power_applied* out_applied);
NUE_API nue_result NUE_CALL nue_power_get_request(nue_power_request* out);
/* Convenience for the most common one. */
NUE_API nue_result NUE_CALL nue_power_set_keep_awake(nue_keep_awake flags);
NUE_API nue_result NUE_CALL nue_power_set_timer_resolution(uint32_t microseconds);   /* 0 releases; returns the period actually granted via nue_power_get_timer_resolution */
NUE_API uint32_t NUE_CALL nue_power_get_timer_resolution(void);                      /* current system timer period in us, 0 = unknown */

/* ============================================================================================== thread helpers */

/* Priority and affinity of threads. thread == NULL means the calling thread. The recompiled game's own native threads are
 * addressed with the native variants (HANDLE/pthread_t as integer, or the native thread id). Priorities are mapped to the
 * platform's scale and capped for safety on Windows (no TIME_CRITICAL; REALTIME maps to the MMCSS "Pro Audio"/"Games" class
 * where available and to ABOVE_NORMAL/HIGHEST otherwise). */
NUE_API nue_result NUE_CALL nue_thread_set_priority(nue_thread thread, nue_thread_priority priority);
NUE_API nue_result NUE_CALL nue_thread_get_priority(nue_thread thread, nue_thread_priority* out);
NUE_API nue_result NUE_CALL nue_thread_set_affinity(nue_thread thread, uint64_t logical_cpu_mask);   /* 0 = all */
NUE_API nue_result NUE_CALL nue_thread_set_core_class(nue_thread thread, nue_cpu_preference preference);   /* performance/efficiency cores (hybrid CPUs, big.LITTLE) */
typedef struct nue_native_thread_ref {
    uint32_t struct_size;
    uint64_t handle;                        /* HANDLE (Windows), pthread_t (POSIX); 0 = unused */
    uint64_t id;                            /* DWORD thread id (Windows), tid (Linux/Android/iOS mach thread); 0 = unused */
} nue_native_thread_ref;
NUE_API nue_result NUE_CALL nue_native_thread_set_priority(const nue_native_thread_ref* thread, nue_thread_priority priority);
NUE_API nue_result NUE_CALL nue_native_thread_set_affinity(const nue_native_thread_ref* thread, uint64_t logical_cpu_mask);
NUE_API nue_result NUE_CALL nue_native_thread_set_core_class(const nue_native_thread_ref* thread, nue_cpu_preference preference);

/* Performance hint session (Android ADPF; Windows/iOS: thread class bookkeeping only): tell the scheduler the work duration you
 * target per frame and the real one, so it can pick core and frequency. Create one for the game/render/audio threads. */
NUE_DEFINE_HANDLE(nue_perf_hint);
NUE_API nue_result NUE_CALL nue_perf_hint_create(const nue_native_thread_ref* threads, uint32_t thread_count, int64_t target_work_duration_ns, nue_perf_hint* out);
NUE_API void NUE_CALL nue_perf_hint_destroy(nue_perf_hint hint);
NUE_API nue_result NUE_CALL nue_perf_hint_update_target(nue_perf_hint hint, int64_t target_work_duration_ns);
NUE_API nue_result NUE_CALL nue_perf_hint_report(nue_perf_hint hint, int64_t actual_work_duration_ns);   /* call once per frame; cheap */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_POWER_H */
