// KTX2 reader / writer (supercompression none). The vkFormat field is authoritative; the data format descriptor is written best-effort.
#include "container.hpp"

namespace nuetex {
namespace {

const uint8_t kIdent[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};

#pragma pack(push, 1)
struct Header {
    uint8_t ident[12];
    uint32_t vk_format, type_size, width, height, depth, layer_count, face_count, level_count, supercompression;
    uint32_t dfd_offset, dfd_length, kvd_offset, kvd_length;
    uint64_t sgd_offset, sgd_length;
};
struct LevelIndex { uint64_t offset, length, uncompressed_length; };
#pragma pack(pop)
static_assert(sizeof(Header) == 80, "KTX2 header");

uint64_t gcd(uint64_t a, uint64_t b) { while (b) { const uint64_t t = a % b; a = b; b = t; } return a; }
uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

enum { MODEL_RGBSDA = 1, MODEL_BC1A = 128, MODEL_BC2 = 129, MODEL_BC3 = 130, MODEL_BC4 = 131, MODEL_BC5 = 132, MODEL_BC6H = 133, MODEL_BC7 = 134, MODEL_ETC1 = 160, MODEL_ETC2 = 161,
       MODEL_ASTC = 162, MODEL_PVRTC = 164, MODEL_PVRTC2 = 165 };

void put32(std::vector<uint8_t>& v, uint32_t x) { const uint8_t* b = reinterpret_cast<const uint8_t*>(&x); v.insert(v.end(), b, b + 4); }

struct Sample { uint16_t bit_offset; uint8_t bit_length_m1; uint8_t channel; uint32_t lower, upper; };

// Best-effort basic data format descriptor.
std::vector<uint8_t> build_dfd(Fmt f, uint32_t type_size) {
    const auto* fi = info(f);
    uint32_t model = MODEL_RGBSDA;
    std::vector<Sample> samples;
    std::vector<uint8_t> dfd;
    bool is_float = false, is_signed = false;
    int ts = 1;
    const bool srgb = fi->colorspace == NUE_TEX_COLORSPACE_SRGB;
    if (fi->kind == NUE_TEX_KIND_UNCOMPRESSED) {
        ChanInfo ci[4]; int n = 0;
        if (channel_layout(f, ci, &n, &is_float, &is_signed, &ts))
            for (int i = 0; i < n; ++i) {
                uint8_t ch = ci[i].id;
                if (is_float) ch |= 0x80;
                if (is_signed) ch |= 0x40;
                if (!srgb || ci[i].id == 15) ch |= 0x10;         // linear flag: alpha is always linear
                const uint32_t bl = ci[i].bits;
                uint32_t lo = 0, hi = bl >= 32 ? 0xFFFFFFFFu : (1u << bl) - 1;
                if (is_float) { lo = 0xBF800000u; hi = 0x3F800000u; }
                else if (is_signed) { lo = uint32_t(-int32_t(hi >> 1)); hi = hi >> 1; }
                samples.push_back({ci[i].offset, uint8_t(bl - 1), ch, lo, hi});
            }
    } else {
        auto add = [&](int offset, int len, int ch, uint32_t lo = 0, uint32_t hi = 0xFFFFFFFFu) { samples.push_back({uint16_t(offset), uint8_t(len - 1), uint8_t(ch), lo, hi}); };
        switch (f) {
        case NUE_TEX_BC1_UNORM: case NUE_TEX_BC1_SRGB: model = MODEL_BC1A; add(0, 64, 1, 0, 1); break;
        case NUE_TEX_BC2_UNORM: case NUE_TEX_BC2_SRGB: case NUE_TEX_BC2_PMA_UNORM: model = MODEL_BC2; add(0, 64, 15); add(64, 64, 0); break;
        case NUE_TEX_BC3_UNORM: case NUE_TEX_BC3_SRGB: case NUE_TEX_BC3_PMA_UNORM: model = MODEL_BC3; add(0, 64, 15); add(64, 64, 0); break;
        case NUE_TEX_BC4_UNORM: case NUE_TEX_BC4_SNORM: model = MODEL_BC4; add(0, 64, 0); break;
        case NUE_TEX_BC5_UNORM: case NUE_TEX_BC5_SNORM: model = MODEL_BC5; add(0, 64, 0); add(64, 64, 1); break;
        case NUE_TEX_BC6H_UFLOAT: case NUE_TEX_BC6H_SFLOAT: model = MODEL_BC6H; add(0, 128, 0); break;
        case NUE_TEX_BC7_UNORM: case NUE_TEX_BC7_SRGB: model = MODEL_BC7; add(0, 128, 0); break;
        case NUE_TEX_ETC1_RGB8: model = MODEL_ETC1; add(0, 64, 0); break;
        case NUE_TEX_ETC2_RGB8: case NUE_TEX_ETC2_RGB8_SRGB: model = MODEL_ETC2; add(0, 64, 0); break;
        case NUE_TEX_ETC2_RGB8A1: case NUE_TEX_ETC2_RGB8A1_SRGB: model = MODEL_ETC2; add(0, 64, 1); break;
        case NUE_TEX_ETC2_RGBA8: case NUE_TEX_ETC2_RGBA8_SRGB: model = MODEL_ETC2; add(0, 64, 15); add(64, 64, 2); break;
        case NUE_TEX_EAC_R11_UNORM: case NUE_TEX_EAC_R11_SNORM: model = MODEL_ETC2; add(0, 64, 0); break;
        case NUE_TEX_EAC_RG11_UNORM: case NUE_TEX_EAC_RG11_SNORM: model = MODEL_ETC2; add(0, 64, 0); add(64, 64, 1); break;
        case NUE_TEX_PVRTC1_2BPP: case NUE_TEX_PVRTC1_2BPP_SRGB: case NUE_TEX_PVRTC1_4BPP: case NUE_TEX_PVRTC1_4BPP_SRGB: model = MODEL_PVRTC; add(0, 64, 0); break;
        case NUE_TEX_PVRTC2_2BPP: case NUE_TEX_PVRTC2_2BPP_SRGB: case NUE_TEX_PVRTC2_4BPP: case NUE_TEX_PVRTC2_4BPP_SRGB: model = MODEL_PVRTC2; add(0, 64, 0); break;
        default: if (is_astc(f)) { model = MODEL_ASTC; add(0, 128, 0); } break;
        }
    }
    (void)type_size;
    const uint32_t block_size = 24 + 16 * uint32_t(samples.size());
    put32(dfd, 4 + block_size);                                          // dfdTotalSize
    put32(dfd, 0u | (0u << 17));                                         // vendor id (17 bits) | descriptor type (15 bits)
    put32(dfd, 2u | (block_size << 16));                                 // version number 2 | descriptor block size
    const uint8_t transfer = srgb ? 2 : 1;                               // 1 linear, 2 sRGB
    const uint8_t primaries = 1;                                         // BT.709
    const uint8_t flags = (fi->alpha == NUE_TEX_ALPHA_PREMULTIPLIED) ? 1 : 0;
    put32(dfd, model | (uint32_t(primaries) << 8) | (uint32_t(transfer) << 16) | (uint32_t(flags) << 24));
    put32(dfd, uint32_t(fi->block_width - 1) | (uint32_t(fi->block_height - 1) << 8));          // texel block dimensions 0..3 (minus 1)
    put32(dfd, fi->block_bytes);                                         // bytesPlane0..3
    put32(dfd, 0);                                                       // bytesPlane4..7
    for (const Sample& s : samples) {
        put32(dfd, uint32_t(s.bit_offset) | (uint32_t(s.bit_length_m1) << 16) | (uint32_t(s.channel) << 24));
        put32(dfd, 0);                                                   // sample positions
        put32(dfd, s.lower);
        put32(dfd, s.upper);
    }
    return dfd;
}

}  // namespace

nue_result ktx2_save(const Image& im, const KeyValues* kv, std::vector<uint8_t>& out) {
    const Fmt f = im.desc.format;
    const auto* fi = info(f);
    const uint32_t vk = nue_tex_format_to_vk(f);
    if (!vk) return NUE_ERR_UNSUPPORTED;
    if (f == NUE_TEX_BC2_PMA_UNORM || f == NUE_TEX_BC3_PMA_UNORM) { /* flagged in the DFD; vkFormat is the base format */ }
    int ts = 1; bool fl = false, sg = false; ChanInfo ci[4]; int n = 0;
    if (fi->kind == NUE_TEX_KIND_UNCOMPRESSED && channel_layout(f, ci, &n, &fl, &sg, &ts)) {} else ts = 1;
    const std::vector<uint8_t> dfd = build_dfd(f, uint32_t(ts));
    // key/value data (sorted by key as the spec requires)
    std::vector<uint8_t> kvd;
    KeyValues all;
    if (kv) all = *kv;
    all["KTXwriter"] = std::string("nue_texture codec ") + std::to_string(NUE_TEX_CODEC_VERSION);
    for (const auto& e : all) {
        const uint32_t len = uint32_t(e.first.size() + 1 + e.second.size());
        put32(kvd, len);
        kvd.insert(kvd.end(), e.first.begin(), e.first.end()); kvd.push_back(0);
        kvd.insert(kvd.end(), e.second.begin(), e.second.end());
        while (kvd.size() % 4) kvd.push_back(0);
    }
    const uint32_t levels = im.levels();
    Header h; std::memset(&h, 0, sizeof h);
    std::memcpy(h.ident, kIdent, 12);
    h.vk_format = vk; h.type_size = uint32_t(ts);
    h.width = im.desc.width; h.height = im.desc.height; h.depth = im.desc.depth > 1 ? im.desc.depth : 0;
    h.layer_count = im.desc.layers > 1 ? im.desc.layers : 0;
    h.face_count = im.desc.faces; h.level_count = levels; h.supercompression = 0;
    const uint64_t index_end = sizeof(Header) + uint64_t(levels) * sizeof(LevelIndex);
    h.dfd_offset = uint32_t(index_end); h.dfd_length = uint32_t(dfd.size());
    h.kvd_offset = h.dfd_offset + h.dfd_length; h.kvd_length = uint32_t(kvd.size());
    const uint64_t data_start_unaligned = h.kvd_offset + h.kvd_length;
    const uint64_t lcm = uint64_t(fi->block_bytes) * 4 / gcd(fi->block_bytes, 4);
    // levels are stored smallest first
    std::vector<LevelIndex> idx(levels);
    uint64_t cursor = align_up(data_start_unaligned, std::max<uint64_t>(lcm, 8));
    for (int l = int(levels) - 1; l >= 0; --l) {
        cursor = align_up(cursor, lcm);
        const uint64_t per = im.level_size(uint32_t(l), 0, 0), len = per * im.layers() * im.faces();
        idx[l] = {cursor, len, len};
        cursor += len;
    }
    out.assign(size_t(cursor), 0);
    std::memcpy(out.data(), &h, sizeof h);
    std::memcpy(out.data() + sizeof h, idx.data(), idx.size() * sizeof(LevelIndex));
    std::memcpy(out.data() + h.dfd_offset, dfd.data(), dfd.size());
    if (!kvd.empty()) std::memcpy(out.data() + h.kvd_offset, kvd.data(), kvd.size());
    for (uint32_t l = 0; l < levels; ++l) {
        uint8_t* p = out.data() + idx[l].offset;
        for (uint32_t layer = 0; layer < im.layers(); ++layer)
            for (uint32_t face = 0; face < im.faces(); ++face) {
                const uint64_t sz = im.level_size(l, layer, face);
                std::memcpy(p, im.level_ptr(l, layer, face), size_t(sz));
                p += sz;
            }
    }
    return NUE_OK;
}

nue_result ktx2_load(const uint8_t* data, size_t size, uint32_t flags, Image** out, KeyValues* kv) {
    if (size < sizeof(Header) || std::memcmp(data, kIdent, 12) != 0) return NUE_ERR_CORRUPT;
    Header h; std::memcpy(&h, data, sizeof h);
    if (h.supercompression != 0) return NUE_ERR_UNSUPPORTED;
    Fmt f = nue_tex_format_from_vk(h.vk_format);
    if (f == NUE_TEX_FORMAT_UNKNOWN) return NUE_ERR_UNSUPPORTED;
    if ((flags & NUE_TEX_FILE_ASSUME_SRGB) && info(f)->srgb_twin != f) f = info(f)->srgb_twin;
    const uint32_t levels = h.level_count ? h.level_count : 1;
    if (!h.width || levels > 32 || size < sizeof(Header) + uint64_t(levels) * sizeof(LevelIndex)) return NUE_ERR_CORRUPT;
    if (h.face_count != 1 && h.face_count != 6) return NUE_ERR_CORRUPT;
    nue_tex_image_desc d{};
    d.struct_size = sizeof d;
    d.format = f; d.width = h.width; d.height = h.height ? h.height : 1; d.depth = h.depth ? h.depth : 1;
    d.layers = h.layer_count ? h.layer_count : 1; d.faces = h.face_count; d.mip_levels = levels;
    if (d.faces == 6) d.flags |= NUE_TEX_IMAGE_CUBE;
    Image* im = nullptr;
    nue_result r = image_create(d, &im);
    if (r < 0) return r;
    std::unique_ptr<Image> g(im);
    for (uint32_t l = 0; l < levels; ++l) {
        LevelIndex li; std::memcpy(&li, data + sizeof(Header) + l * sizeof(LevelIndex), sizeof li);
        const uint64_t expect = im->level_size(l, 0, 0) * im->layers() * im->faces();
        if (li.length != expect || li.offset > size || li.length > size - li.offset) return NUE_ERR_CORRUPT;
        const uint8_t* p = data + li.offset;
        for (uint32_t layer = 0; layer < im->layers(); ++layer)
            for (uint32_t face = 0; face < im->faces(); ++face) {
                const uint64_t sz = im->level_size(l, layer, face);
                std::memcpy(im->level_ptr(l, layer, face), p, size_t(sz));
                p += sz;
            }
    }
    if (kv && h.kvd_length && h.kvd_offset + uint64_t(h.kvd_length) <= size) {
        const uint8_t* p = data + h.kvd_offset;
        const uint8_t* end = p + h.kvd_length;
        while (p + 4 <= end) {
            uint32_t len; std::memcpy(&len, p, 4);
            p += 4;
            if (len == 0 || p + len > end) break;
            const uint8_t* nul = static_cast<const uint8_t*>(std::memchr(p, 0, len));
            if (nul) (*kv)[std::string(reinterpret_cast<const char*>(p), nul - p)] = std::string(reinterpret_cast<const char*>(nul + 1), len - size_t(nul - p) - 1);
            p += (len + 3) & ~3u;
        }
    }
    *out = g.release();
    return NUE_OK;
}

}  // namespace nuetex
