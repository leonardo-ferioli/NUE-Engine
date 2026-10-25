// Internal types of the asset module. Nothing here is public API.
#pragma once
#include "nue/assets.h"
#include "nue/core.h"

#include "bytes.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------------- sources
struct nue_asset_source {
    virtual ~nue_asset_source() {}
    virtual nue_result read(uint64_t offset, void* buffer, size_t size, size_t* out_read) const = 0;
    virtual uint64_t size() const = 0;
    virtual const void* peek(uint64_t offset, size_t size) const { (void)offset; (void)size; return nullptr; }
    virtual nue_file backing_file() const { return 0; }          // the engine file behind the source, for asynchronous reads
};

struct nue_asset_sink {
    virtual ~nue_asset_sink() {}
    virtual nue_result write(const void* data, size_t size) = 0;
    virtual uint64_t position() const = 0;
    virtual nue_result patch(uint64_t offset, const void* data, size_t size) = 0;
};

namespace nuea {

struct Ctx;

// Memory owned through the engine allocator and accounted in the context's budget.
class Buffer {
public:
    Buffer() {}
    Buffer(Buffer&& o) noexcept : ctx_(o.ctx_), p_(o.p_), n_(o.n_), cap_(o.cap_) { o.p_ = nullptr; o.n_ = o.cap_ = 0; }
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) { release(); ctx_ = o.ctx_; p_ = o.p_; n_ = o.n_; cap_ = o.cap_; o.p_ = nullptr; o.n_ = o.cap_ = 0; }
        return *this;
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() { release(); }

    nue_result alloc(Ctx* ctx, size_t size);               // size bytes, zero initialised
    nue_result reserve(size_t capacity);                    // keeps the content; capacity only grows
    nue_result append(const void* data, size_t size);       // grows geometrically
    nue_result resize(size_t size);                         // new bytes are zero
    void release();
    uint8_t* detach(size_t* out_size);                      // hand the memory to the caller (nue_free)
    uint8_t* data() { return p_; }
    const uint8_t* data() const { return p_; }
    size_t size() const { return n_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return n_ == 0; }
    void set_size(size_t n) { n_ = n; }                     // within capacity (after a direct fill)

private:
    Ctx* ctx_ = nullptr;
    uint8_t* p_ = nullptr;
    size_t n_ = 0, cap_ = 0;
};

class MemSource : public nue_asset_source {
public:
    MemSource(const void* data, size_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}
    MemSource(Buffer&& owned) : owned_(std::move(owned)) { data_ = owned_.data(); size_ = owned_.size(); }
    nue_result read(uint64_t offset, void* buffer, size_t size, size_t* out_read) const override;
    uint64_t size() const override { return size_; }
    const void* peek(uint64_t offset, size_t size) const override { return (offset <= size_ && size <= size_ - offset) ? data_ + offset : nullptr; }
private:
    Buffer owned_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

class FileSource : public nue_asset_source {
public:
    static nue_result open(const char* path, std::unique_ptr<FileSource>* out);
    ~FileSource() override { if (file_) nue_file_close(file_); }
    nue_result read(uint64_t offset, void* buffer, size_t size, size_t* out_read) const override;
    uint64_t size() const override { return size_; }
    nue_file handle() const { return file_; }
    nue_file backing_file() const override { return file_; }
private:
    nue_file file_ = 0;
    uint64_t size_ = 0;
};

// Growable memory output.
class MemSink : public nue_asset_sink {
public:
    explicit MemSink(Ctx* ctx) { buf_.alloc(ctx, 0); }                // binds the buffer to the context's budget
    nue_result write(const void* data, size_t size) override;
    uint64_t position() const override { return buf_.size(); }
    nue_result patch(uint64_t offset, const void* data, size_t size) override;
    Buffer& buffer() { return buf_; }
private:
    Buffer buf_;
};

// ---------------------------------------------------------------------------------------------------- registry
struct CodecRec { std::string name; nue_asset_codec codec; };
struct FormatRec {
    std::string name, description, extensions;
    nue_asset_format format;
};

}  // namespace nuea

struct nue_assets_context_s {
    std::vector<std::unique_ptr<nuea::CodecRec>> codecs;
    std::vector<std::unique_ptr<nuea::FormatRec>> formats;
    nue_budget_id budget = 0;
    bool use_budget = false;
    std::string budget_name;
    uint32_t flags = 0;
};

namespace nuea {
struct Ctx : nue_assets_context_s {
    nue_result charge(uint64_t bytes);                      // request budget; NUE_ERR_OUT_OF_MEMORY when it does not fit
    void credit(uint64_t bytes);
    const CodecRec* find_codec(const char* name) const;
    const FormatRec* find_format(const char* name) const;
    const FormatRec* probe_format(const uint8_t* head, size_t head_size, uint64_t total, const char* ext_hint) const;
};
inline Ctx* ctx_of(nue_assets_context c) { return static_cast<Ctx*>(reinterpret_cast<nue_assets_context_s*>(c)); }

// ---------------------------------------------------------------------------------------------------- container
struct NodeRec {
    uint32_t last_child = NUE_ASSET_NO_NODE;
    nue_asset_node_info info;                               // name pointer is fixed up on query
    std::string name;
    bool edited = false;
    Buffer edit;                                            // replacement payload
};

struct ResRec {
    nue_asset_resource_info info;
    std::string name;
    nue_tex_image edit = nullptr;
    bool has_native = false;                                // pending edit in the game's own encoding
    nue_asset_native_texture native;
    std::vector<uint8_t> native_data;
};

// Little helper for hashing names the same way everywhere (FNV-1a 64 over lower case ASCII).
uint64_t name_hash64(const char* s);
bool iequals(const char* a, const char* b);
std::string lower_ext(const char* path);                    // ".bun" of "a/b/C.BUN"

}  // namespace nuea

struct nue_asset_container_s {
    nuea::Ctx* ctx = nullptr;
    const nuea::FormatRec* fmt = nullptr;
    void* state = nullptr;
    std::string format_name;
    std::unique_ptr<nue_asset_source> original;             // the file as given
    const nuea::CodecRec* codec = nullptr;                  // wrapper codec of the original, if any
    uint32_t codec_level = 0;
    std::unique_ptr<nue_asset_source> content;              // decoded content (the original itself without a codec)
    std::vector<nuea::NodeRec> nodes;
    std::vector<nuea::ResRec> res;
    bool dirty = false;
    bool lenient_repairs = false;
    uint32_t open_flags = 0;
    uint64_t file_size = 0;
    std::string path;
};

namespace nuea {

inline nue_asset_container_s* cont(nue_asset_container c) { return reinterpret_cast<nue_asset_container_s*>(c); }
inline nue_asset_container wrap(nue_asset_container_s* c) { return reinterpret_cast<nue_asset_container>(c); }

// Create an empty container bound to a format (used by open and by pack export); the node list is filled through the plugin's builder.
nue_result container_attach_format(nue_asset_container_s* c, const FormatRec* fmt);
nue_result container_add_node(nue_asset_container_s* c, const nue_asset_node_info* node, uint32_t* out_index);
nue_result container_build_index(nue_asset_container_s* c);
void container_free_state(nue_asset_container_s* c);

nue_result native_to_image(const nue_asset_native_texture& n, nue_tex_image* out);

// the file behind a pack opened from a path (for asynchronous reads); 0 when the pack lives in memory
nue_file pack_backing_file(nue_pack pack);
const void* pack_entry_peek(nue_pack pack, uint32_t index);
// where a node's payload lives on disk, when it is a plain slice of a file (no codec, no edit): the file and the offset
bool container_node_file_range(nue_asset_container c, uint32_t node, nue_file* file, uint64_t* offset);   // pointer to the stored bytes of an uncompressed entry when the pack is in memory / mapped

}  // namespace nuea
