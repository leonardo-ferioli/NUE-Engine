// A package or a container as a read-only named storage source, with positioned and asynchronous reads (engine VFS style).
#include "internal.hpp"

using namespace nuea;

struct nue_pack_mount_s {
    Ctx* ctx = nullptr;
    nue_pack pack = nullptr;                                // owned when created from a path
    bool owns_pack = false;
    nue_asset_container container = nullptr;                // not owned
    nue_pack_variant variant = NUE_PACK_VARIANT_SOURCE;
    ~nue_pack_mount_s() { if (owns_pack) nue_pack_close(pack); }
};

struct nue_pack_file_s {
    nue_file file = 0;                                      // disk backed: positioned reads at base + offset
    uint64_t base = 0;
    const uint8_t* mem = nullptr;                           // memory backed
    void* owned = nullptr;                                  // nue_alloc'd memory behind `mem`
    uint64_t size = 0;
    ~nue_pack_file_s() { if (owned) nue_free(owned); }
};

namespace {

nue_pack_mount_s* mt(nue_pack_mount m) { return reinterpret_cast<nue_pack_mount_s*>(m); }
nue_pack_file_s* pf(nue_pack_file f) { return reinterpret_cast<nue_pack_file_s*>(f); }

bool parse_node_name(const char* name, uint32_t* index) {
    if (std::strncmp(name, "node/", 5) != 0 || !name[5]) return false;
    uint64_t v = 0;
    for (const char* p = name + 5; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        v = v * 10 + uint64_t(*p - '0');
        if (v > 0xFFFFFFFEull) return false;
    }
    *index = uint32_t(v);
    return true;
}

// "name.dds" -> resource index
uint32_t find_dds_resource(nue_asset_container c, const char* name) {
    std::string n = name;
    if (n.size() < 5 || !iequals(n.c_str() + n.size() - 4, ".dds")) return NUE_ASSET_NO_NODE;
    n.resize(n.size() - 4);
    return nue_asset_resource_find(c, n.c_str(), 0);
}

}  // namespace

NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_create(nue_assets_context ctx, const char* path, nue_pack_variant preferred, nue_pack_mount* out) {
    if (!ctx || !path || !out) return NUE_ERR_INVALID_ARGUMENT;
    std::unique_ptr<nue_pack_mount_s> m(new nue_pack_mount_s());
    m->ctx = ctx_of(ctx);
    m->variant = preferred;
    nue_result r = nue_pack_open_file(ctx, path, 0, &m->pack);
    if (r < 0) return r;
    m->owns_pack = true;
    *out = reinterpret_cast<nue_pack_mount>(m.release());
    return NUE_OK;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_create_for_caps(nue_assets_context ctx, const char* path, const nue_tex_caps* caps, nue_pack_mount* out) {
    return nue_pack_mount_create(ctx, path, nue_pack_variant_for_caps(caps), out);
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_create_container(nue_assets_context ctx, nue_asset_container container, nue_pack_mount* out) {
    if (!ctx || !container || !out) return NUE_ERR_INVALID_ARGUMENT;
    std::unique_ptr<nue_pack_mount_s> m(new nue_pack_mount_s());
    m->ctx = ctx_of(ctx);
    m->container = container;
    *out = reinterpret_cast<nue_pack_mount>(m.release());
    return NUE_OK;
}

NUE_ASSETS_API void NUE_CALL nue_pack_mount_destroy(nue_pack_mount m) { delete mt(m); }

NUE_ASSETS_API nue_bool NUE_CALL nue_pack_mount_exists(nue_pack_mount mount, const char* name) {
    uint64_t size;
    return nue_pack_mount_size(mount, name, &size) >= 0 ? NUE_TRUE : NUE_FALSE;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_mount_size(nue_pack_mount mount, const char* name, uint64_t* out_size) {
    nue_pack_mount_s* m = mt(mount);
    if (!m || !name || !out_size) return NUE_ERR_INVALID_ARGUMENT;
    if (m->pack) {
        const uint32_t e = nue_pack_entry_find(m->pack, name, m->variant);
        if (e == NUE_ASSET_NO_NODE) return NUE_ERR_NOT_FOUND;
        nue_pack_entry_info i;
        NUE_STRUCT_INIT(&i);
        nue_pack_entry_get(m->pack, e, &i);
        *out_size = i.raw_size;
        return NUE_OK;
    }
    uint32_t node;
    if (parse_node_name(name, &node)) {
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        nue_result r = nue_asset_node_get(m->container, node, &n);
        if (r < 0) return r;
        *out_size = n.size;
        return NUE_OK;
    }
    if (find_dds_resource(m->container, name) != NUE_ASSET_NO_NODE) {
        nue_pack_file f = nullptr;
        nue_result r = nue_pack_file_open(mount, name, &f);
        if (r < 0) return r;
        *out_size = nue_pack_file_size(f);
        nue_pack_file_close(f);
        return NUE_OK;
    }
    return NUE_ERR_NOT_FOUND;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_file_open(nue_pack_mount mount, const char* name, nue_pack_file* out) {
    nue_pack_mount_s* m = mt(mount);
    if (!m || !name || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    std::unique_ptr<nue_pack_file_s> f(new nue_pack_file_s());
    if (m->pack) {
        const uint32_t e = nue_pack_entry_find(m->pack, name, m->variant);
        if (e == NUE_ASSET_NO_NODE) return NUE_ERR_NOT_FOUND;
        nue_pack_entry_info i;
        NUE_STRUCT_INIT(&i);
        nue_pack_entry_get(m->pack, e, &i);
        f->size = i.raw_size;
        nue_file backing = pack_backing_file(m->pack);
        if (i.codec == 0 && backing && !pack_entry_peek(m->pack, e)) {
            f->file = backing;
            f->base = i.offset;
        } else {
            const void* data = nullptr;
            size_t size = 0;
            nue_bool owned = NUE_FALSE;
            nue_result r = nue_pack_entry_data(m->pack, e, &data, &size, &owned);
            if (r < 0) return r;
            f->mem = static_cast<const uint8_t*>(data);
            if (owned) f->owned = const_cast<void*>(data);
        }
    } else {
        uint32_t node;
        if (parse_node_name(name, &node)) {
            nue_asset_node_info n;
            NUE_STRUCT_INIT(&n);
            nue_result r = nue_asset_node_get(m->container, node, &n);
            if (r < 0) return r;
            f->size = n.size;
            nue_file file = 0;
            uint64_t off = 0;
            if (container_node_file_range(m->container, node, &file, &off)) {
                f->file = file;
                f->base = off;
            } else if (const void* p = nue_asset_node_peek(m->container, node, 0, size_t(n.size))) {
                f->mem = static_cast<const uint8_t*>(p);
            } else {
                void* mem = nue_alloc(n.size ? size_t(n.size) : 1, 16);
                if (!mem) return NUE_ERR_OUT_OF_MEMORY;
                size_t got = 0;
                r = nue_asset_node_read(m->container, node, 0, mem, size_t(n.size), &got);
                if (r < 0) { nue_free(mem); return r; }
                f->mem = static_cast<const uint8_t*>(mem);
                f->owned = mem;
            }
        } else {
            const uint32_t res = find_dds_resource(m->container, name);
            if (res == NUE_ASSET_NO_NODE) return NUE_ERR_NOT_FOUND;
            nue_tex_image im = nullptr;
            nue_result r = nue_asset_resource_get_image(m->container, res, &im);
            if (r < 0) return r;
            void* data = nullptr;
            size_t size = 0;
            r = nue_tex_image_save_memory(im, NUE_TEX_FILE_DDS, 0, &data, &size);
            nue_tex_image_destroy(im);
            if (r < 0) return r;
            void* mem = nue_alloc(size ? size : 1, 16);
            if (!mem) { nue_tex_free(data); return NUE_ERR_OUT_OF_MEMORY; }
            std::memcpy(mem, data, size);
            nue_tex_free(data);
            f->mem = static_cast<const uint8_t*>(mem);
            f->owned = mem;
            f->size = size;
        }
    }
    *out = reinterpret_cast<nue_pack_file>(f.release());
    return NUE_OK;
}

NUE_ASSETS_API void NUE_CALL nue_pack_file_close(nue_pack_file file) { delete pf(file); }
NUE_ASSETS_API uint64_t NUE_CALL nue_pack_file_size(nue_pack_file file) { return file ? pf(file)->size : 0; }

NUE_ASSETS_API nue_result NUE_CALL nue_pack_file_read_at(nue_pack_file file, uint64_t offset, void* buffer, size_t size, size_t* out_read) {
    nue_pack_file_s* f = pf(file);
    if (out_read) *out_read = 0;
    if (!f || (!buffer && size)) return NUE_ERR_INVALID_ARGUMENT;
    if (offset >= f->size) return NUE_OK_PARTIAL;
    const size_t want = size_t(std::min<uint64_t>(size, f->size - offset));
    size_t got = want;
    if (f->file) {
        nue_result r = nue_file_read_at(f->file, f->base + offset, buffer, want, &got);
        if (r < 0 && r != NUE_ERR_END_OF_DATA) return r;
    } else if (want) {
        std::memcpy(buffer, f->mem + offset, want);
    }
    if (out_read) *out_read = got;
    return got == size ? NUE_OK : NUE_OK_PARTIAL;
}

NUE_ASSETS_API nue_result NUE_CALL nue_pack_file_read_async(nue_pack_file file, uint64_t offset, void* buffer, size_t size, nue_io_callback callback, void* userdata, nue_io_request* out_request) {
    nue_pack_file_s* f = pf(file);
    if (out_request) *out_request = 0;
    if (!f || (!buffer && size)) return NUE_ERR_INVALID_ARGUMENT;
    if (f->file) {
        const size_t want = offset >= f->size ? 0 : size_t(std::min<uint64_t>(size, f->size - offset));
        nue_io_desc d;
        NUE_STRUCT_INIT(&d);
        d.file = f->file;
        d.offset = f->base + offset;
        d.buffer = buffer;
        d.size = want;
        d.callback = callback;
        d.userdata = userdata;
        return nue_io_read_async(&d, out_request);
    }
    size_t got = 0;
    nue_result r = nue_pack_file_read_at(file, offset, buffer, size, &got);
    if (callback) callback(userdata, 0, r < 0 ? r : NUE_OK, got);
    return r < 0 ? r : NUE_OK;
}
