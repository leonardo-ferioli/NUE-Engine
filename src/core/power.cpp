// Power, thermal, game mode requests, thread priority helpers and performance hint sessions.
// The core keeps the state and the bookkeeping; the platform layer (platform_ops) does the OS work.
#include "internal/core_internal.h"

#include <cstring>
#include <mutex>

namespace {

struct PowerState {
    std::mutex mutex;
    nue_power_request request;
    nue_power_applied applied = 0;
    bool request_active = false;
    // null backend simulation (also the fallback values when no platform layer answers)
    bool sim_thermal_set = false, sim_power_set = false;
    nue_thermal_state sim_thermal = NUE_THERMAL_UNKNOWN;
    nue_power_state sim_power = NUE_POWER_UNKNOWN;
    int32_t sim_percent = -1;
    nue_thermal_state last_thermal = NUE_THERMAL_UNKNOWN;
    nue_power_state last_power = NUE_POWER_UNKNOWN;
    int32_t last_percent = -2;
    bool last_low_power = false;
    nue_game_mode_state last_game_mode = NUE_GAME_MODE_UNKNOWN;
    bool have_last = false;
};

PowerState& P() { static PowerState* p = new PowerState(); return *p; }

nue_power_applied requested_mask(const nue_power_request& r) {
    nue_power_applied m = 0;
    if (r.profile != NUE_PERF_DEFAULT) m |= NUE_POWER_APPLIED_PROFILE;
    if (r.game_hint != NUE_GAME_HINT_NONE) m |= NUE_POWER_APPLIED_GAME_HINT;
    if (r.keep_awake != NUE_KEEP_AWAKE_NONE) m |= NUE_POWER_APPLIED_KEEP_AWAKE;
    if (r.gpu != NUE_GPU_DEFAULT) m |= NUE_POWER_APPLIED_GPU;
    if (r.cpu != NUE_CPU_DEFAULT) m |= NUE_POWER_APPLIED_CPU;
    if (r.timer_resolution_us) m |= NUE_POWER_APPLIED_TIMER;
    if (r.target_frame_time_us) m |= NUE_POWER_APPLIED_FRAME_TIME;
    if (r.disable_power_throttling) m |= NUE_POWER_APPLIED_THROTTLING;
    if (r.prefer_low_latency_audio) m |= NUE_POWER_APPLIED_AUDIO;
    if (NUE_STRUCT_HAS(&r, nue_power_request, tdp_watts) && r.tdp_watts) m |= NUE_POWER_APPLIED_TDP;
    return m;
}

void fill_info(nue_power_info* out) {
    NUE_STRUCT_INIT(out);
    out->state = NUE_POWER_UNKNOWN;
    out->battery_percent = -1;
    out->seconds_left = -1;
    out->thermal = NUE_THERMAL_UNKNOWN;
    out->game_mode = NUE_GAME_MODE_UNKNOWN;
    out->thermal_headroom = -1.0f;
    out->tdp_watts_current = out->tdp_watts_min = out->tdp_watts_max = -1;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->power_info) {
        nue_power_info tmp;
        NUE_STRUCT_INIT(&tmp);
        tmp.battery_percent = -1; tmp.seconds_left = -1; tmp.thermal_headroom = -1.0f;
        tmp.tdp_watts_current = tmp.tdp_watts_min = tmp.tdp_watts_max = -1;
        if (NUE_SUCCEEDED(p->power_info(&tmp))) *out = tmp;
        out->struct_size = sizeof(*out);
    }
    PowerState& s = P();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.sim_power_set) { out->state = s.sim_power; out->battery_percent = s.sim_percent; out->on_ac_power = (s.sim_power != NUE_POWER_ON_BATTERY) ? NUE_TRUE : NUE_FALSE; }
    if (s.sim_thermal_set) out->thermal = s.sim_thermal;
    out->display_keep_awake_active = (s.request_active && (s.request.keep_awake & NUE_KEEP_AWAKE_DISPLAY) && (s.applied & NUE_POWER_APPLIED_KEEP_AWAKE)) ? NUE_TRUE : NUE_FALSE;
}

void push_power_event(nue_event_type type, const nue_power_info& info) {
    nue_event e;
    std::memset(&e, 0, sizeof(e));
    e.type = type;
    e.payload.power.thermal = info.thermal;
    e.payload.power.power_state = info.state;
    e.payload.power.percent = info.battery_percent;
    e.payload.power.low_power = info.low_power_mode;
    e.payload.power.game_mode = uint8_t(info.game_mode);
    nue_internal_push_event(&e);
}

}  // namespace

namespace nuei {

void power_init() {
    PowerState& s = P();
    std::lock_guard<std::mutex> lock(s.mutex);
    NUE_STRUCT_INIT(&s.request);
    s.applied = 0;
    s.request_active = false;
    s.sim_thermal_set = s.sim_power_set = false;
    s.have_last = false;
}

void power_shutdown() {
    nue_power_request zero;
    NUE_STRUCT_INIT(&zero);
    bool had;
    { PowerState& s = P(); std::lock_guard<std::mutex> lock(s.mutex); had = s.request_active; }
    if (had) nue_power_apply(&zero, nullptr);   // give back timer resolution, keep-awake, priority class...
}

// Called from pump when the platform may have changed something: compare and raise events.
void power_poll_and_notify() {
    nue_power_info info;
    fill_info(&info);
    PowerState& s = P();
    bool thermal = false, power = false, low = false, gm = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.have_last) {
            thermal = info.thermal != s.last_thermal;
            power = info.state != s.last_power || info.battery_percent != s.last_percent;
            low = (info.low_power_mode != 0) != s.last_low_power;
            gm = info.game_mode != s.last_game_mode;
        }
        s.last_thermal = info.thermal; s.last_power = info.state; s.last_percent = info.battery_percent;
        s.last_low_power = info.low_power_mode != 0; s.last_game_mode = info.game_mode; s.have_last = true;
    }
    if (thermal) push_power_event(NUE_EVENT_THERMAL_CHANGED, info);
    if (power) push_power_event(NUE_EVENT_POWER_CHANGED, info);
    if (low) push_power_event(NUE_EVENT_LOW_POWER_MODE_CHANGED, info);
    if (gm) push_power_event(NUE_EVENT_GAME_MODE_CHANGED, info);
}

void power_null_set_thermal(nue_thermal_state state) {
    { PowerState& s = P(); std::lock_guard<std::mutex> lock(s.mutex); s.sim_thermal_set = true; s.sim_thermal = state; }
    power_poll_and_notify();
}

void power_null_set_power(nue_power_state state, int32_t percent) {
    { PowerState& s = P(); std::lock_guard<std::mutex> lock(s.mutex); s.sim_power_set = true; s.sim_power = state; s.sim_percent = percent; }
    power_poll_and_notify();
}

}  // namespace nuei

struct nue_perf_hint_s {
    void* impl = nullptr;
    int64_t target_ns = 0;
    uint64_t reports = 0;
    int64_t last_actual_ns = 0;
};

extern "C" {

NUE_API nue_result NUE_CALL nue_power_get_info(nue_power_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    nue_power_info v;
    fill_info(&v);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &v, keep < sizeof(v) ? keep : sizeof(v));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_thermal_state NUE_CALL nue_power_get_thermal_state(void) { nue_power_info i; fill_info(&i); return i.thermal; }

NUE_API const char* NUE_CALL nue_thermal_state_name(nue_thermal_state s) {
    switch (s) {
        case NUE_THERMAL_NOMINAL: return "nominal";
        case NUE_THERMAL_FAIR: return "fair";
        case NUE_THERMAL_SERIOUS: return "serious";
        case NUE_THERMAL_CRITICAL: return "critical";
        default: return "unknown";
    }
}

NUE_API nue_result NUE_CALL nue_power_apply(const nue_power_request* request, nue_power_applied* out_applied) {
    if (!request || request->struct_size < sizeof(uint32_t) * 2) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_power_apply: request is required");
    nue_power_request r;
    NUE_STRUCT_INIT(&r);
    std::memcpy(&r, request, request->struct_size < sizeof(r) ? request->struct_size : sizeof(r));
    r.struct_size = sizeof(r);
    const nue_power_applied wanted = requested_mask(r);
    nue_power_applied applied = 0;
    nue_result pr = NUE_ERR_UNSUPPORTED;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->power_apply) pr = p->power_apply(&r, &applied);
    if (NUE_FAILED(pr)) applied = 0;
    {
        PowerState& s = P();
        std::lock_guard<std::mutex> lock(s.mutex);
        s.request = r;
        s.applied = applied;
        s.request_active = wanted != 0 || applied != 0;
    }
    if (out_applied) *out_applied = applied;
    if (wanted == 0) return NUE_OK;
    nue_power_applied honoured = applied & wanted;
    if (honoured == wanted) return NUE_OK;
    return honoured ? NUE_OK_PARTIAL : NUE_OK_NOOP;
}

NUE_API nue_result NUE_CALL nue_power_get_request(nue_power_request* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    PowerState& s = P();
    std::lock_guard<std::mutex> lock(s.mutex);
    nue_power_request v = s.request;
    if (v.struct_size == 0) NUE_STRUCT_INIT(&v);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &v, keep < sizeof(v) ? keep : sizeof(v));
    out->struct_size = keep;
    return NUE_OK;
}

static nue_result modify_request(void (*edit)(nue_power_request*, uint32_t), uint32_t arg) {
    nue_power_request r;
    NUE_STRUCT_INIT(&r);
    nue_power_get_request(&r);
    edit(&r, arg);
    return nue_power_apply(&r, nullptr);
}

NUE_API nue_result NUE_CALL nue_power_set_keep_awake(nue_keep_awake flags) {
    return modify_request([](nue_power_request* r, uint32_t a) { r->keep_awake = a; }, flags);
}

NUE_API nue_result NUE_CALL nue_power_set_timer_resolution(uint32_t us) {
    return modify_request([](nue_power_request* r, uint32_t a) { r->timer_resolution_us = a; }, us);
}

NUE_API uint32_t NUE_CALL nue_power_get_timer_resolution(void) {
    const nue_platform_ops* p = nuei::platform();
    uint32_t us = 0;
    if (p && p->timer_resolution_get && NUE_SUCCEEDED(p->timer_resolution_get(&us))) return us;
    return 0;
}

// ---- threads -------------------------------------------------------------------------------------------------------------------

static nue_native_thread_ref ref_of(nue_thread t) {
    nue_native_thread_ref r;
    NUE_STRUCT_INIT(&r);
    if (t) r.id = nue_thread_get_id(t);   // NULL = the calling thread (id/handle both zero)
    return r;
}

NUE_API nue_result NUE_CALL nue_thread_set_priority(nue_thread t, nue_thread_priority prio) {
    nue_native_thread_ref r = ref_of(t);
    return nue_native_thread_set_priority(&r, prio);
}
NUE_API nue_result NUE_CALL nue_thread_get_priority(nue_thread t, nue_thread_priority* out) {
    const nue_platform_ops* p = nuei::platform();
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    if (!p || !p->thread_get_priority) return NUE_ERR_UNSUPPORTED;
    nue_native_thread_ref r = ref_of(t);
    return p->thread_get_priority(&r, out);
}
NUE_API nue_result NUE_CALL nue_thread_set_affinity(nue_thread t, uint64_t mask) { nue_native_thread_ref r = ref_of(t); return nue_native_thread_set_affinity(&r, mask); }
NUE_API nue_result NUE_CALL nue_thread_set_core_class(nue_thread t, nue_cpu_preference pref) { nue_native_thread_ref r = ref_of(t); return nue_native_thread_set_core_class(&r, pref); }

NUE_API nue_result NUE_CALL nue_native_thread_set_priority(const nue_native_thread_ref* t, nue_thread_priority prio) {
    const nue_platform_ops* p = nuei::platform();
    if (!t) return NUE_ERR_INVALID_ARGUMENT;
    if (!p || !p->thread_set_priority) return NUE_ERR_UNSUPPORTED;
    return p->thread_set_priority(t, prio);
}
NUE_API nue_result NUE_CALL nue_native_thread_set_affinity(const nue_native_thread_ref* t, uint64_t mask) {
    const nue_platform_ops* p = nuei::platform();
    if (!t) return NUE_ERR_INVALID_ARGUMENT;
    if (!p || !p->thread_set_affinity) return NUE_ERR_UNSUPPORTED;
    return p->thread_set_affinity(t, mask);
}
NUE_API nue_result NUE_CALL nue_native_thread_set_core_class(const nue_native_thread_ref* t, nue_cpu_preference pref) {
    const nue_platform_ops* p = nuei::platform();
    if (!t) return NUE_ERR_INVALID_ARGUMENT;
    if (!p || !p->thread_set_core_class) return NUE_ERR_UNSUPPORTED;
    return p->thread_set_core_class(t, pref);
}

// ---- performance hints ---------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_perf_hint_create(const nue_native_thread_ref* threads, uint32_t count, int64_t target_ns, nue_perf_hint* out) {
    if (!out || (count && !threads)) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    nue_perf_hint_s* h = new (std::nothrow) nue_perf_hint_s();
    if (!h) return NUE_ERR_OUT_OF_MEMORY;
    h->target_ns = target_ns;
    const nue_platform_ops* p = nuei::platform();
    if (p && p->perf_hint_create) {
        nue_result r = p->perf_hint_create(threads, count, target_ns, &h->impl);
        if (NUE_FAILED(r)) h->impl = nullptr;   // keep a bookkeeping-only session: callers do not need to special-case platforms
    }
    *out = h;
    return h->impl ? NUE_OK : NUE_OK_NOOP;
}

NUE_API void NUE_CALL nue_perf_hint_destroy(nue_perf_hint h) {
    if (!h) return;
    const nue_platform_ops* p = nuei::platform();
    if (h->impl && p && p->perf_hint_destroy) p->perf_hint_destroy(h->impl);
    delete h;
}

NUE_API nue_result NUE_CALL nue_perf_hint_update_target(nue_perf_hint h, int64_t target_ns) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    h->target_ns = target_ns;
    const nue_platform_ops* p = nuei::platform();
    if (h->impl && p && p->perf_hint_update_target) return p->perf_hint_update_target(h->impl, target_ns);
    return NUE_OK_NOOP;
}

NUE_API nue_result NUE_CALL nue_perf_hint_report(nue_perf_hint h, int64_t actual_ns) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    ++h->reports;
    h->last_actual_ns = actual_ns;
    const nue_platform_ops* p = nuei::platform();
    if (h->impl && p && p->perf_hint_report) return p->perf_hint_report(h->impl, actual_ns);
    return NUE_OK_NOOP;
}

}  // extern "C"
