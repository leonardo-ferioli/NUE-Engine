// Framework basics: memory (budgeted buffers), sources, sinks, the registry of codecs and formats, the context.
#include "internal.hpp"

#include <cctype>
#include <cstdlib>

namespace nuea {

// ---------------------------------------------------------------------------------------------------- budget
nue_result Ctx::charge(uint64_t bytes) {
    if (!use_budget || !budget || bytes == 0) return NUE_OK;
    nue_result r = nue_budget_request(budget, bytes);
    if (r == NUE_ERR_OVERFLOW) { nue_set_last_error("assets: %llu bytes do not fit the '%s' memory budget", (unsigned long long)bytes, budget_name.c_str()); return NUE_ERR_OUT_OF_MEMORY; }
    return NUE_OK;                                           // not initialised / unknown budget: nothing to account
}

void Ctx::credit(uint64_t bytes) {
    if (use_budget && budget && bytes) nue_budget_report(budget, -int64_t(bytes));
}

// ---------------------------------------------------------------------------------------------------- Buffer
nue_result Buffer::alloc(Ctx* ctx, size_t size) {
    release();
    ctx_ = ctx;
    if (size == 0) return NUE_OK;
    if (ctx && ctx->charge(size) != NUE_OK) return NUE_ERR_OUT_OF_MEMORY;
    p_ = static_cast<uint8_t*>(nue_alloc(size, 16));
    if (!p_) { if (ctx) ctx->credit(size); nue_set_last_error("assets: out of memory allocating %llu bytes", (unsigned long long)size); return NUE_ERR_OUT_OF_MEMORY; }
    std::memset(p_, 0, size);
    n_ = cap_ = size;
    return NUE_OK;
}

nue_result Buffer::reserve(size_t capacity) {
    if (capacity <= cap_) return NUE_OK;
    if (ctx_ && ctx_->charge(capacity - cap_) != NUE_OK) return NUE_ERR_OUT_OF_MEMORY;
    uint8_t* np = static_cast<uint8_t*>(nue_alloc(capacity, 16));
    if (!np) { if (ctx_) ctx_->credit(capacity - cap_); nue_set_last_error("assets: out of memory allocating %llu bytes", (unsigned long long)capacity); return NUE_ERR_OUT_OF_MEMORY; }
    if (n_) std::memcpy(np, p_, n_);
    if (capacity > n_) std::memset(np + n_, 0, capacity - n_);
    if (p_) nue_free(p_);
    p_ = np;
    cap_ = capacity;
    return NUE_OK;
}

nue_result Buffer::append(const void* data, size_t size) {
    if (size == 0) return NUE_OK;
    if (n_ + size < n_) return NUE_ERR_OVERFLOW;
    if (n_ + size > cap_) {
        size_t want = cap_ ? cap_ : 4096;
        while (want < n_ + size) { if (want > SIZE_MAX / 2) { want = n_ + size; break; } want *= 2; }
        nue_result r = reserve(want);
        if (r != NUE_OK) return r;
    }
    std::memcpy(p_ + n_, data, size);
    n_ += size;
    return NUE_OK;
}

nue_result Buffer::resize(size_t size) {
    if (size > cap_) { nue_result r = reserve(size); if (r != NUE_OK) return r; }
    if (size > n_) std::memset(p_ + n_, 0, size - n_);
    n_ = size;
    return NUE_OK;
}

uint8_t* Buffer::detach(size_t* out_size) {
    uint8_t* p = p_;
    if (out_size) *out_size = n_;
    if (p_ && ctx_) ctx_->credit(cap_);
    p_ = nullptr; n_ = cap_ = 0;
    return p;
}

void Buffer::release() {
    if (p_) { nue_free(p_); if (ctx_) ctx_->credit(cap_); }
    p_ = nullptr; n_ = cap_ = 0;
}

// ---------------------------------------------------------------------------------------------------- sources
nue_result MemSource::read(uint64_t offset, void* buffer, size_t size, size_t* out_read) const {
    size_t got = 0;
    if (offset < size_) { got = size_t(std::min<uint64_t>(size, size_ - offset)); if (got) std::memcpy(buffer, data_ + offset, got); }
    if (out_read) *out_read = got;
    return got == size ? NUE_OK : NUE_OK_PARTIAL;
}

nue_result FileSource::open(const char* path, std::unique_ptr<FileSource>* out) {
    std::unique_ptr<FileSource> s(new FileSource());
    nue_result r = nue_file_open(path, NUE_OPEN_READ | NUE_OPEN_RANDOM, &s->file_);
    if (r < 0) return r;
    r = nue_file_size(s->file_, &s->size_);
    if (r < 0) return r;
    *out = std::move(s);
    return NUE_OK;
}

nue_result FileSource::read(uint64_t offset, void* buffer, size_t size, size_t* out_read) const {
    size_t got = 0;
    nue_result r = NUE_OK;
    if (size && offset < size_) {
        r = nue_file_read_at(file_, offset, buffer, size, &got);
        if (r < 0 && r != NUE_ERR_END_OF_DATA) { if (out_read) *out_read = 0; return r; }
    }
    if (out_read) *out_read = got;
    return got == size ? NUE_OK : NUE_OK_PARTIAL;
}

// ---------------------------------------------------------------------------------------------------- sink
nue_result MemSink::write(const void* data, size_t size) { return buf_.append(data, size); }

nue_result MemSink::patch(uint64_t offset, const void* data, size_t size) {
    if (offset > buf_.size() || size > buf_.size() - offset) return NUE_ERR_INVALID_ARGUMENT;
    if (size) std::memcpy(buf_.data() + offset, data, size);
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------- util
uint64_t name_hash64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; s && *s; ++s) { h ^= uint8_t(std::tolower(uint8_t(*s))); h *= 1099511628211ull; }
    return h;
}

bool iequals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) if (std::tolower(uint8_t(*a)) != std::tolower(uint8_t(*b))) return false;
    return *a == *b;
}

std::string lower_ext(const char* path) {
    if (!path) return std::string();
    const char* dot = nullptr;
    for (const char* p = path; *p; ++p) { if (*p == '.') dot = p; else if (*p == '/' || *p == '\\' || *p == ':') dot = nullptr; }
    std::string e = dot ? dot : "";
    for (char& c : e) c = char(std::tolower(uint8_t(c)));
    return e;
}

// ---------------------------------------------------------------------------------------------------- registry lookups
const CodecRec* Ctx::find_codec(const char* name) const {
    if (!name) return nullptr;
    for (auto& c : codecs) if (iequals(c->name.c_str(), name)) return c.get();
    return nullptr;
}

const FormatRec* Ctx::find_format(const char* name) const {
    if (!name) return nullptr;
    for (auto& f : formats) if (iequals(f->name.c_str(), name)) return f.get();
    return nullptr;
}

static bool ext_listed(const std::string& list, const std::string& ext) {
    if (ext.empty()) return false;
    std::string e = ext.substr(1);                           // "bun"
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(';', i);
        if (j == std::string::npos) j = list.size();
        std::string item = list.substr(i, j - i);
        for (char& c : item) c = char(std::tolower(uint8_t(c)));
        if (!item.empty() && item == e) return true;
        i = j + 1;
    }
    return false;
}

const FormatRec* Ctx::probe_format(const uint8_t* head, size_t head_size, uint64_t total, const char* ext_hint) const {
    const FormatRec* best = nullptr;
    int best_score = 0;
    for (auto& f : formats) {
        if (!f->format.probe) continue;
        int s = f->format.probe(f->format.userdata, head, head_size, total);
        if (s > best_score) { best_score = s; best = f.get(); }
    }
    if (!best && total == 0 && ext_hint) {                   // an empty file has no content to recognise: the extension decides
        const std::string e = lower_ext(ext_hint);
        for (auto& f : formats) if (ext_listed(f->extensions, e)) return f.get();
    }
    return best;
}

}  // namespace nuea

using namespace nuea;

// ---------------------------------------------------------------------------------------------------- public: sources and sinks
NUE_ASSETS_API nue_result NUE_CALL nue_asset_source_read(const nue_asset_source* source, uint64_t offset, void* buffer, size_t size, size_t* out_read) {
    if (!source || (!buffer && size)) return NUE_ERR_INVALID_ARGUMENT;
    return source->read(offset, buffer, size, out_read);
}
NUE_ASSETS_API uint64_t NUE_CALL nue_asset_source_size(const nue_asset_source* source) { return source ? source->size() : 0; }
NUE_ASSETS_API const void* NUE_CALL nue_asset_source_peek(const nue_asset_source* source, uint64_t offset, size_t size) { return source ? source->peek(offset, size) : nullptr; }

NUE_ASSETS_API nue_result NUE_CALL nue_asset_sink_write(nue_asset_sink* sink, const void* data, size_t size) { return sink ? sink->write(data, size) : NUE_ERR_INVALID_ARGUMENT; }
NUE_ASSETS_API uint64_t NUE_CALL nue_asset_sink_position(const nue_asset_sink* sink) { return sink ? sink->position() : 0; }
NUE_ASSETS_API nue_result NUE_CALL nue_asset_sink_patch(nue_asset_sink* sink, uint64_t offset, const void* data, size_t size) { return sink ? sink->patch(offset, data, size) : NUE_ERR_INVALID_ARGUMENT; }

// ---------------------------------------------------------------------------------------------------- public: JDLZ as a registered codec
static nue_result NUE_CALL jdlz_identify_cb(void*, const void* data, size_t size, uint64_t* out_unc, uint64_t* out_stream) {
    uint32_t u = 0, s = 0;
    nue_result r = nue_jdlz_identify(data, size, &u, &s);
    if (r == NUE_OK) { if (out_unc) *out_unc = u; if (out_stream) *out_stream = s; }
    return r;
}
static nue_result NUE_CALL jdlz_decompress_cb(void*, const void* src, size_t n, void* dst, size_t cap, size_t* out) { return nue_jdlz_decompress(src, n, dst, cap, out); }
static nue_result NUE_CALL jdlz_compress_cb(void*, const void* src, size_t n, const nue_asset_compress_params* p, void* dst, size_t cap, size_t* out) {
    return nue_jdlz_compress(src, n, p ? p->level : 6, dst, cap, out);
}
static size_t NUE_CALL jdlz_bound_cb(void*, size_t n) { return nue_jdlz_compress_bound(n); }

static nue_result register_jdlz(nue_assets_context ctx) {
    nue_asset_codec c;
    NUE_STRUCT_INIT(&c);
    c.name = "jdlz";
    c.identify = jdlz_identify_cb;
    c.decompress = jdlz_decompress_cb;
    c.compress = jdlz_compress_cb;
    c.compress_bound = jdlz_bound_cb;
    nue_result r = nue_assets_register_codec(ctx, &c);
    return r == NUE_ERR_ALREADY_EXISTS ? NUE_OK : r;
}

// ---------------------------------------------------------------------------------------------------- public: context
NUE_ASSETS_API nue_result NUE_CALL nue_assets_context_create(const nue_assets_context_desc* desc, nue_assets_context* out) {
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<Ctx> c(new Ctx());
    uint32_t flags = desc && desc->struct_size >= offsetof(nue_assets_context_desc, flags) + sizeof(uint32_t) ? desc->flags : 0;
    c->flags = flags;
    c->budget_name = desc && desc->budget_name && desc->budget_name[0] ? desc->budget_name : "assets";
    if (!(flags & NUE_ASSETS_CTX_NO_BUDGET)) {
        nue_budget_id id = 0;
        if (nue_budget_find(c->budget_name.c_str(), &id) == NUE_OK && id) {
            c->budget = id; c->use_budget = true;
        } else {
            nue_budget_desc bd;
            NUE_STRUCT_INIT(&bd);
            bd.name = c->budget_name.c_str();
            bd.kind = NUE_BUDGET_IMPORT_STAGING;
            if (desc && desc->struct_size >= sizeof(nue_assets_context_desc)) bd.policy = desc->budget_policy;
            if (nue_budget_register(&bd, &id) == NUE_OK && id) { c->budget = id; c->use_budget = true; }   // fails while the engine is not initialised: no budget then
        }
    }
    nue_assets_context h = reinterpret_cast<nue_assets_context>(static_cast<nue_assets_context_s*>(c.get()));
    if (!(flags & NUE_ASSETS_CTX_NO_BUILTINS)) {
        nue_result r = register_jdlz(h);
        if (r == NUE_OK) r = nue_assets_register_nfs(h);
        if (r < 0) return r;
    }
    *out = h;
    c.release();
    return NUE_OK;
}

NUE_ASSETS_API void NUE_CALL nue_assets_context_destroy(nue_assets_context ctx) { delete ctx_of(ctx); }

NUE_ASSETS_API nue_result NUE_CALL nue_assets_register_codec(nue_assets_context ctx, const nue_asset_codec* codec) {
    Ctx* c = ctx_of(ctx);
    if (!c || !codec || !codec->name || !codec->name[0] || !codec->identify || !codec->decompress) return NUE_ERR_INVALID_ARGUMENT;
    if (c->find_codec(codec->name)) return NUE_ERR_ALREADY_EXISTS;
    std::unique_ptr<CodecRec> r(new CodecRec());
    r->name = codec->name;
    r->codec = *codec;
    r->codec.name = r->name.c_str();
    c->codecs.push_back(std::move(r));
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_assets_register_format(nue_assets_context ctx, const nue_asset_format* format) {
    Ctx* c = ctx_of(ctx);
    if (!c || !format || !format->name || !format->name[0] || !format->open || !format->probe) return NUE_ERR_INVALID_ARGUMENT;
    if (c->find_format(format->name)) return NUE_ERR_ALREADY_EXISTS;
    std::unique_ptr<FormatRec> r(new FormatRec());
    r->name = format->name;
    r->description = format->description ? format->description : "";
    r->extensions = format->extensions ? format->extensions : "";
    r->format = *format;
    r->format.name = r->name.c_str();
    r->format.description = r->description.c_str();
    r->format.extensions = r->extensions.c_str();
    c->formats.push_back(std::move(r));
    return NUE_OK;
}

NUE_ASSETS_API uint32_t NUE_CALL nue_assets_codec_count(nue_assets_context ctx) { Ctx* c = ctx_of(ctx); return c ? uint32_t(c->codecs.size()) : 0; }
NUE_ASSETS_API const char* NUE_CALL nue_assets_codec_name(nue_assets_context ctx, uint32_t i) { Ctx* c = ctx_of(ctx); return c && i < c->codecs.size() ? c->codecs[i]->name.c_str() : nullptr; }
NUE_ASSETS_API uint32_t NUE_CALL nue_assets_format_count(nue_assets_context ctx) { Ctx* c = ctx_of(ctx); return c ? uint32_t(c->formats.size()) : 0; }
NUE_ASSETS_API const char* NUE_CALL nue_assets_format_name(nue_assets_context ctx, uint32_t i) { Ctx* c = ctx_of(ctx); return c && i < c->formats.size() ? c->formats[i]->name.c_str() : nullptr; }

NUE_ASSETS_API nue_result NUE_CALL nue_assets_codec_encode(nue_assets_context ctx, const char* codec, const void* src, size_t size, int32_t level, void** out, size_t* out_size) {
    Ctx* c = ctx_of(ctx);
    if (!c || !out || !out_size) return NUE_ERR_INVALID_ARGUMENT;
    const CodecRec* cr = c->find_codec(codec);
    if (!cr) return NUE_ERR_NOT_FOUND;
    if (!cr->codec.compress || !cr->codec.compress_bound) return NUE_ERR_UNSUPPORTED;
    size_t cap = cr->codec.compress_bound(cr->codec.userdata, size);
    Buffer b;
    nue_result r = b.alloc(nullptr, cap);                    // handed to the caller: not accounted
    if (r < 0) return r;
    nue_asset_compress_params p;
    NUE_STRUCT_INIT(&p);
    p.level = level;
    size_t written = 0;
    r = cr->codec.compress(cr->codec.userdata, src, size, &p, b.data(), cap, &written);
    if (r < 0) return r;
    void* mem = nue_alloc(written ? written : 1, 16);
    if (!mem) return NUE_ERR_OUT_OF_MEMORY;
    if (written) std::memcpy(mem, b.data(), written);
    *out = mem; *out_size = written;
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_assets_codec_decode(nue_assets_context ctx, const char* codec, const void* src, size_t size, void** out, size_t* out_size) {
    Ctx* c = ctx_of(ctx);
    if (!c || !out || !out_size) return NUE_ERR_INVALID_ARGUMENT;
    const CodecRec* cr = c->find_codec(codec);
    if (!cr) return NUE_ERR_NOT_FOUND;
    uint64_t unc = 0, stream = 0;
    nue_result r = cr->codec.identify(cr->codec.userdata, src, size, &unc, &stream);
    if (r < 0) return r;
    if (unc > SIZE_MAX / 2) return NUE_ERR_OVERFLOW;
    void* mem = nue_alloc(unc ? size_t(unc) : 1, 16);
    if (!mem) return NUE_ERR_OUT_OF_MEMORY;
    size_t written = 0;
    r = cr->codec.decompress(cr->codec.userdata, src, size, mem, size_t(unc), &written);
    if (r < 0) { nue_free(mem); return r; }
    *out = mem; *out_size = written;
    return NUE_OK;
}

NUE_ASSETS_API const char* NUE_CALL nue_assets_version_string(void) { return "nue_assets 1.0 (nuepack 1.0)"; }
