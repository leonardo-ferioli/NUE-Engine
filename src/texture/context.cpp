// Context: caps + policy + executor + on-disk cache; plan-driven image conversion; asynchronous requests.
#include "context.hpp"
#include <algorithm>
#include <filesystem>

namespace nuetex {

namespace fs = std::filesystem;

Context::~Context() {
    if (executor) { executor->wait_idle(); if (owns_executor) delete executor; }
}

namespace {

fs::path u8path(const std::string& s) {
#ifdef _WIN32
    return fs::u8path(s);
#else
    return fs::path(s);
#endif
}

std::string hex(const uint8_t* d, size_t n) { static const char* h = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; ++i) { s += h[d[i] >> 4]; s += h[d[i] & 15]; } return s; }

template <class T> void feed(Sha256& h, const T& v) { h.update(&v, sizeof v); }

// Content hash key: source bytes + everything that changes the produced bytes + codec version.
std::string cache_key(const Image& src, const nue_tex_plan& plan, const nue_tex_encode_params& ep, const nue_tex_mip_params& mp, uint32_t usage) {
    Sha256 h;
    const char tag[] = "NUE-TEX-CACHE";
    h.update(tag, sizeof tag);
    const uint32_t ver = NUE_TEX_CODEC_VERSION;
    feed(h, ver);
    feed(h, src.desc.format); feed(h, src.desc.width); feed(h, src.desc.height); feed(h, src.desc.depth);
    feed(h, src.desc.mip_levels); feed(h, src.desc.layers); feed(h, src.desc.faces);
    h.update(src.data.data(), src.data.size());
    feed(h, plan.target_format); feed(h, plan.action); feed(h, plan.skip_mips); feed(h, plan.quality);
    h.update(plan.shader_swizzle, 5);
    feed(h, ep.quality); feed(h, ep.flags); feed(h, ep.alpha_threshold); h.update(ep.swizzle, 5);
    feed(h, mp.filter); feed(h, mp.flags);
    feed(h, usage);
    uint8_t d[32];
    h.finish(d);
    return hex(d, 32);
}

std::string entry_path(const Context& c, const std::string& key) { return c.cache_dir + "/" + key.substr(0, 2) + "/" + key.substr(2, 2) + "/" + key + ".ktx2"; }

bool cache_load(Context& c, const std::string& key, Image** out) {
    if (c.cache_dir.empty()) return false;
    const std::string path = entry_path(c, key);
    std::vector<uint8_t> bytes;
    if (!read_file(path.c_str(), bytes)) return false;
    KeyValues kv;
    Image* im = nullptr;
    nue_result r = ktx2_load(bytes.data(), bytes.size(), 0, &im, &kv);
    bool ok = r >= 0 && kv.count("NUE.key") && kv["NUE.key"] == key && kv.count("NUE.crc32");
    if (ok) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", crc32(0, im->data.data(), im->data.size()));
        ok = kv["NUE.crc32"] == buf;
    }
    if (!ok) {
        delete im;
        remove_file(path.c_str());
        ++c.corrupt;
        log(4, "cache entry %s is corrupt: removed", key.c_str());
        return false;
    }
    std::error_code ec;
    fs::last_write_time(u8path(path), fs::file_time_type::clock::now(), ec);        // LRU touch
    *out = im;
    c.hit_bytes += bytes.size();
    return true;
}

void cache_store(Context& c, const std::string& key, const Image& im, const nue_tex_plan& plan) {
    if (c.cache_dir.empty()) return;
    KeyValues kv;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", crc32(0, im.data.data(), im.data.size()));
    kv["NUE.key"] = key; kv["NUE.crc32"] = buf;
    kv["NUE.codec"] = std::to_string(NUE_TEX_CODEC_VERSION);
    kv["NUE.plan"] = plan.explanation;
    std::vector<uint8_t> bytes;
    if (ktx2_save(im, &kv, bytes) < 0) return;
    const std::string path = entry_path(c, key);
    make_dirs(path.substr(0, path.find_last_of('/')));
    if (!write_file_atomic(path.c_str(), bytes.data(), bytes.size())) { log(4, "cache: cannot write %s", path.c_str()); return; }
    ++c.stores; ++c.entries; c.store_bytes += bytes.size(); c.bytes += bytes.size();
    if (c.cache_max_bytes && c.bytes > c.cache_max_bytes) cache_trim(c, c.cache_max_bytes * 9 / 10);
}

}  // namespace

void cache_scan(Context& c) {
    if (c.cache_dir.empty()) return;
    uint64_t n = 0, bytes = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(u8path(c.cache_dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ktx2") continue;
        ++n; bytes += it->file_size(ec);
    }
    c.entries = n; c.bytes = bytes; c.scanned = true;
}

void cache_trim(Context& c, uint64_t max_bytes) {
    if (c.cache_dir.empty()) return;
    struct E { fs::path p; uint64_t size; fs::file_time_type t; };
    std::vector<E> files;
    std::error_code ec;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator it(u8path(c.cache_dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ktx2") continue;
        E e{it->path(), it->file_size(ec), it->last_write_time(ec)};
        total += e.size; files.push_back(std::move(e));
    }
    std::sort(files.begin(), files.end(), [](const E& a, const E& b) { return a.t < b.t; });
    size_t i = 0;
    while (total > max_bytes && i < files.size()) {
        if (fs::remove(files[i].p, ec)) { total -= files[i].size; ++c.evicted; }
        ++i;
    }
    uint64_t n = 0;
    for (size_t k = i; k < files.size(); ++k) ++n;
    c.entries = n; c.bytes = total;
}

void snapshot_caps(Context& c, nue_tex_caps* out) { std::lock_guard<std::mutex> l(c.mutex); *out = c.caps; }

nue_tex_encode_params encode_params_for(const Context& c, const nue_tex_plan& plan, nue_tex_usage usage, uint32_t req_flags) {
    (void)c;
    nue_tex_encode_params ep;
    nue_tex_encode_params_default(&ep);
    ep.quality = nue_tex_quality(std::min<uint32_t>(plan.quality, 4));
    if (usage == NUE_TEX_USAGE_NORMAL_MAP) ep.flags |= NUE_TEX_ENC_NORMAL_MAP;
    if (usage == NUE_TEX_USAGE_COLOR || usage == NUE_TEX_USAGE_UI) ep.flags |= NUE_TEX_ENC_PERCEPTUAL;
    if (req_flags & NUE_TEX_REQ_SRGB) ep.flags |= NUE_TEX_ENC_SRGB;
    if (std::strcmp(plan.shader_swizzle, "rrr1") == 0) std::memcpy(ep.swizzle, "rrr1", 5);
    return ep;
}

nue_result context_convert(Context& c, const ConvertOptions& o, Image** out, nue_tex_plan* plan_out, bool* cache_hit) {
    const Image& src = *o.source;
    if (cache_hit) *cache_hit = false;
    nue_tex_caps caps;
    snapshot_caps(c, &caps);
    nue_tex_plan_request rq; std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.source_format = src.desc.format; rq.width = src.desc.width; rq.height = src.desc.height; rq.depth = src.desc.depth;
    rq.mip_levels = src.levels(); rq.layers = src.layers(); rq.faces = src.faces();
    rq.usage = o.usage; rq.flags = o.flags | (src.desc.faces == 6 ? NUE_TEX_REQ_CUBE : 0u) | (src.desc.depth > 1 ? NUE_TEX_REQ_VOLUME : 0u);
    rq.policy = &o.policy;
    rq.alpha_info = o.alpha_info;
    if (rq.alpha_info == NUE_TEX_ALPHA_INFO_UNKNOWN) {
        nue_tex_scan_result sr; NUE_STRUCT_INIT(&sr);
        if (nue_tex_scan_level(src.desc.format, src.desc.width, src.desc.height, src.level_ptr(0, 0, 0), size_t(src.level_size(0, 0, 0)), &sr) >= 0) rq.alpha_info = sr.alpha_info;
    }
    nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
    nue_result r = nue_tex_make_plan(&caps, &rq, &plan);
    if (plan_out) { const uint32_t keep = plan_out->struct_size; *plan_out = plan; plan_out->struct_size = keep ? keep : (uint32_t)sizeof plan; }
    if (r < 0) return r;
    log(3, "plan: %s", plan.explanation);

    // drop top mips first: cheap, and the cache key then sees only what is encoded
    const Image* work = &src;
    std::unique_ptr<Image> dropped;
    if (plan.skip_mips) {
        Image* d = nullptr;
        nue_tex_image tmp = nullptr;
        r = nue_tex_image_drop_top_mips(handle(const_cast<Image*>(&src)), plan.skip_mips, &tmp);
        if (r < 0) return r;
        d = img(tmp); dropped.reset(d); work = d;
    }
    if (plan.action == NUE_TEX_ACTION_UPLOAD_AS_IS) { *out = new Image(*work); return NUE_OK; }

    nue_tex_encode_params ep = encode_params_for(c, plan, o.usage, o.flags);
    nue_tex_mip_params mp; nue_tex_mip_params_default(&mp);
    std::string key;
    const bool cacheable = !c.cache_dir.empty() && (plan.action == NUE_TEX_ACTION_ENCODE || plan.action == NUE_TEX_ACTION_TRANSCODE);
    if (cacheable) {
        key = cache_key(*work, plan, ep, mp, uint32_t(o.usage));
        Image* hit = nullptr;
        if (cache_load(c, key, &hit)) {
            ++c.hits;
            if (cache_hit) *cache_hit = true;
            *out = hit;
            return NUE_OK;
        }
        ++c.misses;
    }
    ep.executor = reinterpret_cast<nue_tex_executor>(c.executor);
    Image* res = nullptr;
    r = image_convert(*work, plan.target_format, work->levels(), &ep, &mp, &res);
    if (r < 0) return r;
    if (cacheable) cache_store(c, key, *res, plan);
    *out = res;
    return NUE_OK;
}

}  // namespace nuetex

using namespace nuetex;

extern "C" {

nue_result NUE_CALL nue_tex_context_create(const nue_tex_context_desc* desc, nue_tex_context* out) {
    if (!desc || !desc->caps || !out) return NUE_ERR_INVALID_ARGUMENT;
    auto c = std::make_unique<Context>();
    c->caps = *desc->caps;
    nue_tex_policy_default(&c->policy);
    if (desc->policy) std::memcpy(&c->policy, desc->policy, std::min<size_t>(desc->policy->struct_size, sizeof c->policy));
    if (desc->cache_dir && *desc->cache_dir) {
        c->cache_dir = desc->cache_dir;
        while (!c->cache_dir.empty() && (c->cache_dir.back() == '/' || c->cache_dir.back() == '\\')) c->cache_dir.pop_back();
        if (!make_dirs(c->cache_dir)) { log(4, "cache directory %s cannot be created: cache disabled", c->cache_dir.c_str()); c->cache_dir.clear(); }
    }
    c->cache_max_bytes = desc->cache_max_bytes;
    c->budget_policy = desc->budget;
    if (desc->executor && desc->executor->submit) {
        c->executor = new Executor(*desc->executor);
        c->owns_executor = true;
    } else {
        nue_tex_executor_desc ed{};
        ed.struct_size = sizeof ed;
        ed.threads = desc->worker_threads;
        ed.name = "nue-tex";
        if (desc->executor) ed = *desc->executor;
        if (desc->worker_threads) ed.threads = desc->worker_threads;
        c->executor = new Executor(ed);
        c->owns_executor = true;
    }
    *out = reinterpret_cast<nue_tex_context>(c.release());
    return NUE_OK;
}

void NUE_CALL nue_tex_context_destroy(nue_tex_context cx) { delete ctx(cx); }

nue_result NUE_CALL nue_tex_context_get_caps(nue_tex_context cx, nue_tex_caps* out) {
    if (!cx || !out) return NUE_ERR_INVALID_HANDLE;
    snapshot_caps(*ctx(cx), out);
    return NUE_OK;
}
nue_result NUE_CALL nue_tex_context_set_caps(nue_tex_context cx, const nue_tex_caps* caps) {
    if (!cx || !caps) return NUE_ERR_INVALID_HANDLE;
    std::lock_guard<std::mutex> l(ctx(cx)->mutex);
    ctx(cx)->caps = *caps;
    return NUE_OK;
}
nue_tex_executor NUE_CALL nue_tex_context_executor(nue_tex_context cx) { return cx ? reinterpret_cast<nue_tex_executor>(ctx(cx)->executor) : nullptr; }

nue_result NUE_CALL nue_tex_cache_get_stats(nue_tex_context cx, nue_tex_cache_stats* out) {
    if (!cx || !out) return NUE_ERR_INVALID_HANDLE;
    Context& c = *ctx(cx);
    if (!c.scanned) cache_scan(c);
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    out->hits = c.hits; out->misses = c.misses; out->stores = c.stores; out->corrupt_dropped = c.corrupt; out->evicted = c.evicted;
    out->entries = c.entries; out->bytes = c.bytes; out->hit_bytes = c.hit_bytes; out->store_bytes = c.store_bytes;
    return NUE_OK;
}
nue_result NUE_CALL nue_tex_cache_clear(nue_tex_context cx) {
    if (!cx) return NUE_ERR_INVALID_HANDLE;
    cache_trim(*ctx(cx), 0);
    return NUE_OK;
}
nue_result NUE_CALL nue_tex_cache_trim(nue_tex_context cx, uint64_t max_bytes) {
    if (!cx) return NUE_ERR_INVALID_HANDLE;
    cache_trim(*ctx(cx), max_bytes);
    return NUE_OK;
}
nue_result NUE_CALL nue_tex_cache_scan(nue_tex_context cx) {
    if (!cx) return NUE_ERR_INVALID_HANDLE;
    cache_scan(*ctx(cx));
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_context_convert(nue_tex_context cx, const nue_tex_convert_request* rq, nue_tex_image* out_image, nue_tex_plan* out_plan, nue_bool* out_hit) {
    if (!cx || !rq || !rq->source || !out_image) return NUE_ERR_INVALID_ARGUMENT;
    Context& c = *ctx(cx);
    ConvertOptions o;
    o.source = img(rq->source); o.usage = rq->usage; o.flags = rq->flags; o.alpha_info = rq->alpha_info;
    nue_tex_policy_default(&o.policy);
    const nue_tex_policy* pol = rq->policy;
    if (pol) std::memcpy(&o.policy, pol, std::min<size_t>(pol->struct_size, sizeof o.policy));
    else o.policy = c.policy;
    Image* res = nullptr;
    bool hit = false;
    const nue_result r = context_convert(c, o, &res, out_plan, &hit);
    if (out_hit) *out_hit = hit ? NUE_TRUE : NUE_FALSE;
    *out_image = r >= 0 ? handle(res) : nullptr;
    return r;
}

nue_result NUE_CALL nue_tex_request_async(nue_tex_context cx, const nue_tex_convert_request* rq, nue_tex_job_priority priority, nue_tex_done_fn done, void* userdata, nue_tex_request_id* out_id) {
    if (!cx || !rq || !rq->source || !done) return NUE_ERR_INVALID_ARGUMENT;
    Context& c = *ctx(cx);
    struct Job {
        Context* c; std::shared_ptr<Context::Request> state; uint32_t id; std::unique_ptr<Image> source; ConvertOptions o; nue_tex_done_fn done; void* user;
    };
    auto* j = new Job{&c, std::make_shared<Context::Request>(), c.next_request.fetch_add(1), std::make_unique<Image>(*img(rq->source)), {}, done, userdata};
    if (j->id == 0) j->id = c.next_request.fetch_add(1);
    j->o.source = j->source.get(); j->o.usage = rq->usage; j->o.flags = rq->flags; j->o.alpha_info = rq->alpha_info;
    if (rq->policy) { nue_tex_policy_default(&j->o.policy); std::memcpy(&j->o.policy, rq->policy, std::min<size_t>(rq->policy->struct_size, sizeof j->o.policy)); }
    else j->o.policy = c.policy;
    { std::lock_guard<std::mutex> l(c.mutex); c.requests[j->id] = j->state; }
    if (out_id) *out_id = j->id;
    c.executor->submit([](void* u) {
        std::unique_ptr<Job> j(static_cast<Job*>(u));
        int expected = 0;
        nue_result r = NUE_ERR_CANCELLED;
        Image* res = nullptr;
        nue_tex_plan plan; NUE_STRUCT_INIT(&plan);
        if (j->state->state.compare_exchange_strong(expected, 1)) r = context_convert(*j->c, j->o, &res, &plan, nullptr);
        { std::lock_guard<std::mutex> l(j->c->mutex); j->c->requests.erase(j->id); }
        j->done(j->user, j->id, r, r >= 0 ? handle(res) : nullptr, &plan);
    }, j, priority);
    return NUE_OK_PENDING;
}

nue_result NUE_CALL nue_tex_request_cancel(nue_tex_context cx, nue_tex_request_id id) {
    if (!cx) return NUE_ERR_INVALID_HANDLE;
    Context& c = *ctx(cx);
    std::shared_ptr<Context::Request> st;
    { std::lock_guard<std::mutex> l(c.mutex); auto it = c.requests.find(id); if (it == c.requests.end()) return NUE_OK_NOOP; st = it->second; }
    int expected = 0;
    return st->state.compare_exchange_strong(expected, 2) ? NUE_OK : NUE_OK_NOOP;
}

void NUE_CALL nue_tex_wait_idle(nue_tex_context cx) { if (cx) ctx(cx)->executor->wait_idle(); }

}  // extern "C"
