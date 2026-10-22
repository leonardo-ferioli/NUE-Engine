// Containers: open (unwrap codec, probe format, build the node tree), query, edit, write.
#include "internal.hpp"

#include <cstdlib>

using namespace nuea;

namespace {

const nue_asset_source* content_of(const nue_asset_container_s* c) { return c->content ? c->content.get() : c->original.get(); }

// ---------------------------------------------------------------------------------------------------- open context handed to plugins
nue_result NUE_CALL ctx_add_node(nue_asset_open_ctx* ctx, const nue_asset_node_info* node, uint32_t* out_index) {
    return container_add_node(cont(ctx->container), node, out_index);
}

nue_result NUE_CALL ctx_truncate_children(nue_asset_open_ctx* ctx, uint32_t node) {
    nue_asset_container_s* c = cont(ctx->container);
    if (node >= c->nodes.size()) return NUE_ERR_INVALID_ARGUMENT;
    c->nodes.resize(size_t(node) + 1);
    c->nodes[node].info.kind = NUE_ASSET_NODE_BLOB;
    c->nodes[node].info.flags |= NUE_ASSET_NODE_OPAQUE;
    c->nodes[node].info.first_child = NUE_ASSET_NO_NODE;
    c->nodes[node].last_child = NUE_ASSET_NO_NODE;
    return NUE_OK;
}

void NUE_CALL ctx_set_state(nue_asset_open_ctx* ctx, void* state) { cont(ctx->container)->state = state; }
void* NUE_CALL ctx_get_state(const nue_asset_open_ctx* ctx) { return cont(ctx->container)->state; }

const nue_asset_open_ctx_ops g_open_ops = {sizeof(nue_asset_open_ctx_ops), ctx_add_node, ctx_truncate_children, ctx_set_state, ctx_get_state};

nue_result NUE_CALL res_add(void* builder, const nue_asset_resource_info* r, uint32_t* out_index) {
    nue_asset_container_s* c = static_cast<nue_asset_container_s*>(builder);
    if (!r) return NUE_ERR_INVALID_ARGUMENT;
    ResRec rec;
    std::memset(&rec.info, 0, sizeof(rec.info));
    std::memcpy(&rec.info, r, std::min<size_t>(r->struct_size, sizeof(rec.info)));
    rec.info.struct_size = sizeof(nue_asset_resource_info);
    rec.info.index = uint32_t(c->res.size());
    rec.name = r->name ? r->name : "";
    c->res.push_back(std::move(rec));
    if (out_index) *out_index = uint32_t(c->res.size() - 1);
    return NUE_OK;
}

const nue_asset_resource_ops g_res_ops = {sizeof(nue_asset_resource_ops), res_add};

// ---------------------------------------------------------------------------------------------------- file helpers
nue_result read_all(Ctx* ctx, const nue_asset_source* src, uint64_t offset, uint64_t size, Buffer& out) {
    if (size > SIZE_MAX / 2) return NUE_ERR_OUT_OF_MEMORY;
    nue_result r = out.alloc(ctx, size_t(size));
    if (r < 0) return r;
    uint64_t done = 0;
    while (done < size) {
        size_t chunk = size_t(std::min<uint64_t>(size - done, 1u << 24)), got = 0;
        r = src->read(offset + done, out.data() + done, chunk, &got);
        if (r < 0) return r;
        if (got == 0) { nue_set_last_error("assets: unexpected end of file"); return NUE_ERR_CORRUPT; }
        done += got;
    }
    return NUE_OK;
}

}  // namespace

namespace nuea {

nue_result container_add_node(nue_asset_container_s* c, const nue_asset_node_info* node, uint32_t* out_index) {
    if (!node || node->struct_size < offsetof(nue_asset_node_info, name)) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t idx = uint32_t(c->nodes.size());
    if (node->parent != NUE_ASSET_NO_NODE && node->parent >= idx) return NUE_ERR_INVALID_ARGUMENT;
    NodeRec rec;
    std::memset(&rec.info, 0, sizeof(rec.info));
    std::memcpy(&rec.info, node, std::min<size_t>(node->struct_size, sizeof(rec.info)));
    rec.info.struct_size = sizeof(nue_asset_node_info);
    rec.info.index = idx;
    rec.info.first_child = rec.info.next_sibling = NUE_ASSET_NO_NODE;
    rec.info.depth = node->parent == NUE_ASSET_NO_NODE ? 0 : c->nodes[node->parent].info.depth + 1;
    if (node->name) { rec.name = node->name; rec.info.flags |= NUE_ASSET_NODE_HAS_NAME; }
    rec.info.name = nullptr;
    if (node->parent != NUE_ASSET_NO_NODE) {
        NodeRec& p = c->nodes[node->parent];
        if (p.last_child == NUE_ASSET_NO_NODE) p.info.first_child = idx;
        else c->nodes[p.last_child].info.next_sibling = idx;
        p.last_child = idx;
    } else {
        // roots are siblings of each other
        for (uint32_t i = idx; i-- > 0;) if (c->nodes[i].info.parent == NUE_ASSET_NO_NODE) { c->nodes[i].info.next_sibling = idx; break; }
    }
    c->nodes.push_back(std::move(rec));
    if (out_index) *out_index = idx;
    return NUE_OK;
}

nue_result container_attach_format(nue_asset_container_s* c, const FormatRec* fmt) {
    c->fmt = fmt;
    c->format_name = fmt->name;
    return NUE_OK;
}

nue_result container_build_index(nue_asset_container_s* c) {
    c->res.clear();
    if (!c->fmt || !c->fmt->format.index) return NUE_OK;
    return c->fmt->format.index(c->fmt->format.userdata, wrap(c), c->state, &g_res_ops, c);
}

void container_free_state(nue_asset_container_s* c) {
    if (c->fmt && c->fmt->format.close) c->fmt->format.close(c->fmt->format.userdata, c->state);
    c->state = nullptr;
}

}  // namespace nuea

// ---------------------------------------------------------------------------------------------------- open / close
NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_open(nue_assets_context ctx, const nue_asset_open_desc* desc, nue_asset_container* out) {
    Ctx* x = ctx_of(ctx);
    if (!x || !desc || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<nue_asset_container_s> c(new nue_asset_container_s());
    c->ctx = x;
    c->open_flags = desc->flags;
    std::string ext;
    nue_result r = NUE_OK;

    if (desc->path && desc->path[0]) {
        std::unique_ptr<FileSource> fs;
        r = FileSource::open(desc->path, &fs);
        if (r < 0) { nue_set_last_error("assets: cannot open '%s'", desc->path); return r; }
        c->original = std::move(fs);
        c->path = desc->path;
        ext = lower_ext(desc->path);
    } else if (desc->memory || desc->memory_size == 0) {
        if (desc->flags & NUE_ASSET_OPEN_COPY) {
            Buffer b;
            r = b.alloc(x, desc->memory_size);
            if (r < 0) return r;
            if (desc->memory_size) std::memcpy(b.data(), desc->memory, desc->memory_size);
            c->original.reset(new MemSource(std::move(b)));
        } else {
            c->original.reset(new MemSource(desc->memory, desc->memory_size));
        }
        ext = lower_ext(desc->name_hint);
    } else {
        return NUE_ERR_INVALID_ARGUMENT;
    }
    c->file_size = c->original->size();

    // 1. a codec wrapper around the whole file?
    if (!(desc->flags & NUE_ASSET_OPEN_NO_UNWRAP) && c->file_size > 0) {
        uint8_t head[64];
        size_t got = 0;
        c->original->read(0, head, sizeof(head), &got);
        for (auto& cr : x->codecs) {
            uint64_t unc = 0, stream = 0;
            if (cr->codec.identify(cr->codec.userdata, head, got, &unc, &stream) != NUE_OK) continue;
            if (stream == 0 || stream > c->file_size) { nue_set_last_error("assets: %s stream larger than the file", cr->name.c_str()); return NUE_ERR_CORRUPT; }
            Buffer packed, plain;
            const void* in = c->original->peek(0, size_t(stream));
            if (!in) {
                r = read_all(x, c->original.get(), 0, stream, packed);
                if (r < 0) return r;
                in = packed.data();
            }
            if (unc > SIZE_MAX / 2) return NUE_ERR_OUT_OF_MEMORY;
            r = plain.alloc(x, size_t(unc));
            if (r < 0) return r;
            size_t written = 0;
            r = cr->codec.decompress(cr->codec.userdata, in, size_t(stream), plain.data(), plain.size(), &written);
            if (r < 0) { nue_set_last_error("assets: %s stream is damaged", cr->name.c_str()); return r; }
            if (written != plain.size()) return NUE_ERR_CORRUPT;
            packed.release();
            c->codec = cr.get();
            c->content.reset(new MemSource(std::move(plain)));
            break;
        }
    }

    // 2. which format?
    const nue_asset_source* src = content_of(c.get());
    const FormatRec* fmt = nullptr;
    if (desc->flags & NUE_ASSET_OPEN_FORMAT) {
        fmt = x->find_format(desc->format_name);
        if (!fmt) { nue_set_last_error("assets: unknown format '%s'", desc->format_name ? desc->format_name : ""); return NUE_ERR_NOT_FOUND; }
    } else {
        uint8_t head[4096];
        size_t got = 0;
        src->read(0, head, sizeof(head), &got);
        fmt = x->probe_format(head, got, src->size(), ext.c_str());
        if (!fmt) { nue_set_last_error("assets: no registered format recognises this file"); return NUE_ERR_UNSUPPORTED; }
    }
    container_attach_format(c.get(), fmt);

    // 3. the plugin builds the node tree
    nue_asset_open_ctx oc;
    NUE_STRUCT_INIT(&oc);
    oc.ops = &g_open_ops;
    oc.container = wrap(c.get());
    oc.source = src;
    oc.flags = desc->flags;
    r = fmt->format.open(fmt->format.userdata, &oc);
    if (r < 0) { container_free_state(c.get()); return r; }
    r = container_build_index(c.get());
    if (r < 0) { container_free_state(c.get()); return r; }
    // plugins report repairs by setting the OPAQUE flag on nodes or adding a TAIL node
    for (auto& n : c->nodes) if ((n.info.flags & NUE_ASSET_NODE_OPAQUE) || n.info.kind == NUE_ASSET_NODE_TAIL) c->lenient_repairs = true;
    *out = wrap(c.release());
    return NUE_OK;
}

NUE_ASSETS_API void NUE_CALL nue_asset_container_close(nue_asset_container container) {
    nue_asset_container_s* c = cont(container);
    if (!c) return;
    container_free_state(c);
    for (auto& r : c->res) if (r.edit) nue_tex_image_destroy(r.edit);
    delete c;
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_get_info(nue_asset_container container, nue_asset_container_info* out) {
    nue_asset_container_s* c = cont(container);
    if (!c || !out) return NUE_ERR_INVALID_ARGUMENT;
    nue_asset_container_info i;
    NUE_STRUCT_INIT(&i);
    i.format = c->format_name.c_str();
    i.codec = c->codec ? c->codec->name.c_str() : nullptr;
    i.node_count = uint32_t(c->nodes.size());
    i.resource_count = uint32_t(c->res.size());
    i.file_size = c->file_size;
    i.content_size = content_of(c)->size();
    i.dirty = c->dirty;
    i.lenient_repairs = c->lenient_repairs;
    std::memcpy(out, &i, std::min<size_t>(out->struct_size ? out->struct_size : sizeof(i), sizeof(i)));
    return NUE_OK;
}

// ---------------------------------------------------------------------------------------------------- nodes
NUE_ASSETS_API uint32_t NUE_CALL nue_asset_node_count(nue_asset_container container) { return container ? uint32_t(cont(container)->nodes.size()) : 0; }

NUE_ASSETS_API nue_result NUE_CALL nue_asset_node_get(nue_asset_container container, uint32_t index, nue_asset_node_info* out) {
    nue_asset_container_s* c = cont(container);
    if (!c || !out) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= c->nodes.size()) return NUE_ERR_NOT_FOUND;
    NodeRec& n = c->nodes[index];
    nue_asset_node_info i = n.info;
    i.name = (n.info.flags & NUE_ASSET_NODE_HAS_NAME) ? n.name.c_str() : nullptr;
    if (n.edited) i.flags |= NUE_ASSET_NODE_EDITED;
    std::memcpy(out, &i, std::min<size_t>(out->struct_size ? out->struct_size : sizeof(i), sizeof(i)));
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_node_read(nue_asset_container container, uint32_t index, uint64_t offset, void* buffer, size_t size, size_t* out_read) {
    nue_asset_container_s* c = cont(container);
    if (out_read) *out_read = 0;
    if (!c || (!buffer && size)) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= c->nodes.size()) return NUE_ERR_NOT_FOUND;
    const NodeRec& n = c->nodes[index];
    if (offset >= n.info.size) return NUE_OK_PARTIAL;
    size_t want = size_t(std::min<uint64_t>(size, n.info.size - offset));
    nue_result r = NUE_OK;
    size_t got = want;
    if (n.edited) {
        std::memcpy(buffer, n.edit.data() + offset, want);
    } else {
        r = content_of(c)->read(n.info.offset + offset, buffer, want, &got);
        if (r < 0) return r;
    }
    if (out_read) *out_read = got;
    return got == size ? NUE_OK : NUE_OK_PARTIAL;
}

NUE_ASSETS_API const void* NUE_CALL nue_asset_node_peek(nue_asset_container container, uint32_t index, uint64_t offset, size_t size) {
    nue_asset_container_s* c = cont(container);
    if (!c || index >= c->nodes.size()) return nullptr;
    const NodeRec& n = c->nodes[index];
    if (offset > n.info.size || size > n.info.size - offset) return nullptr;
    if (n.edited) return n.edit.data() + offset;
    return content_of(c)->peek(n.info.offset + offset, size);
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_node_set_payload(nue_asset_container container, uint32_t index, const void* data, size_t size) {
    nue_asset_container_s* c = cont(container);
    if (!c || (!data && size)) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= c->nodes.size()) return NUE_ERR_NOT_FOUND;
    NodeRec& n = c->nodes[index];
    if (n.info.kind == NUE_ASSET_NODE_GROUP || n.info.kind == NUE_ASSET_NODE_DERIVED) return NUE_ERR_UNSUPPORTED;
    Buffer b;
    nue_result r = b.alloc(c->ctx, size);
    if (r < 0) return r;
    if (size) std::memcpy(b.data(), data, size);
    n.edit = std::move(b);
    n.edited = true;
    n.info.size = size;
    c->dirty = true;
    return NUE_OK;
}

NUE_ASSETS_API uint32_t NUE_CALL nue_asset_node_find(nue_asset_container container, uint64_t id, uint32_t start) {
    nue_asset_container_s* c = cont(container);
    if (!c) return NUE_ASSET_NO_NODE;
    for (size_t i = start; i < c->nodes.size(); ++i) if (c->nodes[i].info.id == id) return uint32_t(i);
    return NUE_ASSET_NO_NODE;
}

// ---------------------------------------------------------------------------------------------------- resources
NUE_ASSETS_API uint32_t NUE_CALL nue_asset_resource_count(nue_asset_container container) { return container ? uint32_t(cont(container)->res.size()) : 0; }

NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_get(nue_asset_container container, uint32_t index, nue_asset_resource_info* out) {
    nue_asset_container_s* c = cont(container);
    if (!c || !out) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= c->res.size()) return NUE_ERR_NOT_FOUND;
    ResRec& r = c->res[index];
    nue_asset_resource_info i = r.info;
    i.name = r.name.c_str();
    if (r.edit) {
        i.flags |= NUE_ASSET_RESFLAG_EDITED;
        nue_tex_image_desc d;
        NUE_STRUCT_INIT(&d);
        if (nue_tex_image_get_desc(r.edit, &d) == NUE_OK) { i.format = d.format; i.width = d.width; i.height = d.height; i.depth = d.depth ? d.depth : 1; i.mip_levels = d.mip_levels; i.data_size = nue_tex_image_total_size(r.edit); }
    }
    std::memcpy(out, &i, std::min<size_t>(out->struct_size ? out->struct_size : sizeof(i), sizeof(i)));
    return NUE_OK;
}

NUE_ASSETS_API uint32_t NUE_CALL nue_asset_resource_find(nue_asset_container container, const char* name, uint64_t hash) {
    nue_asset_container_s* c = cont(container);
    if (!c) return NUE_ASSET_NO_NODE;
    for (auto& r : c->res) {
        if (name && name[0]) { if (iequals(r.name.c_str(), name)) return r.info.index; }
        else if (hash && r.info.hash == hash) return r.info.index;
    }
    return NUE_ASSET_NO_NODE;
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_get_image(nue_asset_container container, uint32_t index, nue_tex_image* out_image) {
    nue_asset_container_s* c = cont(container);
    if (!c || !out_image) return NUE_ERR_INVALID_ARGUMENT;
    *out_image = nullptr;
    if (index >= c->res.size()) return NUE_ERR_NOT_FOUND;
    ResRec& r = c->res[index];
    if (r.edit) return nue_tex_image_clone(r.edit, out_image);
    if (r.info.kind != NUE_ASSET_RES_TEXTURE || !c->fmt->format.get_image) return NUE_ERR_UNSUPPORTED;
    return c->fmt->format.get_image(c->fmt->format.userdata, container, c->state, index, out_image);
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_set_image(nue_asset_container container, uint32_t index, nue_tex_image image) {
    nue_asset_container_s* c = cont(container);
    if (!c || !image) return NUE_ERR_INVALID_ARGUMENT;
    if (index >= c->res.size()) return NUE_ERR_NOT_FOUND;
    ResRec& r = c->res[index];
    if (r.info.kind != NUE_ASSET_RES_TEXTURE) return NUE_ERR_UNSUPPORTED;
    nue_tex_image copy = nullptr;
    nue_result res = nue_tex_image_clone(image, &copy);
    if (res < 0) return res;
    if (r.edit) nue_tex_image_destroy(r.edit);
    r.edit = copy;
    c->dirty = true;
    return NUE_OK;
}

NUE_ASSETS_API nue_tex_image NUE_CALL nue_asset_resource_edit(nue_asset_container container, uint32_t index) {
    nue_asset_container_s* c = cont(container);
    return c && index < c->res.size() ? c->res[index].edit : nullptr;
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_resource_save_texture(nue_asset_container container, uint32_t index, const char* path, nue_tex_file_kind kind) {
    nue_tex_image im = nullptr;
    nue_result r = nue_asset_resource_get_image(container, index, &im);
    if (r < 0) return r;
    r = nue_tex_image_save_file(im, path, kind, 0);
    nue_tex_image_destroy(im);
    return r;
}

// ---------------------------------------------------------------------------------------------------- writing
namespace {

nue_result copy_source(const nue_asset_source* src, nue_asset_sink* sink) {
    const uint64_t total = src->size();
    if (const void* p = src->peek(0, size_t(total))) return sink->write(p, size_t(total));
    std::vector<uint8_t> tmp(1u << 20);
    for (uint64_t off = 0; off < total;) {
        size_t got = 0;
        nue_result r = src->read(off, tmp.data(), size_t(std::min<uint64_t>(tmp.size(), total - off)), &got);
        if (r < 0) return r;
        if (!got) return NUE_ERR_IO;
        r = sink->write(tmp.data(), got);
        if (r < 0) return r;
        off += got;
    }
    return NUE_OK;
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_write(nue_asset_container container, const nue_asset_write_desc* desc) {
    nue_asset_container_s* c = cont(container);
    if (!c || !desc) return NUE_ERR_INVALID_ARGUMENT;
    if (!desc->path && !desc->out_data) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t flags = desc->flags;
    const bool rebuild = (flags & NUE_ASSET_WRITE_REBUILD) || c->dirty;
    const CodecRec* codec = nullptr;
    if (flags & NUE_ASSET_WRITE_FORCE_CODEC) {
        const char* name = desc->codec;
        if (!name && c->fmt->format.default_codec) name = c->fmt->format.default_codec(c->fmt->format.userdata, lower_ext(desc->path).c_str());
        codec = c->ctx->find_codec(name);
        if (!codec) { nue_set_last_error("assets: codec '%s' is not registered", name ? name : "(none)"); return NUE_ERR_NOT_FOUND; }
    } else if (c->codec && !(flags & NUE_ASSET_WRITE_NO_CODEC)) {
        codec = c->codec;
    }

    MemSink out(c->ctx);
    nue_result r = NUE_OK;
    if (!rebuild && codec && codec == c->codec && !(flags & NUE_ASSET_WRITE_RECOMPRESS)) {
        r = copy_source(c->original.get(), &out);                                   // unchanged: the file as it was, byte for byte
    } else {
        MemSink plain(c->ctx);
        if (rebuild) {
            if (!c->fmt->format.write) return NUE_ERR_UNSUPPORTED;
            r = c->fmt->format.write(c->fmt->format.userdata, container, c->state, flags, &plain);
        } else {
            r = copy_source(content_of(c), &plain);
        }
        if (r < 0) return r;
        if (!codec) {
            out.buffer() = std::move(plain.buffer());
        } else {
            if (!codec->codec.compress || !codec->codec.compress_bound) { nue_set_last_error("assets: codec '%s' cannot compress", codec->name.c_str()); return NUE_ERR_UNSUPPORTED; }
            const size_t n = plain.buffer().size();
            const size_t cap = codec->codec.compress_bound(codec->codec.userdata, n);
            r = out.buffer().alloc(c->ctx, cap);
            if (r < 0) return r;
            nue_asset_compress_params p;
            NUE_STRUCT_INIT(&p);
            p.level = desc->codec_level ? desc->codec_level : 6;
            size_t written = 0;
            r = codec->codec.compress(codec->codec.userdata, plain.buffer().data(), n, &p, out.buffer().data(), cap, &written);
            if (r < 0) return r;
            out.buffer().set_size(written);
        }
    }
    if (r < 0) return r;
    if (desc->path) {
        r = nue_file_save(desc->path, out.buffer().data(), out.buffer().size());
        if (r < 0) nue_set_last_error("assets: cannot write '%s'", desc->path);
        return r;
    }
    size_t n = 0;
    uint8_t* mem = out.buffer().detach(&n);
    if (!mem) { mem = static_cast<uint8_t*>(nue_alloc(1, 16)); n = 0; }
    *desc->out_data = mem;
    if (desc->out_size) *desc->out_size = n;
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_asset_container_content_hash(nue_asset_container container, uint8_t out_sha256[32]) {
    nue_asset_container_s* c = cont(container);
    if (!c || !out_sha256) return NUE_ERR_INVALID_ARGUMENT;
    MemSink sink(c->ctx);
    nue_result r;
    if (c->dirty) r = c->fmt->format.write ? c->fmt->format.write(c->fmt->format.userdata, container, c->state, 0, &sink) : NUE_ERR_UNSUPPORTED;
    else r = copy_source(content_of(c), &sink);
    if (r < 0) return r;
    nue_sha256(sink.buffer().data(), sink.buffer().size(), out_sha256);
    return NUE_OK;
}
