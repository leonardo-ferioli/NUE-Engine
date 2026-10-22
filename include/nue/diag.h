/*
 * NUE engine - diagnostics: crash handling and reports, breadcrumbs, assertions, performance counters and frame statistics.
 *
 * The crash handler is a hook framework: the core owns policy (report folder, rotation, notes, log tail, callbacks) and the platform
 * layer owns the mechanism (Windows: SetUnhandledExceptionFilter + vectored handler + MiniDumpWriteDump; Linux: signal handlers with
 * an alternate stack + /proc maps; Android: signal handlers + tombstone-compatible text; iOS: signal handlers + Mach exceptions).
 * A report is a directory  user:crash/<timestamp>-<id>/  holding  report.txt  (reason, version, system info, notes, breadcrumbs, log
 * tail, thread list with stacks where the platform can) and, where supported, a minidump (crash.dmp).
 *
 * Thread: [any] unless noted. The crash callback runs inside the failing context: async-signal-safe rules apply (no allocation, no locks).
 */
#ifndef NUE_DIAG_H
#define NUE_DIAG_H

#include "nue/base.h"
#include "nue/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== crash reports */

typedef uint32_t nue_crash_flags;
enum {
    NUE_CRASH_MINIDUMP       = 1u << 0,     /* write crash.dmp (small: threads, stacks, modules) */
    NUE_CRASH_FULL_DUMP      = 1u << 1,     /* include full process memory (large) */
    NUE_CRASH_INCLUDE_LOG    = 1u << 2,     /* tail of the memory log ring */
    NUE_CRASH_NO_UI          = 1u << 3,     /* never show a dialog */
    NUE_CRASH_RELEASE_CURSOR = 1u << 4,     /* ClipCursor(NULL)/ungrab and show the pointer before anything else (the desktop must never be left captured) */
    NUE_CRASH_DEFAULT        = NUE_CRASH_MINIDUMP | NUE_CRASH_INCLUDE_LOG | NUE_CRASH_NO_UI | NUE_CRASH_RELEASE_CURSOR
};

typedef struct nue_crash_info {
    uint32_t struct_size;
    const char* reason;                     /* "access violation reading 0x00000000", "SIGSEGV", "assert: x != NULL" */
    uint64_t fault_address;
    uint64_t fault_pc;                      /* instruction pointer; for a recompiled game see nue_diag_set_guest_pc_provider */
    uint32_t signal_or_code;                /* NTSTATUS / signal number */
    const char* report_dir;                 /* native path of the directory being written */
} nue_crash_info;

/* Runs in the crash context after the standard report was written. Return NUE_TRUE to let the process terminate normally,
 * NUE_FALSE to continue the platform's default handling (e.g. let the OS crash dialog / tombstone run). */
typedef nue_bool (NUE_CALL *nue_crash_callback)(void* userdata, const nue_crash_info* info);
/* Optional: tells the report which guest (recompiled program) address was executing; read in the crash context, must be a plain load. */
typedef uint64_t (NUE_CALL *nue_guest_pc_provider)(void* userdata);

typedef struct nue_crash_desc {
    uint32_t struct_size;
    nue_crash_flags flags;
    const char* report_root;                /* VFS path, default "user:crash" */
    uint32_t keep_reports;                  /* rotate: keep this many (0 = 10) */
    nue_crash_callback callback;
    void* userdata;
    nue_guest_pc_provider guest_pc;
    void* guest_pc_userdata;
} nue_crash_desc;

NUE_API nue_result NUE_CALL nue_diag_install_crash_handler(const nue_crash_desc* desc);   /* chains to a previously installed handler */
NUE_API nue_result NUE_CALL nue_diag_uninstall_crash_handler(void);
/* Free-form facts attached to every report: build id, game state, current track. Fixed capacity (64 entries of 128+256 bytes). */
NUE_API nue_result NUE_CALL nue_diag_set_note(const char* key, const char* value);        /* value NULL removes */
NUE_API void NUE_CALL nue_diag_breadcrumb(const char* message);                           /* ring of the last 128 messages, cheap, lock free */
/* Write a report now without crashing (for fatal-but-handled errors). Returns the native directory path via buffer. */
NUE_API nue_result NUE_CALL nue_diag_write_report(const char* reason, char* dir_buffer, size_t dir_capacity, size_t* out_size);
/* Reports left by earlier runs (newest first). Use to offer "send report" or to note that the last run crashed. */
NUE_API nue_result NUE_CALL nue_diag_list_reports(char* buffer, size_t capacity, size_t* out_size);   /* '\n' separated native directory paths */
NUE_API nue_result NUE_CALL nue_diag_clear_reports(void);

/* Test hook: raise a crash of the given kind inside the engine's handler in a child-safe way. Only active with NUE_INIT flag
 * NUE_INIT_DETERMINISTIC or when the config key "diag.allow_test_crash" is true. */
typedef enum nue_test_crash_kind { NUE_TEST_CRASH_NULL_WRITE = 1, NUE_TEST_CRASH_ABORT, NUE_TEST_CRASH_DIVIDE_BY_ZERO, NUE_TEST_CRASH_STACK_OVERFLOW } nue_test_crash_kind;
NUE_API nue_result NUE_CALL nue_diag_test_crash(nue_test_crash_kind kind);

/* ============================================================================================== assertions */

typedef enum nue_assert_action { NUE_ASSERT_BREAK = 0, NUE_ASSERT_IGNORE, NUE_ASSERT_ABORT } nue_assert_action;
typedef nue_assert_action (NUE_CALL *nue_assert_handler)(void* userdata, const char* expression, const char* message, const char* file, int line);
NUE_API void NUE_CALL nue_diag_set_assert_handler(nue_assert_handler handler, void* userdata);   /* default: log FATAL, then break in debug builds / abort in release */
NUE_API nue_assert_action NUE_CALL nue_diag_assert_failed(const char* expression, const char* message, const char* file, int line);
#if defined(NUE_ENABLE_ASSERTS) || !defined(NDEBUG)
#  define NUE_ASSERT(expr) do { if (!(expr) && nue_diag_assert_failed(#expr, NULL, __FILE__, __LINE__) == NUE_ASSERT_BREAK) { NUE_DEBUG_BREAK(); } } while (0)
#  define NUE_ASSERT_MSG(expr, msg) do { if (!(expr) && nue_diag_assert_failed(#expr, (msg), __FILE__, __LINE__) == NUE_ASSERT_BREAK) { NUE_DEBUG_BREAK(); } } while (0)
#else
#  define NUE_ASSERT(expr) ((void)0)
#  define NUE_ASSERT_MSG(expr, msg) ((void)0)
#endif
#if defined(_MSC_VER)
#  define NUE_DEBUG_BREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
#  define NUE_DEBUG_BREAK() __builtin_trap()
#else
#  define NUE_DEBUG_BREAK() ((void)0)
#endif

/* ============================================================================================== counters and frame stats */

typedef uint32_t nue_counter_id;        /* 0 = invalid */
typedef enum nue_counter_kind { NUE_COUNTER_GAUGE = 1, NUE_COUNTER_ACCUMULATOR, NUE_COUNTER_TIMER_NS } nue_counter_kind;

NUE_API nue_result NUE_CALL nue_counter_register(const char* name, nue_counter_kind kind, nue_counter_id* out_id);   /* same name -> same id */
NUE_API void NUE_CALL nue_counter_set(nue_counter_id id, int64_t value);        /* [any] lock free */
NUE_API void NUE_CALL nue_counter_add(nue_counter_id id, int64_t delta);
NUE_API int64_t NUE_CALL nue_counter_get(nue_counter_id id);
typedef struct nue_counter_value { const char* name; nue_counter_kind kind; int64_t value; } nue_counter_value;
NUE_API nue_result NUE_CALL nue_counter_snapshot(nue_counter_value* out, uint32_t capacity, uint32_t* out_count);   /* name pointers stay valid until shutdown */

/* Frame statistics for the host's frame loop (what the launcher's --fps-log prints). Call nue_frame_mark() once per presented frame. */
typedef struct nue_frame_stats {
    uint32_t struct_size;
    uint32_t window_frames;                 /* frames in the window */
    double window_seconds;
    double fps;                             /* average over the window */
    double frame_ms_avg, frame_ms_p50, frame_ms_p95, frame_ms_p99, frame_ms_max;
    double cpu_busy_percent;                /* main thread CPU time / wall time in the window, -1 unknown */
    uint64_t total_frames;
} nue_frame_stats;
NUE_API void NUE_CALL nue_frame_mark(void);
NUE_API nue_result NUE_CALL nue_frame_get_stats(double window_seconds /* 0 = 5 s */, nue_frame_stats* out);
NUE_API nue_result NUE_CALL nue_frame_set_log_interval(double seconds);       /* 0 disables the periodic INFO line "fps=.. p95=.." */

/* Trace zones written as Chrome trace JSON (chrome://tracing, Perfetto). Off by default; ~50 ns per zone when on, ~1 ns off. */
NUE_API nue_result NUE_CALL nue_trace_start(const char* path);
NUE_API nue_result NUE_CALL nue_trace_stop(void);
NUE_API nue_bool NUE_CALL nue_trace_enabled(void);
NUE_API void NUE_CALL nue_trace_begin(const char* name);
NUE_API void NUE_CALL nue_trace_end(void);
#define NUE_TRACE_BEGIN(name) do { if (nue_trace_enabled()) nue_trace_begin(name); } while (0)
#define NUE_TRACE_END()       do { if (nue_trace_enabled()) nue_trace_end(); } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_DIAG_H */
