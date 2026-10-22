#pragma once
#include "executor.hpp"
#include "container.hpp"
#include <atomic>
#include <chrono>
#include <map>

namespace nuetex {

struct Context {
    nue_tex_caps caps;
    nue_tex_policy policy;
    std::mutex mutex;                   // caps, async table
    std::string cache_dir;
    uint64_t cache_max_bytes = 0;
    Executor* executor = nullptr;
    bool owns_executor = false;
    nue_tex_budget_policy budget_policy;

    // cache statistics
    std::atomic<uint64_t> hits{0}, misses{0}, stores{0}, corrupt{0}, evicted{0}, hit_bytes{0}, store_bytes{0};
    std::atomic<uint64_t> entries{0}, bytes{0};
    std::atomic<bool> scanned{false};

    // async requests
    struct Request { std::atomic<int> state{0}; };      // 0 queued, 1 running, 2 cancelled
    std::map<uint32_t, std::shared_ptr<Request>> requests;
    std::atomic<uint32_t> next_request{1};

    ~Context();
};

inline Context* ctx(nue_tex_context c) { return reinterpret_cast<Context*>(c); }

struct ConvertOptions {
    const Image* source;
    nue_tex_usage usage;
    uint32_t flags;
    nue_tex_policy policy;
    nue_tex_alpha_info alpha_info;
};

nue_result context_convert(Context& c, const ConvertOptions& o, Image** out, nue_tex_plan* plan, bool* cache_hit);
void cache_scan(Context& c);
void cache_trim(Context& c, uint64_t max_bytes);
nue_tex_encode_params encode_params_for(const Context& c, const nue_tex_plan& plan, nue_tex_usage usage, uint32_t req_flags);
void snapshot_caps(Context& c, nue_tex_caps* out);

}  // namespace nuetex
