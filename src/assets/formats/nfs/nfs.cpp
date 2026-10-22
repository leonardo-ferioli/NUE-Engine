// NFS plugin: the chunk streams of Need for Speed Underground 2 (.BUN, and .LZC = a JDLZ wrapped .BUN, unwrapped by the framework).
//
// A file is a sequence of chunks: u32 id, u32 payload size, payload. Ids with bit 31 set hold a nested chunk sequence. Chunk id 0 is padding that keeps the
// following chunk aligned. Layouts, as observed on the game files, are in docs/ASSET_FORMATS.md.
//
// This plugin uses only the public plugin interface of include/nue/assets.h.
#include "nfs.hpp"

#include <algorithm>
#include <cstring>

using namespace nuea;

namespace nfs {

// ---------------------------------------------------------------------------------------------------- reading chunk headers
namespace {

class HeaderReader {
public:
    explicit HeaderReader(const nue_asset_source* s) : s_(s), size_(nue_asset_source_size(s)), peek_(nue_asset_source_peek(s, 0, size_t(std::min<uint64_t>(size_, SIZE_MAX)))) {}
    // false when the 8 bytes cannot be read
    bool read(uint64_t off, uint32_t* id, uint32_t* size) {
        const uint8_t* p = nullptr;
        if (peek_) { if (off + 8 > size_) return false; p = static_cast<const uint8_t*>(peek_) + off; }
        else {
            if (off < base_ || off + 8 > base_ + n_) {
                buf_.resize(kWindow);
                size_t got = 0;
                if (nue_asset_source_read(s_, off, buf_.data(), buf_.size(), &got) < 0 || got < 8) return false;
                base_ = off; n_ = got;
            }
            p = buf_.data() + (off - base_);
        }
        *id = rd32(p);
        *size = rd32(p + 4);
        return true;
    }
private:
    static constexpr size_t kWindow = 1u << 18;
    const nue_asset_source* s_;
    uint64_t size_;
    const void* peek_;
    std::vector<uint8_t> buf_;
    uint64_t base_ = 0, n_ = 0;
};

}  // namespace

int32_t NUE_CALL probe(void*, const void* head, size_t head_size, uint64_t total) {
    const uint8_t* h = static_cast<const uint8_t*>(head);
    if (total < 8 || head_size < 8) return 0;
    // Follow the chunk chain through the bytes we have: every header must fit the file, and the chain must be consistent.
    uint64_t off = 0;
    int checked = 0;
    while (off + 8 <= head_size && checked < 8) {
        const uint32_t id = rd32(h + off), size = rd32(h + off + 4);
        if (id == 0xFFFFFFFFu || off + 8 + size > total) return 0;
        ++checked;
        if (id & 0x80000000u) {                          // descend into the first child when it is visible and fits; otherwise the group is opaque
            bool descend = size >= 8 && off + 16 <= head_size && uint64_t(rd32(h + off + 12)) + 8 <= size;
            off += descend ? 8 : 8 + uint64_t(size);
        } else {
            off += 8 + uint64_t(size);
        }
    }
    return checked >= 1 ? 40 + 6 * checked : 0;
}

nue_result NUE_CALL open(void*, nue_asset_open_ctx* oc) {
    const nue_asset_source* src = oc->source;
    const uint64_t total = nue_asset_source_size(src);
    const bool lenient = (oc->flags & NUE_ASSET_OPEN_LENIENT) != 0;
    HeaderReader hr(src);

    struct Frame { uint32_t node; uint64_t end; };
    std::vector<Frame> stack;
    stack.push_back({NUE_ASSET_NO_NODE, total});
    uint64_t cur = 0;

    while (!stack.empty()) {
        Frame f = stack.back();
        bool bad = false;
        uint32_t id = 0, size = 0;
        if (cur + 8 > f.end) {
            bad = cur != f.end;                          // 1..7 leftover bytes
        } else if (!hr.read(cur, &id, &size) || uint64_t(size) > f.end - (cur + 8)) {
            bad = true;                                  // unreadable header, or the chunk runs out of its parent
        }
        if (cur == f.end || bad) {
            if (bad) {
                if (f.node == NUE_ASSET_NO_NODE) {
                    if (!lenient) { nue_set_last_error("nfs: chunk at offset %llu does not fit the file", (unsigned long long)cur); return NUE_ERR_CORRUPT; }
                    nue_asset_node_info t;
                    NUE_STRUCT_INIT(&t);
                    t.parent = NUE_ASSET_NO_NODE;
                    t.kind = NUE_ASSET_NODE_TAIL;
                    t.offset = cur;
                    t.size = f.end - cur;
                    nue_result r = oc->ops->add_node(oc, &t, nullptr);
                    if (r < 0) return r;
                } else {
                    oc->ops->truncate_children(oc, f.node);      // not a real group after all: keep its bytes as one blob
                }
            }
            cur = f.end;
            stack.pop_back();
            continue;
        }
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        n.parent = f.node;
        n.id = id;
        n.header_size = 8;
        n.offset = cur + 8;
        n.size = size;
        const uint64_t after = cur + 8 + size;
        if (id == 0 && !(id & 0x80000000u)) {
            n.kind = NUE_ASSET_NODE_PADDING;
            if (after < f.end) n.align = uint32_t(std::min<uint64_t>(lowbit(after), kMaxPadAlign));   // a next sibling starts at `after`
        } else if (id & 0x80000000u) {
            n.kind = NUE_ASSET_NODE_GROUP;
        } else {
            n.kind = NUE_ASSET_NODE_BLOB;
        }
        uint32_t idx = 0;
        nue_result r = oc->ops->add_node(oc, &n, &idx);
        if (r < 0) return r;
        if (n.kind == NUE_ASSET_NODE_GROUP) { stack.push_back({idx, after}); cur += 8; }
        else cur = after;
    }
    return NUE_OK;
}

void NUE_CALL close(void*, void*) {}

const char* NUE_CALL default_codec(void*, const char* ext) { return ext && std::strcmp(ext, ".lzc") == 0 ? "jdlz" : nullptr; }

// ---------------------------------------------------------------------------------------------------- writing
namespace {

struct Emitter {
    nue_asset_container c;
    nue_asset_sink* sink;
    const std::vector<const std::vector<uint8_t>*>& override_payload;       // per node: replaced payload (generated tables)
    const std::vector<int>& data_plan;                                      // per node: index of the TPK plan that generates it, or -1
    const std::vector<TpkPlan>& plans;

    nue_result copy_payload(uint32_t node, const nue_asset_node_info& info) {
        std::vector<uint8_t> tmp;
        uint64_t done = 0;
        while (done < info.size) {
            if (const void* p = nue_asset_node_peek(c, node, done, size_t(info.size - done))) return nue_asset_sink_write(sink, p, size_t(info.size - done));
            tmp.resize(size_t(std::min<uint64_t>(info.size - done, 1u << 20)));
            size_t got = 0;
            nue_result r = nue_asset_node_read(c, node, done, tmp.data(), tmp.size(), &got);
            if (r < 0) return r;
            if (!got) return NUE_ERR_IO;
            r = nue_asset_sink_write(sink, tmp.data(), got);
            if (r < 0) return r;
            done += got;
        }
        return NUE_OK;
    }

    nue_result emit_all() {
        const uint32_t count = nue_asset_node_count(c);
        struct Frame { uint32_t node; uint32_t next_child; uint64_t header_pos; };
        std::vector<Frame> stack;
        uint32_t root_next = 0;
        // first root = node 0
        auto close_chunk = [&](uint64_t header_pos) -> nue_result {
            uint8_t sz[4];
            wr32(sz, uint32_t(nue_asset_sink_position(sink) - header_pos - 8));
            return nue_asset_sink_patch(sink, header_pos + 4, sz, 4);
        };
        // visit(node): writes the node; returns true if it opened a group frame
        auto visit = [&](uint32_t idx, bool* opened) -> nue_result {
            *opened = false;
            nue_asset_node_info n;
            NUE_STRUCT_INIT(&n);
            nue_asset_node_get(c, idx, &n);
            if (n.kind == NUE_ASSET_NODE_TAIL) return copy_payload(idx, n);
            const uint64_t hpos = nue_asset_sink_position(sink);
            uint8_t h[8];
            wr32(h, uint32_t(n.id));
            wr32(h + 4, 0);
            nue_result r = nue_asset_sink_write(sink, h, 8);
            if (r < 0) return r;
            if (n.kind == NUE_ASSET_NODE_GROUP) {
                stack.push_back({idx, n.first_child, hpos});
                *opened = true;
                return NUE_OK;
            }
            if (n.kind == NUE_ASSET_NODE_PADDING) {
                uint64_t size = n.size;
                if (n.align > 1 && !(n.flags & NUE_ASSET_NODE_EDITED)) {
                    const uint64_t a = n.align, pay = hpos + 8;
                    size = (n.size / a) * a + ((a - pay % a) % a);                  // keep any extra blocks, re-aim the end at the alignment
                }
                if (size == n.size && !(n.flags & NUE_ASSET_NODE_EDITED)) { r = copy_payload(idx, n); if (r < 0) return r; }
                else {
                    std::vector<uint8_t> z(size_t(std::min<uint64_t>(size, 1u << 16)), 0);
                    for (uint64_t left = size; left;) { size_t k = size_t(std::min<uint64_t>(left, z.size())); r = nue_asset_sink_write(sink, z.data(), k); if (r < 0) return r; left -= k; }
                }
            } else if (data_plan[idx] >= 0) {
                r = emit_tpk_data(c, sink, plans[size_t(data_plan[idx])], hpos + 8);
                if (r < 0) return r;
            } else if (override_payload[idx]) {
                r = nue_asset_sink_write(sink, override_payload[idx]->data(), override_payload[idx]->size());
                if (r < 0) return r;
            } else {
                r = copy_payload(idx, n);
                if (r < 0) return r;
            }
            return close_chunk(hpos);
        };
        while (true) {
            uint32_t next;
            if (stack.empty()) {
                if (root_next >= count) break;
                next = root_next;
                nue_asset_node_info n;
                NUE_STRUCT_INIT(&n);
                nue_asset_node_get(c, next, &n);
                root_next = n.next_sibling == NUE_ASSET_NO_NODE ? count : n.next_sibling;
            } else {
                Frame& f = stack.back();
                if (f.next_child == NUE_ASSET_NO_NODE) {
                    nue_result r = close_chunk(f.header_pos);
                    stack.pop_back();
                    if (r < 0) return r;
                    continue;
                }
                next = f.next_child;
                nue_asset_node_info n;
                NUE_STRUCT_INIT(&n);
                nue_asset_node_get(c, next, &n);
                f.next_child = n.next_sibling;
            }
            bool opened;
            nue_result r = visit(next, &opened);
            if (r < 0) return r;
        }
        return NUE_OK;
    }
};

}  // namespace

nue_result NUE_CALL write(void*, nue_asset_container c, void*, uint32_t flags, nue_asset_sink* sink) {
    const uint32_t count = nue_asset_node_count(c);
    std::vector<const std::vector<uint8_t>*> over(count, nullptr);
    std::vector<int> data_plan(count, -1);
    std::vector<TpkPlan> plans;
    const bool regenerate_all = (flags & NUE_ASSET_WRITE_REGENERATE) != 0;
    for (uint32_t i = 0; i < count; ++i) {
        nue_asset_node_info n;
        NUE_STRUCT_INIT(&n);
        nue_asset_node_get(c, i, &n);
        if (n.id != kIdTpk || n.kind != NUE_ASSET_NODE_GROUP) continue;
        Tpk t;
        if (!load_tpk(c, i, &t)) continue;                                  // unrecognised layout: its chunks stay as they are
        bool any_edit = regenerate_all;
        for (uint32_t r = 0; r < nue_asset_resource_count(c) && !any_edit; ++r) {
            nue_asset_resource_info ri;
            NUE_STRUCT_INIT(&ri);
            nue_asset_resource_get(c, r, &ri);
            if (ri.kind == NUE_ASSET_RES_TEXTURE && ri.node == i && nue_asset_resource_edit(c, r)) any_edit = true;
        }
        if (!any_edit) continue;
        plans.emplace_back();
        nue_result r = plan_tpk(c, t, &plans.back());
        if (r < 0) return r;
    }
    for (size_t p = 0; p < plans.size(); ++p) {
        over[plans[p].rec_node] = &plans[p].rec_bytes;
        if (plans[p].fmt_node != NUE_ASSET_NO_NODE) over[plans[p].fmt_node] = &plans[p].fmt_bytes;
        data_plan[plans[p].data_node] = int(p);
    }
    Emitter e{c, sink, over, data_plan, plans};
    return e.emit_all();
}

}  // namespace nfs

NUE_ASSETS_API nue_result NUE_CALL nue_assets_register_nfs(nue_assets_context ctx) {
    nue_asset_format f;
    NUE_STRUCT_INIT(&f);
    f.name = "nfs-bun";
    f.description = "Need for Speed Underground 2 chunk stream (.BUN, and .LZC = JDLZ compressed .BUN)";
    f.extensions = "bun;lzc";
    f.probe = nfs::probe;
    f.open = nfs::open;
    f.close = nfs::close;
    f.index = nfs::index;
    f.get_image = nfs::get_image;
    f.write = nfs::write;
    f.default_codec = nfs::default_codec;
    nue_result r = nue_assets_register_format(ctx, &f);
    return r == NUE_ERR_ALREADY_EXISTS ? NUE_OK : r;
}
