// Memory information, pressure handling and configurable budgets with eviction hooks. No constants about any particular device live here:
// limits are resolved from policies against what the machine reports (nue_memory_info), and every parameter can be overridden by configuration.
#include "internal/core_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

constexpr uint64_t kMiB = 1024ull * 1024ull;

struct Budget {
    nue_budget_id id = 0;
    std::string name;
    nue_budget_kind kind = NUE_BUDGET_GAME;
    nue_budget_policy policy;
    nue_budget_evict_fn evict = nullptr;
    void* userdata = nullptr;
    uint64_t used = 0, peak = 0, evictions = 0, bytes_evicted = 0;
};

struct MemState {
    std::mutex mutex;
    std::map<nue_budget_id, Budget> budgets;
    nue_budget_id next_id = 1;
    nue_memory_pressure pressure = NUE_PRESSURE_NONE;
    // null-backend simulation of the machine
    bool sim = false;
    nue_memory_info sim_info;
    // cached real query (platform calls can be slow)
    nue_memory_info cached;
    int64_t cached_at = 0;
    bool have_cached = false;
};

MemState& M() { static MemState* m = new MemState(); return *m; }

bool policy_is_zero(const nue_budget_policy& p) { return !p.fixed_bytes && p.fraction == 0.0f && !p.min_bytes && !p.max_bytes && !p.reserve_bytes; }

void derive_process_available(nue_memory_info* out) {
    if (out->process_available != 0) return;
    uint64_t by_limit = (out->process_limit > out->process_used) ? out->process_limit - out->process_used : 0;
    if (out->process_limit && out->physical_available) out->process_available = std::min(by_limit, out->physical_available);
    else if (out->process_limit) out->process_available = by_limit;
    else out->process_available = out->physical_available;
}

void query_info(nue_memory_info* out) {
    NUE_STRUCT_INIT(out);
    const nue_platform_ops* p = nuei::platform();
    uint64_t total = 0, avail = 0;
    if (p && p->memory_info && NUE_SUCCEEDED(p->memory_info(&total, &avail))) { out->physical_total = total; out->physical_available = avail; }
    if (p && NUE_PLATFORM_ENTRY(p, memory_info_ex)) {
        nue_memory_info ex;
        NUE_STRUCT_INIT(&ex);
        ex = *out;
        if (NUE_SUCCEEDED(p->memory_info_ex(&ex))) { *out = ex; out->struct_size = sizeof(*out); }
    }
    derive_process_available(out);
}

nue_memory_info current_info(bool force_refresh) {
    MemState& m = M();
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        if (m.sim) { nue_memory_info i = m.sim_info; i.pressure = m.pressure; return i; }
        if (!force_refresh && m.have_cached && nuei::now_ns() - m.cached_at < 1000000000ll) { nue_memory_info i = m.cached; i.pressure = m.pressure; return i; }
    }
    nue_memory_info fresh;
    query_info(&fresh);
    std::lock_guard<std::mutex> lock(m.mutex);
    m.cached = fresh;
    m.cached_at = nuei::now_ns();
    m.have_cached = true;
    fresh.pressure = m.pressure;
    return fresh;
}

nue_budget_policy resolve_overrides(const Budget& b) {
    nue_budget_policy p = b.policy;
    const std::string base = "budget." + b.name + ".";
    if (nue_config_has((base + "limit_mb").c_str())) {
        int64_t mb = nue_config_get_int((base + "limit_mb").c_str(), 0);
        p.fixed_bytes = mb > 0 ? uint64_t(mb) * kMiB : 0;
    }
    if (nue_config_has((base + "fraction").c_str())) p.fraction = float(nue_config_get_double((base + "fraction").c_str(), p.fraction));
    if (nue_config_has((base + "min_mb").c_str())) p.min_bytes = uint64_t(std::max<int64_t>(0, nue_config_get_int((base + "min_mb").c_str(), 0))) * kMiB;
    if (nue_config_has((base + "max_mb").c_str())) p.max_bytes = uint64_t(std::max<int64_t>(0, nue_config_get_int((base + "max_mb").c_str(), 0))) * kMiB;
    if (nue_config_has((base + "reserve_mb").c_str())) p.reserve_bytes = uint64_t(std::max<int64_t>(0, nue_config_get_int((base + "reserve_mb").c_str(), 0))) * kMiB;
    return p;
}

uint64_t resolve_limit(const Budget& b, const nue_memory_info& info, nue_budget_policy* effective) {
    nue_budget_policy p = resolve_overrides(b);
    if (effective) *effective = p;
    uint64_t limit;
    if (p.fixed_bytes) limit = p.fixed_bytes;
    else {
        uint64_t base = 0;
        if (b.kind == NUE_BUDGET_TEXTURE && info.gpu_dedicated_total > 0 && !info.unified_memory)
            base = info.gpu_dedicated_available ? info.gpu_dedicated_available : info.gpu_dedicated_total;
        else base = info.process_available ? info.process_available : info.physical_available;
        if (base == 0 || p.fraction <= 0.0f) return UINT64_MAX;   // nothing known about the machine (or no policy): no limit, never a guess
        limit = uint64_t(double(base) * double(std::min(p.fraction, 1.0f)));
    }
    if (p.min_bytes) limit = std::max(limit, p.min_bytes);
    if (p.max_bytes) limit = std::min(limit, p.max_bytes);
    limit = limit > p.reserve_bytes ? limit - p.reserve_bytes : 0;
    return limit;
}

uint64_t run_evict(nue_budget_id id, uint64_t needed, nue_memory_pressure level) {
    nue_budget_evict_fn fn = nullptr;
    void* ud = nullptr;
    {
        MemState& m = M();
        std::lock_guard<std::mutex> lock(m.mutex);
        auto it = m.budgets.find(id);
        if (it == m.budgets.end() || !it->second.evict) return 0;
        fn = it->second.evict;
        ud = it->second.userdata;
    }
    uint64_t freed = fn(ud, id, needed, level);   // no lock held: the hook reports its own frees with nue_budget_report
    MemState& m = M();
    std::lock_guard<std::mutex> lock(m.mutex);
    auto it = m.budgets.find(id);
    if (it != m.budgets.end()) { ++it->second.evictions; it->second.bytes_evicted += freed; }
    return freed;
}

nue_bool NUE_CALL pressure_watcher(void*, const nue_event* e) {
    if (e->type == NUE_EVENT_MEMORY_PRESSURE) {
        nue_memory_pressure level = nue_memory_pressure(e->payload.memory.level);
        { MemState& m = M(); std::lock_guard<std::mutex> lock(m.mutex); m.pressure = level; m.have_cached = false; }
        if (level != NUE_PRESSURE_NONE) nue_budget_trim(level);
    } else if (e->type == NUE_EVENT_APP_LOW_MEMORY) {
        { MemState& m = M(); std::lock_guard<std::mutex> lock(m.mutex); m.have_cached = false; }
        nue_budget_trim(NUE_PRESSURE_HIGH);
    } else if (e->type == NUE_EVENT_APP_RESUMED || e->type == NUE_EVENT_SYSTEM_RESUMED) {
        nue_budget_recompute();
    }
    return NUE_TRUE;
}

}  // namespace

namespace nuei {

void memory_init() {
    MemState& m = M();
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        m.budgets.clear();
        m.next_id = 1;
        m.pressure = NUE_PRESSURE_NONE;
        m.sim = false;
        m.have_cached = false;
    }
    nue_event_add_watcher(pressure_watcher, nullptr);
}

void memory_shutdown() {
    nue_event_remove_watcher(pressure_watcher, nullptr);
    MemState& m = M();
    std::lock_guard<std::mutex> lock(m.mutex);
    m.budgets.clear();
    m.sim = false;
}

void memory_null_set(const nue_memory_info* info) {
    MemState& m = M();
    {
        std::lock_guard<std::mutex> lock(m.mutex);
        m.sim = info != nullptr;
        if (info) { m.sim_info = *info; derive_process_available(&m.sim_info); }
    }
    nue_budget_recompute();
}

}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_memory_get_info(nue_memory_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    nue_memory_info v = current_info(false);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &v, keep < sizeof(v) ? keep : sizeof(v));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API const char* NUE_CALL nue_memory_pressure_name(nue_memory_pressure l) {
    switch (l) {
        case NUE_PRESSURE_MODERATE: return "moderate";
        case NUE_PRESSURE_HIGH: return "high";
        case NUE_PRESSURE_CRITICAL: return "critical";
        default: return "none";
    }
}

NUE_API nue_result NUE_CALL nue_budget_default_policy(nue_budget_kind kind, nue_budget_policy* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    uint32_t keep = out->struct_size ? out->struct_size : uint32_t(sizeof(*out));
    nue_budget_policy p;
    std::memset(&p, 0, sizeof(p));
    p.struct_size = sizeof(p);
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    nue_system_get_info(&si);
    // Policy defaults by machine class: fractions of what the machine has, never absolute sizes. Phones and handhelds share memory with the GPU
    // and the OS kills a process that is too greedy, so their fractions are smaller. All of it can be overridden per budget.
    const bool constrained = si.device_class == NUE_DEVICE_PHONE || si.device_class == NUE_DEVICE_TABLET || si.device_class == NUE_DEVICE_HANDHELD ||
                             (si.platform_flags & NUE_PLATFORM_HANDHELD) != 0;
    switch (kind) {
        case NUE_BUDGET_TEXTURE: p.fraction = constrained ? 0.35f : 0.50f; break;
        case NUE_BUDGET_GEOMETRY: p.fraction = constrained ? 0.10f : 0.15f; break;
        case NUE_BUDGET_AUDIO: p.fraction = constrained ? 0.08f : 0.10f; break;
        case NUE_BUDGET_CACHE_RAM: p.fraction = constrained ? 0.05f : 0.10f; break;
        case NUE_BUDGET_IMPORT_STAGING: p.fraction = 0.10f; break;
        default: break;   // CACHE_DISK and GAME: no default limit
    }
    std::memcpy(out, &p, keep < sizeof(p) ? keep : sizeof(p));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_budget_register(const nue_budget_desc* desc, nue_budget_id* out_id) {
    if (!desc || !desc->name || !*desc->name || !out_id) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_budget_register: name and out_id are required");
    for (const char* c = desc->name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-'))
            return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "budget name '%s' may only contain [a-z0-9_.-]", desc->name);
    nue_budget_policy policy = desc->policy;
    if (policy.struct_size == 0 || policy_is_zero(policy)) nue_budget_default_policy(desc->kind, &policy);
    MemState& m = M();
    std::lock_guard<std::mutex> lock(m.mutex);
    for (auto& kv : m.budgets) if (kv.second.name == desc->name) { *out_id = kv.first; return NUE_OK_NOOP; }
    Budget b;
    b.id = m.next_id++;
    b.name = desc->name;
    b.kind = desc->kind;
    b.policy = policy;
    b.policy.struct_size = sizeof(nue_budget_policy);
    b.evict = desc->evict;
    b.userdata = desc->userdata;
    *out_id = b.id;
    m.budgets[b.id] = b;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_budget_unregister(nue_budget_id id) {
    MemState& m = M();
    std::lock_guard<std::mutex> lock(m.mutex);
    return m.budgets.erase(id) ? NUE_OK : NUE_ERR_INVALID_HANDLE;
}

NUE_API nue_result NUE_CALL nue_budget_find(const char* name, nue_budget_id* out_id) {
    if (!name || !out_id) return NUE_ERR_INVALID_ARGUMENT;
    MemState& m = M();
    std::lock_guard<std::mutex> lock(m.mutex);
    for (auto& kv : m.budgets) if (kv.second.name == name) { *out_id = kv.first; return NUE_OK; }
    return NUE_ERR_NOT_FOUND;
}

NUE_API nue_result NUE_CALL nue_budget_get_state(nue_budget_id id, nue_budget_state* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    nue_memory_info info = current_info(false);
    nue_budget_state s;
    std::memset(&s, 0, sizeof(s));
    s.struct_size = sizeof(s);
    {
        MemState& m = M();
        std::lock_guard<std::mutex> lock(m.mutex);
        auto it = m.budgets.find(id);
        if (it == m.budgets.end()) return NUE_ERR_INVALID_HANDLE;
        const Budget& b = it->second;
        s.limit_bytes = resolve_limit(b, info, &s.policy);
        s.policy.struct_size = sizeof(nue_budget_policy);
        s.used_bytes = b.used; s.peak_bytes = b.peak; s.evictions = b.evictions; s.bytes_evicted = b.bytes_evicted;
    }
    uint32_t keep = out->struct_size;
    std::memcpy(out, &s, keep < sizeof(s) ? keep : sizeof(s));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_budget_report(nue_budget_id id, int64_t delta) {
    MemState& m = M();
    std::lock_guard<std::mutex> lock(m.mutex);
    auto it = m.budgets.find(id);
    if (it == m.budgets.end()) return;
    Budget& b = it->second;
    if (delta < 0) { uint64_t d = uint64_t(-delta); b.used = d > b.used ? 0 : b.used - d; }
    else { b.used += uint64_t(delta); b.peak = std::max(b.peak, b.used); }
}

NUE_API nue_result NUE_CALL nue_budget_request(nue_budget_id id, uint64_t bytes) {
    nue_memory_info info = current_info(false);
    auto try_fit = [&]() -> int {   // 1 fits (accounted), 0 does not, -1 bad id
        MemState& m = M();
        std::lock_guard<std::mutex> lock(m.mutex);
        auto it = m.budgets.find(id);
        if (it == m.budgets.end()) return -1;
        Budget& b = it->second;
        uint64_t limit = resolve_limit(b, info, nullptr);
        if (limit == UINT64_MAX || b.used + bytes <= limit) { b.used += bytes; b.peak = std::max(b.peak, b.used); return 1; }
        return 0;
    };
    int r = try_fit();
    if (r < 0) return NUE_ERR_INVALID_HANDLE;
    if (r == 1) return NUE_OK;
    // does not fit: ask the owner first, then everybody else
    uint64_t needed = 0;
    {
        MemState& m = M();
        std::lock_guard<std::mutex> lock(m.mutex);
        const Budget& b = m.budgets[id];
        uint64_t limit = resolve_limit(b, info, nullptr);
        needed = b.used + bytes > limit ? b.used + bytes - limit : 0;
    }
    run_evict(id, needed, NUE_PRESSURE_MODERATE);
    if (try_fit() == 1) return NUE_OK;
    std::vector<nue_budget_id> others;
    { MemState& m = M(); std::lock_guard<std::mutex> lock(m.mutex); for (auto& kv : m.budgets) if (kv.first != id) others.push_back(kv.first); }
    for (nue_budget_id o : others) {
        run_evict(o, needed, NUE_PRESSURE_HIGH);
        info = current_info(true);
        if (try_fit() == 1) return NUE_OK;
    }
    return nuei::fail(NUE_ERR_OVERFLOW, "budget request of %llu bytes does not fit even after eviction", (unsigned long long)bytes);
}

NUE_API nue_result NUE_CALL nue_budget_set_policy(nue_budget_id id, const nue_budget_policy* policy) {
    if (!policy) return NUE_ERR_INVALID_ARGUMENT;
    nue_memory_info info = current_info(false);
    uint64_t over = 0;
    {
        MemState& m = M();
        std::lock_guard<std::mutex> lock(m.mutex);
        auto it = m.budgets.find(id);
        if (it == m.budgets.end()) return NUE_ERR_INVALID_HANDLE;
        it->second.policy = *policy;
        it->second.policy.struct_size = sizeof(nue_budget_policy);
        uint64_t limit = resolve_limit(it->second, info, nullptr);
        if (limit != UINT64_MAX && it->second.used > limit) over = it->second.used - limit;
    }
    if (over) run_evict(id, over, NUE_PRESSURE_MODERATE);
    return NUE_OK;
}

NUE_API uint64_t NUE_CALL nue_budget_trim(nue_memory_pressure level) {
    std::vector<nue_budget_id> ids;
    { MemState& m = M(); std::lock_guard<std::mutex> lock(m.mutex); for (auto& kv : m.budgets) ids.push_back(kv.first); }
    uint64_t freed = 0;
    for (nue_budget_id id : ids) freed += run_evict(id, 0, level);
    return freed;
}

NUE_API nue_result NUE_CALL nue_budget_recompute(void) {
    nue_memory_info info = current_info(true);
    std::vector<std::pair<nue_budget_id, uint64_t>> over;
    {
        MemState& m = M();
        std::lock_guard<std::mutex> lock(m.mutex);
        for (auto& kv : m.budgets) {
            uint64_t limit = resolve_limit(kv.second, info, nullptr);
            if (limit != UINT64_MAX && kv.second.used > limit) over.push_back({kv.first, kv.second.used - limit});
        }
    }
    for (auto& o : over) run_evict(o.first, o.second, NUE_PRESSURE_MODERATE);
    return NUE_OK;
}

}  // extern "C"
