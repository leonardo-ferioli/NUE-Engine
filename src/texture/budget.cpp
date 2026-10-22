// Texture memory budget: byte accounting with LRU eviction candidates, policy presets, statistics, plus the memory estimate model.
#include "internal.hpp"
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

// Called with the lock NOT held; asks the owner to release at least `need` bytes.
void enforce(Budget& b) {
    for (int round = 0; round < 4; ++round) {
        std::vector<nue_tex_resource_id> cand;
        uint64_t need = 0;
        nue_tex_evict_fn fn;
        void* user;
        {
            std::lock_guard<std::mutex> l(b.m);
            if (b.used <= b.limit || !b.evict || b.evicting) return;
            need = b.used - b.limit;
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

uint64_t NUE_CALL nue_tex_memory_estimate(const nue_tex_memory_item* items, uint32_t n, nue_tex_format target) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const nue_tex_format f = target == NUE_TEX_FORMAT_UNKNOWN ? items[i].format : target;
        total += nue_tex_chain_size(f, items[i].width, items[i].height, 1, items[i].mip_levels, 1, 1) * (items[i].count ? items[i].count : 1u);
    }
    return total;
}

}  // extern "C"
