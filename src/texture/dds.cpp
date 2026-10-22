// DDS reader / writer: legacy D3D9 headers (FOURCC and bit masks), DX10 extension header, mips, cube maps, volumes, arrays.
#include "container.hpp"

namespace nuetex {
namespace {

#pragma pack(push, 1)
struct PixelFormat { uint32_t size, flags, fourcc, bits, rmask, gmask, bmask, amask; };
struct Header {
    uint32_t size, flags, height, width, pitch_or_linear, depth, mip_count, reserved1[11];
    PixelFormat pf;
    uint32_t caps, caps2, caps3, caps4, reserved2;
};
struct Header10 { uint32_t dxgi, dimension, misc, array_size, misc2; };
#pragma pack(pop)
static_assert(sizeof(Header) == 124, "DDS header");

enum : uint32_t {
    F_CAPS = 0x1, F_HEIGHT = 0x2, F_WIDTH = 0x4, F_PITCH = 0x8, F_PIXELFORMAT = 0x1000, F_MIPMAPCOUNT = 0x20000, F_LINEARSIZE = 0x80000, F_DEPTH = 0x800000,
    PF_ALPHAPIXELS = 0x1, PF_ALPHA = 0x2, PF_FOURCC = 0x4, PF_RGB = 0x40, PF_LUMINANCE = 0x20000, PF_BUMPDUDV = 0x80000,
    C_COMPLEX = 0x8, C_TEXTURE = 0x1000, C_MIPMAP = 0x400000,
    C2_CUBEMAP = 0x200, C2_CUBEMAP_ALL = 0xFC00, C2_VOLUME = 0x200000
};
const uint32_t kMagic = 0x20534444;      // "DDS "
const uint32_t kDX10 = 0x30315844;       // "DX10"

struct Mask { uint32_t flags, bits, r, g, b, a; Fmt fmt; };
const Mask kMasks[] = {
    {PF_RGB | PF_ALPHAPIXELS, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, NUE_TEX_BGRA8_UNORM},
    {PF_RGB, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0, NUE_TEX_BGRX8_UNORM},
    {PF_RGB | PF_ALPHAPIXELS, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000, NUE_TEX_RGBA8_UNORM},
    {PF_RGB, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0, NUE_TEX_RGBX8_UNORM},
    {PF_RGB, 32, 0x0000FFFF, 0xFFFF0000, 0, 0, NUE_TEX_RG16_UNORM},
    {PF_RGB | PF_ALPHAPIXELS, 32, 0x000003FF, 0x000FFC00, 0x3FF00000, 0xC0000000, NUE_TEX_A2BGR10},
    {PF_RGB | PF_ALPHAPIXELS, 32, 0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000, NUE_TEX_A2RGB10},
    {PF_RGB, 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0, NUE_TEX_BGR8_UNORM},
    {PF_RGB, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0, NUE_TEX_RGB8_UNORM},
    {PF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0, NUE_TEX_RGB565},
    {PF_RGB | PF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000, NUE_TEX_A1RGB5},
    {PF_RGB, 16, 0x7C00, 0x03E0, 0x001F, 0, NUE_TEX_XRGB1555},
    {PF_RGB | PF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000, NUE_TEX_ARGB4},
    {PF_RGB, 16, 0x0F00, 0x00F0, 0x000F, 0, NUE_TEX_XRGB4},
    {PF_RGB | PF_ALPHAPIXELS, 16, 0x00E0, 0x001C, 0x0003, 0xFF00, NUE_TEX_A8RGB332},
    {PF_RGB, 8, 0xE0, 0x1C, 0x03, 0, NUE_TEX_RGB332},
    {PF_LUMINANCE, 8, 0xFF, 0, 0, 0, NUE_TEX_L8_UNORM},
    {PF_LUMINANCE, 16, 0xFFFF, 0, 0, 0, NUE_TEX_L16_UNORM},
    {PF_LUMINANCE | PF_ALPHAPIXELS, 16, 0x00FF, 0, 0, 0xFF00, NUE_TEX_LA8_UNORM},
    {PF_LUMINANCE | PF_ALPHAPIXELS, 8, 0x0F, 0, 0, 0xF0, NUE_TEX_LA4_UNORM},
    {PF_ALPHA, 8, 0, 0, 0, 0xFF, NUE_TEX_A8_UNORM},
    {PF_ALPHAPIXELS, 8, 0, 0, 0, 0xFF, NUE_TEX_A8_UNORM},
    {PF_BUMPDUDV, 16, 0x00FF, 0xFF00, 0, 0, NUE_TEX_RG8_SNORM},
    {PF_BUMPDUDV, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000, NUE_TEX_RGBA8_SNORM},
    {PF_BUMPDUDV, 32, 0x0000FFFF, 0xFFFF0000, 0, 0, NUE_TEX_RG16_SNORM},
};

Fmt format_from_pf(const PixelFormat& pf) {
    if (pf.flags & PF_FOURCC) {
        if (pf.fourcc < 0x100) return nue_tex_format_from_d3d9(pf.fourcc, nullptr);      // numeric D3DFMT stored in the FOURCC field (D3DX style)
        return nue_tex_format_from_d3d9(pf.fourcc, nullptr);
    }
    for (const Mask& m : kMasks) {
        const uint32_t relevant = PF_RGB | PF_LUMINANCE | PF_ALPHA | PF_BUMPDUDV | PF_ALPHAPIXELS;
        uint32_t want = m.flags & relevant, have = pf.flags & relevant;
        if (m.fmt == NUE_TEX_A8_UNORM && (m.flags & PF_ALPHAPIXELS) && !(m.flags & PF_ALPHA)) {}
        if (want == have && pf.bits == m.bits && pf.rmask == m.r && pf.gmask == m.g && pf.bmask == m.b && pf.amask == m.a) return m.fmt;
    }
    return NUE_TEX_FORMAT_UNKNOWN;
}

bool legacy_header_for(Fmt f, PixelFormat& pf) {
    std::memset(&pf, 0, sizeof pf);
    pf.size = 32;
    const uint32_t d3d = nue_tex_format_to_d3d9(f);
    const auto* fi = info(f);
    if (fi->kind == NUE_TEX_KIND_BLOCK) {
        if (!d3d) return false;
        if (f == NUE_TEX_BC4_SNORM || f == NUE_TEX_BC5_SNORM) {}
        pf.flags = PF_FOURCC; pf.fourcc = d3d;
        return true;
    }
    for (const Mask& m : kMasks)
        if (m.fmt == f && !(m.flags == PF_ALPHAPIXELS)) { pf.flags = m.flags; pf.bits = m.bits; pf.rmask = m.r; pf.gmask = m.g; pf.bmask = m.b; pf.amask = m.a; return true; }
    if (d3d && fi->kind == NUE_TEX_KIND_UNCOMPRESSED) { pf.flags = PF_FOURCC; pf.fourcc = d3d; return true; }      // D3DFMT numbers: A16B16G16R16F, R32F...
    return false;
}

}  // namespace

nue_result dds_load(const uint8_t* data, size_t size, uint32_t flags, Image** out) {
    if (size < 4 + sizeof(Header)) return NUE_ERR_CORRUPT;
    uint32_t magic; std::memcpy(&magic, data, 4);
    if (magic != kMagic) return NUE_ERR_CORRUPT;
    Header h; std::memcpy(&h, data + 4, sizeof h);
    if (h.size != 124 || h.pf.size != 32) return NUE_ERR_CORRUPT;
    size_t off = 4 + sizeof h;
    nue_tex_image_desc d{};
    d.struct_size = sizeof d;
    d.width = h.width; d.height = h.height ? h.height : 1;
    d.depth = (h.flags & F_DEPTH) && h.depth > 1 ? h.depth : 1;
    d.mip_levels = (h.flags & F_MIPMAPCOUNT) && h.mip_count ? h.mip_count : 1;
    d.layers = 1; d.faces = 1;
    Fmt f;
    if ((h.pf.flags & PF_FOURCC) && h.pf.fourcc == kDX10) {
        if (size < off + sizeof(Header10)) return NUE_ERR_CORRUPT;
        Header10 x; std::memcpy(&x, data + off, sizeof x);
        off += sizeof x;
        f = nue_tex_format_from_dxgi(x.dxgi);
        d.layers = x.array_size ? x.array_size : 1;
        if (x.misc & 0x4) d.faces = 6;
        if (x.dimension == 4) d.depth = std::max(1u, h.depth);
        if (x.dimension == 2) d.height = 1;
        if (x.dimension == 4 && h.depth > 1) d.depth = h.depth;
    } else {
        f = format_from_pf(h.pf);
        if (h.caps2 & C2_CUBEMAP) { if ((h.caps2 & C2_CUBEMAP_ALL) != C2_CUBEMAP_ALL) return NUE_ERR_UNSUPPORTED; d.faces = 6; }
        if ((h.caps2 & C2_VOLUME) && h.depth > 1) d.depth = h.depth;
    }
    if (f == NUE_TEX_FORMAT_UNKNOWN) return NUE_ERR_UNSUPPORTED;
    if ((flags & NUE_TEX_FILE_ASSUME_SRGB) && info(f)->srgb_twin != f) f = info(f)->srgb_twin;
    d.format = f;
    if (d.depth > 1 && (d.layers > 1 || d.faces > 1)) return NUE_ERR_UNSUPPORTED;
    if (d.faces == 6) d.flags |= NUE_TEX_IMAGE_CUBE;
    if (d.mip_levels > nue_tex_mip_count(d.width, d.height, d.depth)) d.mip_levels = nue_tex_mip_count(d.width, d.height, d.depth);
    Image* im = nullptr;
    nue_result r = image_create(d, &im);
    if (r < 0) return r;
    std::unique_ptr<Image> g(im);
    if (size - off < im->data.size()) {
        // some writers omit the lowest mips: accept a short file only if level 0..N-1 present is impossible to know -> corrupt
        return NUE_ERR_CORRUPT;
    }
    // DDS order is [array][face][mip] = our layout
    std::memcpy(im->data.data(), data + off, im->data.size());
    *out = g.release();
    return NUE_OK;
}

nue_result dds_save(const Image& im, uint32_t flags, std::vector<uint8_t>& out) {
    const Fmt f = im.desc.format;
    const auto* fi = info(f);
    PixelFormat pf;
    const bool legacy_ok = legacy_header_for(f, pf);
    const uint32_t dxgi = nue_tex_format_to_dxgi(f);
    bool use_dx10 = (flags & NUE_TEX_FILE_DDS_FORCE_DX10) || !legacy_ok || im.desc.layers > 1;
    if (f == NUE_TEX_BC1_SRGB || f == NUE_TEX_BC2_SRGB || f == NUE_TEX_BC3_SRGB) use_dx10 = true;       // the colour space only exists in the DX10 header
    if (use_dx10 && !dxgi) return NUE_ERR_UNSUPPORTED;
    if (use_dx10 && (flags & NUE_TEX_FILE_DDS_LEGACY_ONLY)) return NUE_ERR_UNSUPPORTED;
    if (!use_dx10 && !legacy_ok) return NUE_ERR_UNSUPPORTED;
    if (f == NUE_TEX_BC2_PMA_UNORM || f == NUE_TEX_BC3_PMA_UNORM) { if (use_dx10) return NUE_ERR_UNSUPPORTED; }
    Header h; std::memset(&h, 0, sizeof h);
    h.size = 124;
    h.flags = F_CAPS | F_HEIGHT | F_WIDTH | F_PIXELFORMAT | (im.levels() > 1 ? F_MIPMAPCOUNT : 0);
    h.height = im.desc.height; h.width = im.desc.width;
    if (fi->kind == NUE_TEX_KIND_BLOCK) { h.flags |= F_LINEARSIZE; h.pitch_or_linear = uint32_t(im.level_size(0, 0, 0)); }
    else { h.flags |= F_PITCH; h.pitch_or_linear = nue_tex_row_pitch(f, im.desc.width); }
    if (im.desc.depth > 1) { h.flags |= F_DEPTH; h.depth = im.desc.depth; }
    h.mip_count = im.levels();
    if (use_dx10) { std::memset(&h.pf, 0, sizeof h.pf); h.pf.size = 32; h.pf.flags = PF_FOURCC; h.pf.fourcc = kDX10; } else h.pf = pf;
    h.caps = C_TEXTURE | (im.levels() > 1 || im.desc.faces > 1 || im.desc.depth > 1 ? C_COMPLEX : 0) | (im.levels() > 1 ? C_MIPMAP : 0);
    if (im.desc.faces == 6) { h.caps |= C_COMPLEX; h.caps2 |= C2_CUBEMAP | C2_CUBEMAP_ALL; }
    if (im.desc.depth > 1) { h.caps |= C_COMPLEX; h.caps2 |= C2_VOLUME; }
    out.clear();
    out.reserve(4 + sizeof h + sizeof(Header10) + im.data.size());
    auto put = [&](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); out.insert(out.end(), b, b + n); };
    put(&kMagic, 4);
    put(&h, sizeof h);
    if (use_dx10) {
        Header10 x{dxgi, im.desc.depth > 1 ? 4u : 3u, im.desc.faces == 6 ? 4u : 0u, im.desc.layers, 0};
        put(&x, sizeof x);
    }
    put(im.data.data(), im.data.size());
    return NUE_OK;
}

}  // namespace nuetex
