#pragma once
#include "executor.hpp"
#include "container.hpp"
#include <atomic>
#include <chrono>
#include <list>
#include <map>
#include <unordered_map>

namespace nuetex {

struct LevelRef { std::string path; uint32_t level, layer, face; };

struct Context {
    nue_tex_caps caps;
    nue_tex_policy policy;
    std::mutex mutex;                   // caps, async table
    std::string cache_dir;
    uint64_t cache_max_bytes = 0;
    uint32_t flags = 0;                 // NUE_TEX_CONTEXT_*
    Executor* executor = nullptr;
    bool owns_executor = false;
    nue_tex_budget_policy budget_policy;

    // cache statistics
    std::atomic<uint64_t> hits{0}, misses{0}, stores{0}, corrupt{0}, evicted{0}, hit_bytes{0}, store_bytes{0};
    std::atomic<uint64_t> entries{0}, bytes{0};
    std::atomic<bool> scanned{false};

    // cold cache: source level hash + target format -> (file, level)
    std::mutex index_mutex;
    std::unordered_map<std::string, LevelRef> level_index;
    std::atomic<bool> index_dirty{false};
    std::atomic<uint64_t> level_hits{0}, level_misses{0}, runtime_encodes{0};

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
    bool allow_encode = false;          // lossy encodes may run now (import/cook, or the context allows run-time encoding)
    bool no_scan = false;               // alpha_info UNKNOWN is taken literally (do not scan the content): the plan the D3D9 layer asks for when it knows nothing
    bool cooking = false;               // the encode is part of a cold import (not counted as a run-time encode)
};

// caps_override: plan for that device instead of the context's (cooking); store_only: make sure the variant is in the cache and return no image.
nue_result context_convert(Context& c, const ConvertOptions& o, Image** out, nue_tex_plan* plan, bool* cache_hit, const nue_tex_caps* caps_override = nullptr, bool store_only = false);
nue_result cook_image(Context& c, const ConvertOptions& o, uint32_t class_mask, nue_tex_cook_result* out);
std::string level_hash(Fmt f, uint32_t w, uint32_t h, const void* data, size_t size);      // 32 hex characters
bool lookup_level(Context& c, const std::string& hash, Fmt target, std::vector<uint8_t>& out);
void cache_scan(Context& c);
void index_save(Context& c);                                                              // writes <cache>/index.nuei (done after a cook and at shutdown)
bool index_load(Context& c);
void cache_trim(Context& c, uint64_t max_bytes);
nue_tex_encode_params encode_params_for(const Context& c, const nue_tex_plan& plan, nue_tex_usage usage, uint32_t req_flags);
void snapshot_caps(Context& c, nue_tex_caps* out);

}  // namespace nuetex
