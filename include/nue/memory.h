/*
 * NUE engine - memory information, pressure and budgets.
 *
 * There are no hard-coded memory limits anywhere in the engine. A "budget" is a named, configurable pool (textures, audio, geometry, caches,
 * import staging...). Its limit comes from a POLICY that is resolved against what the machine really has (nue_memory_info) and can be overridden
 * at run time by the application or by configuration keys:
 *     budget.<name>.limit_mb       absolute limit in MiB (0 or absent = use the policy)
 *     budget.<name>.fraction       fraction of the available memory (policy parameter)
 *     budget.<name>.min_mb, budget.<name>.max_mb, budget.<name>.reserve_mb
 * Default policies per platform class are provided by nue_budget_default_policy (a fraction of what is available, smaller on phones than on
 * desktops); they are starting points, not constants. Owners of resources (nue_texture, the Direct3D 9 layer, the audio mixer, the game) report
 * their use and register an EVICTION hook; the engine calls the hooks when a request does not fit or when the system reports memory pressure.
 *
 * Thread: [any]. Eviction hooks run on the thread that triggered them, without engine locks held; they may free memory and call nue_budget_report.
 */
#ifndef NUE_MEMORY_H
#define NUE_MEMORY_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nue_memory_pressure {
    NUE_PRESSURE_NONE = 0,
    NUE_PRESSURE_MODERATE,              /* trim caches that are cheap to rebuild */
    NUE_PRESSURE_HIGH,                  /* drop everything not needed for the next few seconds */
    NUE_PRESSURE_CRITICAL               /* the OS may kill the process: shed all that can be reloaded */
} nue_memory_pressure;

typedef struct nue_memory_info {
    uint32_t struct_size;
    uint64_t physical_total, physical_available;     /* system RAM, 0 = unknown */
    uint64_t process_used;                           /* resident set / working set / footprint of this process, 0 = unknown */
    uint64_t process_limit;                          /* hard cap imposed on the process (cgroup, Android heap class, iOS jetsam), 0 = none known */
    uint64_t process_available;                      /* best estimate of what the process can still allocate: min(limit - used, physical_available) */
    uint64_t gpu_dedicated_total, gpu_dedicated_available;   /* VRAM, 0 = unknown / unified memory */
    uint64_t gpu_shared_total;                       /* GPU-addressable system memory */
    nue_bool unified_memory;
    nue_memory_pressure pressure;                    /* the last pressure level reported by the OS (NONE when the platform reports none) */
    uint32_t reserved[8];
} nue_memory_info;

NUE_API nue_result NUE_CALL nue_memory_get_info(nue_memory_info* out);
NUE_API const char* NUE_CALL nue_memory_pressure_name(nue_memory_pressure level);

/* ---- budgets ---------------------------------------------------------------------------------------------------------------------- */

typedef uint32_t nue_budget_id;         /* 0 = invalid */

typedef enum nue_budget_kind {
    NUE_BUDGET_TEXTURE = 1, NUE_BUDGET_GEOMETRY, NUE_BUDGET_AUDIO, NUE_BUDGET_CACHE_RAM, NUE_BUDGET_CACHE_DISK, NUE_BUDGET_IMPORT_STAGING, NUE_BUDGET_GAME = 100
} nue_budget_kind;

/* limit = max(clamp(fixed_bytes ? fixed_bytes : fraction * base, min_bytes, max_bytes) - reserve_bytes, 0), where base is the process-available memory
 * (physical-available when the process has no known limit; GPU dedicated memory for NUE_BUDGET_TEXTURE on discrete GPUs). Unset fields are 0. */
typedef struct nue_budget_policy {
    uint32_t struct_size;
    uint64_t fixed_bytes;               /* absolute limit; wins over fraction */
    float fraction;                     /* of the base memory, 0..1 */
    uint64_t min_bytes, max_bytes;      /* 0 = unbounded */
    uint64_t reserve_bytes;             /* kept free for everything else */
} nue_budget_policy;

/* Called when a request does not fit or under memory pressure: free at least `bytes_needed` (0 = as much as is cheap) and return how much was
 * freed. The hook must also account what it freed with nue_budget_report(id, -freed); the return value is only statistics. `level` says how
 * aggressive to be. */
typedef uint64_t (NUE_CALL *nue_budget_evict_fn)(void* userdata, nue_budget_id budget, uint64_t bytes_needed, nue_memory_pressure level);

typedef struct nue_budget_desc {
    uint32_t struct_size;
    const char* name;                   /* unique, [a-z0-9_.-]: also the config key prefix */
    nue_budget_kind kind;
    nue_budget_policy policy;           /* all zero = nue_budget_default_policy(kind) */
    nue_budget_evict_fn evict;          /* may be NULL */
    void* userdata;
} nue_budget_desc;

typedef struct nue_budget_state {
    uint32_t struct_size;
    uint64_t limit_bytes;               /* resolved now; UINT64_MAX when the policy yields no limit (nothing is known about the machine) */
    uint64_t used_bytes, peak_bytes;
    uint64_t evictions, bytes_evicted;
    nue_budget_policy policy;
} nue_budget_state;

NUE_API nue_result NUE_CALL nue_budget_default_policy(nue_budget_kind kind, nue_budget_policy* out);   /* per-platform default for this machine class */
NUE_API nue_result NUE_CALL nue_budget_register(const nue_budget_desc* desc, nue_budget_id* out_id);   /* same name twice returns the existing id */
NUE_API nue_result NUE_CALL nue_budget_unregister(nue_budget_id id);
NUE_API nue_result NUE_CALL nue_budget_set_policy(nue_budget_id id, const nue_budget_policy* policy);  /* run-time change; evicts if the new limit is lower than the use */
NUE_API nue_result NUE_CALL nue_budget_get_state(nue_budget_id id, nue_budget_state* out);
NUE_API nue_result NUE_CALL nue_budget_find(const char* name, nue_budget_id* out_id);
/* Account for memory: delta may be negative. Never fails and never evicts (use nue_budget_request before big allocations). */
NUE_API void NUE_CALL nue_budget_report(nue_budget_id id, int64_t delta_bytes);
/* Ask whether `bytes` fit. If not, runs the eviction hook (then the hooks of the other budgets) and re-checks.
 * NUE_OK = fits and is accounted (no separate report needed); NUE_ERR_OVERFLOW = does not fit even after eviction (nothing accounted). */
NUE_API nue_result NUE_CALL nue_budget_request(nue_budget_id id, uint64_t bytes);
/* Run the eviction hooks of every budget for the given level (done automatically on NUE_EVENT_MEMORY_PRESSURE / NUE_EVENT_APP_LOW_MEMORY).
 * Returns the bytes freed. */
NUE_API uint64_t NUE_CALL nue_budget_trim(nue_memory_pressure level);
/* Re-resolve all limits against the current memory state (done on pressure events and resume). */
NUE_API nue_result NUE_CALL nue_budget_recompute(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_MEMORY_H */
