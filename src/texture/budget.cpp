// Texture memory budget: byte accounting with LRU eviction candidates, policy presets, statistics, plus the memory estimate model.
#include "internal.hpp"
#ifdef NUE_TEXTURE_WITH_ENGINE
#  include "nue/memory.h"
#endif
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace nuetex {
namespace {

struct Res { uint64_t bytes; uint32_t flags; uint64_t tick; uint32_t gen; bool used; };

struct Budget {
    std::mutex m;
    nue_tex_budget_policy policy;
    uint64_t limit = UINT64_MAX;
    nue_tex_evict_fn evict = nullptr;
    void* user = nullptr;
    std::vector<Res> res;               // index = id & 0xFFFFF, generation in the high bits
    std::vector<uint32_t> free_list;
    uint64_t used = 0, peak = 0, tick = 0, evictions_requested = 0, bytes_evicted = 0;
    uint32_t count = 0, pinned = 0, over_events = 0;
    bool evicting = false;
    uint32_t engine_id = 0;             // nue_budget_id when attached to the engine budget manager
};

inline Budget* B(nue_tex_budget b) { return reinterpret_cast<Budget*>(b); }
constexpr uint32_t kIndexBits = 20;
inline uint32_t make_id(uint32_t index, uint32_t gen) { return ((gen & 0xFFF) << kIndexBits) | (index + 1); }
inline bool split_id(uint32_t id, uint32_t& index, uint32_t& gen) { index = (id & ((1u << kIndexBits) - 1)); if (!index) return false; --index; gen = id >> kIndexBits; return true; }

Res* lookup(Budget& b, uint32_t id) {
    uint32_t i, g;
    if (!split_id(id, i, g) || i >= b.res.size()) return nullptr;
    Res& r = b.res[i];
    return r.used && (r.gen & 0xFFF) == g ? &r : nullptr;
}

// Ask the owner to release at least `need` bytes (lock NOT held on entry). Returns the bytes the owner reports as freed.
uint64_t evict_bytes(Budget& b, uint64_t need) {
    std::vector<nue_tex_resource_id> cand;
    nue_tex_evict_fn fn;
    void* user;
    {
        std::lock_guard<std::mutex> l(b.m);
        if (!b.evict || b.evicting) return 0;
        std::vector<std::pair<uint64_t, uint32_t>> order;
        for (uint32_t i = 0; i < b.res.size(); ++i)
            if (b.res[i].used && !(b.res[i].flags & NUE_TEX_RES_PINNED)) order.push_back({b.res[i].tick, i});
        std::sort(order.begin(), order.end());
        uint64_t acc = 0;
        for (auto& o : order) { cand.push_back(make_id(o.second, b.res[o.second].gen)); acc += b.res[o.second].bytes; if (acc >= need) break; }
        if (cand.empty()) return 0;
        b.evicting = true;
        ++b.evictions_requested;
        fn = b.evict; user = b.user;
    }
    const uint64_t freed = fn(user, cand.data(), uint32_t(cand.size()), need);
    std::lock_guard<std::mutex> l(b.m);
    b.evicting = false;
    b.bytes_evicted += freed;
    return freed;
}

// Called with the lock NOT held; asks the owner to release at least `need` bytes.
void enforce(Budget& b) {
    if (b.engine_id) return;            // the engine budget manager enforces the limit
    for (int round = 0; round < 4; ++round) {
        std::vector<nue_tex_resource_id> cand;
        uint64_t need = 0;
        nue_tex_evict_fn fn;
        void* user;
        {
            std::lock_guard<std::mutex> l(b.m);
            if (b.used <= b.limit || !b.evict || b.evicting) return;
            need = b.used - b.limit;
            need += std::min<uint64_t>(b.limit / 32, 16ull << 20);          // hysteresis: free a little more than the overshoot so the next adds do not each trigger a round
            b.evicting = true;
            ++b.over_events;
            std::vector<std::pair<uint64_t, uint32_t>> order;
            for (uint32_t i = 0; i < b.res.size(); ++i)
                if (b.res[i].used && !(b.res[i].flags & NUE_TEX_RES_PINNED)) order.push_back({b.res[i].tick, i});
            std::sort(order.begin(), order.end());
            uint64_t acc = 0;
            for (auto& o : order) { cand.push_back(make_id(o.second, b.res[o.second].gen)); acc += b.res[o.second].bytes; if (acc >= need * 2) break; }
            fn = b.evict; user = b.user;
            ++b.evictions_requested;
        }
        const uint64_t freed = cand.empty() ? 0 : fn(user, cand.data(), uint32_t(cand.size()), need);
        {
            std::lock_guard<std::mutex> l(b.m);
            b.evicting = false;
            b.bytes_evicted += freed;
            if (b.used <= b.limit || freed == 0) return;
        }
    }
}

}  // namespace
}  // namespace nuetex

using namespace nuetex;

extern "C" {

uint64_t NUE_CALL nue_tex_budget_resolve(const nue_tex_budget_policy* p) {
    if (!p) return UINT64_MAX;
    uint64_t v = UINT64_MAX;
    switch (p->mode) {
    case NUE_TEX_BUDGET_UNLIMITED: return UINT64_MAX;
    case NUE_TEX_BUDGET_BYTES: v = p->limit_bytes; break;
    case NUE_TEX_BUDGET_FRACTION: v = uint64_t(double(p->available_bytes) * std::min(1.0, std::max(0.0, p->fraction))); break;
    case NUE_TEX_BUDGET_PRESET: {
        double f = 0;
        switch (p->preset) {
        case NUE_TEX_BUDGET_PRESET_LOW_MEMORY_MOBILE: f = 0.12; break;
        case NUE_TEX_BUDGET_PRESET_MOBILE: f = 0.20; break;
        case NUE_TEX_BUDGET_PRESET_DESKTOP: f = 0.35; break;
        case NUE_TEX_BUDGET_PRESET_HIGH_END: f = 0.50; break;
        default: return UINT64_MAX;
        }
        v = uint64_t(double(p->available_bytes) * f);
        if (p->preset == NUE_TEX_BUDGET_PRESET_LOW_MEMORY_MOBILE) v = std::max<uint64_t>(v, 64ull << 20);
        break;
    }
    }
    if (p->min_bytes) v = std::max(v, p->min_bytes);
    if (p->max_bytes) v = std::min(v, p->max_bytes);
    return v;
}

void NUE_CALL nue_tex_budget_policy_preset(nue_tex_budget_preset preset, uint64_t available_bytes, nue_tex_budget_policy* out) {
    if (!out) return;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->mode = preset == NUE_TEX_BUDGET_PRESET_UNLIMITED ? NUE_TEX_BUDGET_UNLIMITED : NUE_TEX_BUDGET_PRESET;
    out->preset = preset;
    out->available_bytes = available_bytes;
}

nue_result NUE_CALL nue_tex_budget_create(const nue_tex_budget_policy* policy, nue_tex_evict_fn evict, void* userdata, nue_tex_budget* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    auto* b = new Budget();
    if (policy) b->policy = *policy; else { std::memset(&b->policy, 0, sizeof b->policy); b->policy.struct_size = sizeof b->policy; }
    b->limit = nue_tex_budget_resolve(&b->policy);
    b->evict = evict; b->user = userdata;
    *out = reinterpret_cast<nue_tex_budget>(b);
    return NUE_OK;
}

void NUE_CALL nue_tex_budget_destroy(nue_tex_budget h) { delete B(h); }

void NUE_CALL nue_tex_budget_set_policy(nue_tex_budget h, const nue_tex_budget_policy* policy) {
    if (!h || !policy) return;
    {
        std::lock_guard<std::mutex> l(B(h)->m);
        B(h)->policy = *policy;
        B(h)->limit = nue_tex_budget_resolve(policy);
    }
    enforce(*B(h));
}

nue_result NUE_CALL nue_tex_budget_add(nue_tex_budget h, uint64_t bytes, uint32_t flags, nue_tex_resource_id* out_id) {
    if (!h || !out_id) return NUE_ERR_INVALID_ARGUMENT;
    Budget& b = *B(h);
#ifdef NUE_TEXTURE_WITH_ENGINE
    if (b.engine_id && nue_budget_request(b.engine_id, bytes) < 0) return NUE_ERR_OVERFLOW;       // accounted by the engine on success
#endif
    {
        std::lock_guard<std::mutex> l(b.m);
        uint32_t index;
        if (!b.free_list.empty()) { index = b.free_list.back(); b.free_list.pop_back(); }
        else { index = (uint32_t)b.res.size(); b.res.push_back(Res{0, 0, 0, 0, false}); }
        Res& r = b.res[index];
        r.bytes = bytes; r.flags = flags; r.tick = ++b.tick; r.used = true;
        b.used += bytes; b.peak = std::max(b.peak, b.used);
        ++b.count; if (flags & NUE_TEX_RES_PINNED) ++b.pinned;
        *out_id = make_id(index, r.gen);
    }
    enforce(b);
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_budget_resize(nue_tex_budget h, nue_tex_resource_id id, uint64_t bytes) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    Budget& b = *B(h);
    uint64_t old = 0;
    {
        std::lock_guard<std::mutex> l(b.m);
        Res* r = lookup(b, id);
        if (!r) return NUE_ERR_INVALID_HANDLE;
        old = r->bytes;
    }
#ifdef NUE_TEXTURE_WITH_ENGINE
    if (b.engine_id) {
        if (bytes > old) { if (nue_budget_request(b.engine_id, bytes - old) < 0) return NUE_ERR_OVERFLOW; }
        else if (bytes < old) nue_budget_report(b.engine_id, -int64_t(old - bytes));
    }
#endif
    {
        std::lock_guard<std::mutex> l(b.m);
        Res* r = lookup(b, id);
        if (!r) return NUE_ERR_INVALID_HANDLE;
        b.used = b.used - r->bytes + bytes; r->bytes = bytes; b.peak = std::max(b.peak, b.used);
    }
    enforce(b);
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_budget_remove(nue_tex_budget h, nue_tex_resource_id id) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    Budget& b = *B(h);
    std::lock_guard<std::mutex> l(b.m);
    Res* r = lookup(b, id);
    if (!r) return NUE_ERR_INVALID_HANDLE;
#ifdef NUE_TEXTURE_WITH_ENGINE
    if (b.engine_id) nue_budget_report(b.engine_id, -int64_t(r->bytes));        // never evicts, safe under our lock
#endif
    b.used -= r->bytes; --b.count; if (r->flags & NUE_TEX_RES_PINNED) --b.pinned;
    r->used = false; ++r->gen;
    b.free_list.push_back(uint32_t((id & ((1u << kIndexBits) - 1)) - 1));
    return NUE_OK;
}

void NUE_CALL nue_tex_budget_touch(nue_tex_budget h, nue_tex_resource_id id) {
    if (!h) return;
    Budget& b = *B(h);
    std::lock_guard<std::mutex> l(b.m);
    if (Res* r = lookup(b, id)) r->tick = ++b.tick;
}

nue_result NUE_CALL nue_tex_budget_set_flags(nue_tex_budget h, nue_tex_resource_id id, uint32_t flags) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    Budget& b = *B(h);
    std::lock_guard<std::mutex> l(b.m);
    Res* r = lookup(b, id);
    if (!r) return NUE_ERR_INVALID_HANDLE;
    if ((r->flags & NUE_TEX_RES_PINNED) && !(flags & NUE_TEX_RES_PINNED)) --b.pinned;
    if (!(r->flags & NUE_TEX_RES_PINNED) && (flags & NUE_TEX_RES_PINNED)) ++b.pinned;
    r->flags = flags;
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_budget_get_stats(nue_tex_budget h, nue_tex_budget_stats* out) {
    if (!h || !out) return NUE_ERR_INVALID_HANDLE;
    Budget& b = *B(h);
    std::lock_guard<std::mutex> l(b.m);
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    out->limit = b.limit; out->used = b.used; out->peak = b.peak; out->resources = b.count; out->pinned = b.pinned;
    out->evictions_requested = b.evictions_requested; out->bytes_evicted = b.bytes_evicted; out->over_budget_events = b.over_events;
    return NUE_OK;
}

uint64_t NUE_CALL nue_tex_budget_evict(nue_tex_budget h, uint64_t bytes_needed) { return h ? evict_bytes(*B(h), bytes_needed) : 0; }

#ifdef NUE_TEXTURE_WITH_ENGINE
static uint64_t NUE_CALL engine_evict(void* user, nue_budget_id, uint64_t needed, nue_memory_pressure level) {
    Budget& b = *B(reinterpret_cast<nue_tex_budget>(user));
    if (!needed) {
        uint64_t used;
        { std::lock_guard<std::mutex> l(b.m); used = b.used; }
        needed = level >= NUE_PRESSURE_CRITICAL ? used : level >= NUE_PRESSURE_HIGH ? used / 2 : used / 5;
    }
    return evict_bytes(b, needed);        // the owner's callback removes the resources, which reports the freed bytes to the engine
}

nue_result NUE_CALL nue_tex_budget_attach_engine(nue_tex_budget h, const char* name, uint32_t* out_id) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    Budget& b = *B(h);
    if (b.engine_id) return NUE_ERR_ALREADY_EXISTS;
    nue_budget_desc d;
    NUE_STRUCT_INIT(&d);
    d.name = name ? name : "texture"; d.kind = NUE_BUDGET_TEXTURE; d.evict = engine_evict; d.userdata = h;
    nue_budget_id id = 0;
    const nue_result r = nue_budget_register(&d, &id);
    if (r < 0) return r;
    uint64_t used;
    {
        std::lock_guard<std::mutex> l(b.m);
        used = b.used; b.limit = UINT64_MAX; b.engine_id = id;
    }
    if (used) nue_budget_report(id, int64_t(used));             // what was accounted before the attach
    if (out_id) *out_id = id;
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_budget_detach_engine(nue_tex_budget h) {
    if (!h) return NUE_ERR_INVALID_HANDLE;
    Budget& b = *B(h);
    uint32_t id;
    { std::lock_guard<std::mutex> l(b.m); id = b.engine_id; b.engine_id = 0; b.limit = nue_tex_budget_resolve(&b.policy); }
    return id ? nue_budget_unregister(id) : NUE_OK_NOOP;
}
#else
nue_result NUE_CALL nue_tex_budget_attach_engine(nue_tex_budget, const char*, uint32_t*) { return NUE_ERR_UNSUPPORTED; }
nue_result NUE_CALL nue_tex_budget_detach_engine(nue_tex_budget) { return NUE_ERR_UNSUPPORTED; }
#endif

uint64_t NUE_CALL nue_tex_memory_estimate(const nue_tex_memory_item* items, uint32_t n, nue_tex_format target) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const nue_tex_format f = target == NUE_TEX_FORMAT_UNKNOWN ? items[i].format : target;
        total += nue_tex_chain_size(f, items[i].width, items[i].height, 1, items[i].mip_levels, 1, 1) * (items[i].count ? items[i].count : 1u);
    }
    return total;
}

}  // extern "C"
