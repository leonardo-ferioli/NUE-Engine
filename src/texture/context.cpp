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

bool cache_exists(const Context& c, const std::string& key) {
    if (c.cache_dir.empty()) return false;
    std::error_code ec;
    return fs::exists(u8path(entry_path(c, key)), ec);
}

void index_add(Context& c, const std::string& path, const KeyValues& kv, const Ktx2Meta& m) {
    const auto it = kv.find("NUE.levels");
    if (it == kv.end() || !m.levels) return;
    const Fmt target = nue_tex_format_from_vk(m.vk_format);
    if (target == NUE_TEX_FORMAT_UNKNOWN) return;
    const std::string& s = it->second;
    std::lock_guard<std::mutex> l(c.index_mutex);
    size_t pos = 0;
    uint32_t idx = 0;
    while (pos + 32 <= s.size()) {
        const uint32_t level = idx % m.levels, face = (idx / m.levels) % m.faces, layer = idx / (m.levels * m.faces);
        c.level_index[s.substr(pos, 32) + ":" + std::to_string((int)target)] = LevelRef{path, level, layer, face};
        pos += 33;
        ++idx;
    }
}

void cache_store(Context& c, const std::string& key, const Image& im, const nue_tex_plan& plan, const std::string& level_hashes) {
    if (c.cache_dir.empty()) return;
    KeyValues kv;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", crc32(0, im.data.data(), im.data.size()));
    kv["NUE.key"] = key; kv["NUE.crc32"] = buf;
    kv["NUE.codec"] = std::to_string(NUE_TEX_CODEC_VERSION);
    kv["NUE.plan"] = plan.explanation;
    kv["NUE.levels"] = level_hashes;
    std::vector<uint8_t> bytes;
    if (ktx2_save(im, &kv, bytes) < 0) return;
    const std::string path = entry_path(c, key);
    make_dirs(path.substr(0, path.find_last_of('/')));
    if (!write_file_atomic(path.c_str(), bytes.data(), bytes.size())) { log(4, "cache: cannot write %s", path.c_str()); return; }
    ++c.stores; ++c.entries; c.store_bytes += bytes.size(); c.bytes += bytes.size();
    Ktx2Meta m;
    m.vk_format = nue_tex_format_to_vk(im.desc.format); m.levels = im.levels(); m.layers = im.layers(); m.faces = im.faces();
    index_add(c, path, kv, m);
    if (c.cache_max_bytes && c.bytes > c.cache_max_bytes) cache_trim(c, c.cache_max_bytes * 9 / 10);
}

}  // namespace

void cache_scan(Context& c) {
    if (c.cache_dir.empty()) return;
    uint64_t n = 0, bytes = 0;
    std::error_code ec;
    { std::lock_guard<std::mutex> l(c.index_mutex); c.level_index.clear(); c.loaded.clear(); }
    for (fs::recursive_directory_iterator it(u8path(c.cache_dir), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".ktx2") continue;
        ++n; bytes += it->file_size(ec);
        KeyValues kv;
        Ktx2Meta m;
        const std::string path = it->path().generic_string();
        if (ktx2_read_meta(path.c_str(), kv, m)) index_add(c, path, kv, m);
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

std::string level_hash(Fmt f, uint32_t w, uint32_t h, const void* data, size_t size) {
    Sha256 s;
    const char tag[] = "NUE-TEX-LEVEL";
    s.update(tag, sizeof tag);
    s.update(&f, sizeof f); s.update(&w, sizeof w); s.update(&h, sizeof h);
    s.update(data, size);
    uint8_t d[32];
    s.finish(d);
    return hex(d, 16);          // 128 bits are plenty to identify a level
}

static nue_result plan_for(const Context& c, const ConvertOptions& o, const nue_tex_caps& caps, nue_tex_plan* plan, nue_tex_alpha_info* alpha_out) {
    const Image& src = *o.source;
    nue_tex_plan_request rq; std::memset(&rq, 0, sizeof rq);
    rq.struct_size = sizeof rq;
    rq.source_format = src.desc.format; rq.width = src.desc.width; rq.height = src.desc.height; rq.depth = src.desc.depth;
    rq.mip_levels = src.levels(); rq.layers = src.layers(); rq.faces = src.faces();
    rq.usage = o.usage; rq.flags = o.flags | (src.desc.faces == 6 ? NUE_TEX_REQ_CUBE : 0u) | (src.desc.depth > 1 ? NUE_TEX_REQ_VOLUME : 0u);
    rq.policy = &o.policy;
    rq.alpha_info = o.alpha_info;
    if (rq.alpha_info == NUE_TEX_ALPHA_INFO_UNKNOWN && !o.no_scan) {
        nue_tex_scan_result sr; NUE_STRUCT_INIT(&sr);
        if (nue_tex_scan_level(src.desc.format, src.desc.width, src.desc.height, src.level_ptr(0, 0, 0), size_t(src.level_size(0, 0, 0)), &sr) >= 0) rq.alpha_info = sr.alpha_info;
    }
    (void)c;
    if (alpha_out) *alpha_out = rq.alpha_info;
    NUE_STRUCT_INIT(plan);
    return nue_tex_make_plan(&caps, &rq, plan);
}

nue_result context_convert(Context& c, const ConvertOptions& o, Image** out, nue_tex_plan* plan_out, bool* cache_hit, const nue_tex_caps* caps_override, bool store_only) {
    const Image& src = *o.source;
    if (cache_hit) *cache_hit = false;
    *out = nullptr;
    nue_tex_caps caps;
    if (caps_override) caps = *caps_override; else snapshot_caps(c, &caps);
    nue_tex_plan plan;
    nue_tex_alpha_info alpha = NUE_TEX_ALPHA_INFO_UNKNOWN;
    nue_result r = plan_for(c, o, caps, &plan, &alpha);
    if (plan_out) { const uint32_t keep = plan_out->struct_size; *plan_out = plan; plan_out->struct_size = keep ? keep : (uint32_t)sizeof plan; }
    if (r < 0) return r;
    log(3, "plan: %s", plan.explanation);

    // drop top mips first: cheap, and the cache key then sees only what is encoded
    const Image* work = &src;
    std::unique_ptr<Image> dropped;
    if (plan.skip_mips) {
        nue_tex_image tmp = nullptr;
        r = nue_tex_image_drop_top_mips(handle(const_cast<Image*>(&src)), plan.skip_mips, &tmp);
        if (r < 0) return r;
        dropped.reset(img(tmp)); work = dropped.get();
    }
    if (plan.action == NUE_TEX_ACTION_UPLOAD_AS_IS) { if (!store_only) *out = new Image(*work); return NUE_OK; }

    nue_tex_encode_params ep = encode_params_for(c, plan, o.usage, o.flags);
    nue_tex_mip_params mp; nue_tex_mip_params_default(&mp);
    const bool lossy = plan.action == NUE_TEX_ACTION_ENCODE || plan.action == NUE_TEX_ACTION_TRANSCODE;
    const bool cacheable = !c.cache_dir.empty() && lossy;
    std::string key;
    if (cacheable) {
        key = cache_key(*work, plan, ep, mp, uint32_t(o.usage));
        if (store_only && cache_exists(c, key)) { ++c.hits; if (cache_hit) *cache_hit = true; return NUE_OK; }
        Image* hit = nullptr;
        if (!store_only && cache_load(c, key, &hit)) {
            ++c.hits;
            if (cache_hit) *cache_hit = true;
            *out = hit;
            return NUE_OK;
        }
        ++c.misses;
    }
    if (lossy && !o.allow_encode) {
        log(4, "texture variant %s is not in the cold cache and run-time encoding is disabled", nue_tex_format_name(plan.target_format));
        return NUE_ERR_NOT_FOUND;
    }
    if (lossy && !o.cooking) ++c.runtime_encodes;
    ep.executor = reinterpret_cast<nue_tex_executor>(c.executor);
    Image* res = nullptr;
    r = image_convert(*work, plan.target_format, work->levels(), &ep, &mp, &res);
    if (r < 0) return r;
    if (cacheable) {
        // hashes of the SOURCE level behind every stored level, so a run-time lookup by content can find it
        std::string hashes;
        for (uint32_t layer = 0; layer < work->layers(); ++layer)
            for (uint32_t face = 0; face < work->faces(); ++face)
                for (uint32_t l = 0; l < work->levels(); ++l) {
                    if (!hashes.empty()) hashes += ',';
                    hashes += level_hash(work->desc.format, work->w(l), work->h(l), work->level_ptr(l, layer, face), size_t(work->level_size(l, layer, face)));
                }
        cache_store(c, key, *res, plan, hashes);
    }
    if (store_only) delete res; else *out = res;
    return NUE_OK;
}

bool lookup_level(Context& c, const std::string& hash, Fmt target, std::vector<uint8_t>& out) {
    LevelRef ref;
    std::shared_ptr<Image> file;
    Fmt used = target;
    {
        std::lock_guard<std::mutex> l(c.index_mutex);
        auto it = c.level_index.find(hash + ":" + std::to_string((int)target));
        if (it == c.level_index.end()) {
            // colour-space twins share their bytes for the formats whose encoders do not depend on it (everything but ASTC)
            const auto* ti = info(target);
            if (!ti || is_astc(target)) return false;
            const Fmt twin = ti->colorspace == NUE_TEX_COLORSPACE_SRGB ? ti->linear_twin : ti->srgb_twin;
            if (twin == target) return false;
            it = c.level_index.find(hash + ":" + std::to_string((int)twin));
            if (it == c.level_index.end()) return false;
            used = twin;
        }
        ref = it->second;
        for (auto li = c.loaded.begin(); li != c.loaded.end(); ++li)
            if (li->first == ref.path) { file = li->second; c.loaded.splice(c.loaded.begin(), c.loaded, li); break; }
    }
    if (!file) {
        std::vector<uint8_t> bytes;
        Image* im = nullptr;
        if (!read_file(ref.path.c_str(), bytes) || ktx2_load(bytes.data(), bytes.size(), 0, &im, nullptr) < 0) return false;
        file.reset(im);
        std::lock_guard<std::mutex> l(c.index_mutex);
        c.loaded.emplace_front(ref.path, file);
        while (c.loaded.size() > 6) c.loaded.pop_back();
    }
    if (ref.level >= file->levels() || ref.layer >= file->layers() || ref.face >= file->faces()) return false;
    (void)used;
    const uint8_t* p = file->level_ptr(ref.level, ref.layer, ref.face);
    out.assign(p, p + size_t(file->level_size(ref.level, ref.layer, ref.face)));
    return true;
}

// A variant is identified by what the bytes depend on. Colour-space twins of everything but ASTC hold the same bytes (the sRGB flag only changes how the GPU reads them).
static std::string variant_signature(const nue_tex_plan& plan) {
    Fmt t = plan.target_format;
    if (!is_astc(t)) { const auto* ti = info(t); if (ti) t = ti->linear_twin; }
    return std::to_string((int)t) + "/" + plan.shader_swizzle + "/" + std::to_string(plan.skip_mips) + "/" + std::to_string(plan.quality);
}

nue_result cook_image(Context& c, const ConvertOptions& in, uint32_t class_mask, nue_tex_cook_result* out) {
    ConvertOptions base = in;
    base.allow_encode = true; base.cooking = true;
    nue_tex_cook_result res; std::memset(&res, 0, sizeof res); res.struct_size = sizeof res;
    nue_tex_alpha_info scanned = NUE_TEX_ALPHA_INFO_UNKNOWN;
    {
        nue_tex_caps probe; nue_tex_caps_for_class(NUE_TEX_DEVICE_ANDROID_ASTC, &probe);
        nue_tex_plan p;
        plan_for(c, base, probe, &p, &scanned);                // only to scan the alpha once for all classes
    }
    // The D3D9 layer asks for a plan with what it knows: it may know the alpha (or not) and whether the texture is sampled as sRGB. Cook every combination it can ask for.
    struct Mode { nue_tex_alpha_info alpha; bool no_scan; bool srgb; };
    std::vector<Mode> modes;
    for (int srgb = 0; srgb < 2; ++srgb) {
        modes.push_back({scanned, false, srgb != 0});
        if (scanned != NUE_TEX_ALPHA_INFO_UNKNOWN) modes.push_back({NUE_TEX_ALPHA_INFO_UNKNOWN, true, srgb != 0});
    }
    std::vector<std::string> seen;
    for (int cls = 0; cls < NUE_TEX_DEVICE_COUNT; ++cls) {
        if (!(class_mask & (1u << cls))) continue;
        nue_tex_caps caps;
        nue_tex_caps_for_class(nue_tex_device_class(cls), &caps);
        for (const Mode& m : modes) {
            ConvertOptions o = base;
            o.alpha_info = m.alpha; o.no_scan = m.no_scan;
            o.flags = (base.flags & ~uint32_t(NUE_TEX_REQ_SRGB)) | (m.srgb ? uint32_t(NUE_TEX_REQ_SRGB) : 0u);
            nue_tex_plan plan;
            if (plan_for(c, o, caps, &plan, nullptr) < 0) continue;
            if (plan.action != NUE_TEX_ACTION_ENCODE && plan.action != NUE_TEX_ACTION_TRANSCODE) continue;       // nothing to store: as is, or a cheap decode at run time
            const std::string sig = variant_signature(plan) + (is_astc(plan.target_format) ? (m.srgb ? "/s" : "/l") : "");
            if (std::find(seen.begin(), seen.end(), sig) != seen.end()) continue;
            seen.push_back(sig);
            ++res.variants;
            Image* unused = nullptr;
            bool hit = false;
            const uint64_t before = c.store_bytes.load();
            const nue_result r = context_convert(c, o, &unused, nullptr, &hit, &caps, true);
            if (r < 0) { ++res.failed; log(4, "cook: %s failed (%d)", nue_tex_format_name(plan.target_format), (int)r); }
            else if (hit) ++res.already_cached;
            else { ++res.stored; res.bytes_written += c.store_bytes.load() - before; }
        }
    }
    if (out) { const uint32_t keep = out->struct_size; *out = res; out->struct_size = keep ? keep : (uint32_t)sizeof res; }
    return res.failed ? NUE_ERR_FAILED : NUE_OK;
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
    c->flags = NUE_STRUCT_HAS(desc, nue_tex_context_desc, flags) ? desc->flags : 0u;
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
    cache_scan(*c);                                       // builds the cold-cache index of what is already converted
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
    o.allow_encode = (c.flags & NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE) != 0;
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
    j->o.allow_encode = (c.flags & NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE) != 0;
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

nue_result NUE_CALL nue_tex_cook_image(nue_tex_context cx, const nue_tex_convert_request* rq, uint32_t class_mask, nue_tex_cook_result* out) {
    if (!cx || !rq || !rq->source) return NUE_ERR_INVALID_ARGUMENT;
    Context& c = *ctx(cx);
    ConvertOptions o;
    o.source = img(rq->source); o.usage = rq->usage; o.flags = rq->flags; o.alpha_info = rq->alpha_info;
    nue_tex_policy_default(&o.policy);
    if (rq->policy) std::memcpy(&o.policy, rq->policy, std::min<size_t>(rq->policy->struct_size, sizeof o.policy)); else o.policy = c.policy;
    return cook_image(c, o, class_mask ? class_mask : NUE_TEX_CLASS_MASK_ALL, out);
}

nue_result NUE_CALL nue_tex_context_get_runtime_stats(nue_tex_context cx, nue_tex_runtime_stats* out) {
    if (!cx || !out) return NUE_ERR_INVALID_HANDLE;
    Context& c = *ctx(cx);
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    out->level_hits = c.level_hits; out->level_misses = c.level_misses; out->runtime_encodes = c.runtime_encodes;
    std::lock_guard<std::mutex> l(c.index_mutex);
    out->indexed_levels = c.level_index.size();
    return NUE_OK;
}

void NUE_CALL nue_tex_wait_idle(nue_tex_context cx) { if (cx) ctx(cx)->executor->wait_idle(); }

}  // extern "C"
