#include "layer/formats.hpp"
#include "layer/d3d9_consts.hpp"
#include <algorithm>
#include <cstring>

namespace nue::d3d9 {
namespace {
using GF = gpu::Format;
using namespace k;

constexpr FormatInfo make(uint32_t d3d, const char* name, uint8_t bw, uint8_t bh, uint8_t bytes, GF native, Convert convert = Convert::none,
                          bool depth = false, bool compressed = false, GF fallback = GF::rgba8) {
    return FormatInfo{d3d, name, bw, bh, bytes, native, fallback, convert, depth, compressed, true};
}

const FormatInfo table[] = {
    make(fmt_a8r8g8b8, "A8R8G8B8", 1, 1, 4, GF::bgra8),
    make(fmt_x8r8g8b8, "X8R8G8B8", 1, 1, 4, GF::count, Convert::x8_opaque, false, false, GF::bgra8),
    make(fmt_a8b8g8r8, "A8B8G8R8", 1, 1, 4, GF::rgba8),
    make(fmt_x8b8g8r8, "X8B8G8R8", 1, 1, 4, GF::count, Convert::x8_opaque, false, false, GF::rgba8),
    make(fmt_r8g8b8, "R8G8B8", 1, 1, 3, GF::count, Convert::r8g8b8, false, false, GF::bgra8),
    make(fmt_r5g6b5, "R5G6B5", 1, 1, 2, GF::count, Convert::none, false, false, GF::bgra8),   // decoded on the CPU: the GPU formats swap red and blue (nue_format_test)
    make(fmt_x1r5g5b5, "X1R5G5B5", 1, 1, 2, GF::count, Convert::x1r5g5b5),
    make(fmt_a1r5g5b5, "A1R5G5B5", 1, 1, 2, GF::a1r5g5b5),
    make(fmt_a4r4g4b4, "A4R4G4B4", 1, 1, 2, GF::a4r4g4b4),
    make(fmt_x4r4g4b4, "X4R4G4B4", 1, 1, 2, GF::count, Convert::x4r4g4b4),
    make(fmt_r3g3b2, "R3G3B2", 1, 1, 1, GF::count, Convert::r3g3b2),
    make(fmt_a8r3g3b2, "A8R3G3B2", 1, 1, 2, GF::count, Convert::a8r3g3b2),
    make(fmt_a8, "A8", 1, 1, 1, GF::a8),
    make(fmt_l8, "L8", 1, 1, 1, GF::count, Convert::l8),
    make(fmt_a8l8, "A8L8", 1, 1, 2, GF::count, Convert::a8l8),
    make(fmt_a4l4, "A4L4", 1, 1, 1, GF::count, Convert::a4l4),
    make(fmt_l16, "L16", 1, 1, 2, GF::count, Convert::l16),
    make(fmt_g16r16, "G16R16", 1, 1, 4, GF::count, Convert::g16r16),
    make(fmt_a16b16g16r16, "A16B16G16R16", 1, 1, 8, GF::count, Convert::a16b16g16r16),
    make(fmt_a2b10g10r10, "A2B10G10R10", 1, 1, 4, GF::count, Convert::a2b10g10r10),
    make(fmt_a2r10g10b10, "A2R10G10B10", 1, 1, 4, GF::count, Convert::a2r10g10b10),
    make(fmt_v8u8, "V8U8", 1, 1, 2, GF::count, Convert::v8u8, false, false, GF::rgba8s),
    make(fmt_q8w8v8u8, "Q8W8V8U8", 1, 1, 4, GF::rgba8s),
    make(fmt_v16u16, "V16U16", 1, 1, 4, GF::count, Convert::v16u16),
    make(fmt_r16f, "R16F", 1, 1, 2, GF::r16f),
    make(fmt_g16r16f, "G16R16F", 1, 1, 4, GF::rg16f),
    make(fmt_a16b16g16r16f, "A16B16G16R16F", 1, 1, 8, GF::rgba16f),
    make(fmt_r32f, "R32F", 1, 1, 4, GF::r32f),
    make(fmt_g32r32f, "G32R32F", 1, 1, 8, GF::rg32f),
    make(fmt_a32b32g32r32f, "A32B32G32R32F", 1, 1, 16, GF::rgba32f),
    make(fmt_dxt1, "DXT1", 4, 4, 8, GF::bc1, Convert::dxt1, false, true),
    make(fmt_dxt2, "DXT2", 4, 4, 16, GF::bc2, Convert::dxt3, false, true),
    make(fmt_dxt3, "DXT3", 4, 4, 16, GF::bc2, Convert::dxt3, false, true),
    make(fmt_dxt4, "DXT4", 4, 4, 16, GF::bc3, Convert::dxt5, false, true),
    make(fmt_dxt5, "DXT5", 4, 4, 16, GF::bc3, Convert::dxt5, false, true),
    make(fmt_d16, "D16", 1, 1, 2, GF::d16, Convert::none, true),
    make(fmt_d16_lockable, "D16_LOCKABLE", 1, 1, 2, GF::d16, Convert::none, true),
    make(fmt_d15s1, "D15S1", 1, 1, 2, GF::d24s8, Convert::none, true),
    make(fmt_d24s8, "D24S8", 1, 1, 4, GF::d24s8, Convert::none, true),
    make(fmt_d24x8, "D24X8", 1, 1, 4, GF::d24s8, Convert::none, true),
    make(fmt_d24x4s4, "D24X4S4", 1, 1, 4, GF::d24s8, Convert::none, true),
    make(fmt_d24fs8, "D24FS8", 1, 1, 4, GF::d24s8, Convert::none, true),
    make(fmt_d32, "D32", 1, 1, 4, GF::d32f, Convert::none, true),
    make(fmt_d32f_lockable, "D32F_LOCKABLE", 1, 1, 4, GF::d32f, Convert::none, true),
};
const FormatInfo unknown_format{};

inline uint8_t expand(uint32_t v, uint32_t bits) { return uint8_t((v * 255u + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

void put(uint8_t* d, uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool bgra) {
    if (bgra) { d[0] = b; d[1] = g; d[2] = r; d[3] = a; } else { d[0] = r; d[1] = g; d[2] = b; d[3] = a; }
}

struct Color { uint8_t r, g, b, a; };
Color from565(uint16_t v) { return {expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5), 255}; }

void decode_color_block(const uint8_t* b, Color out[4], bool allow_transparent) {
    uint16_t c0, c1;
    std::memcpy(&c0, b, 2); std::memcpy(&c1, b + 2, 2);
    out[0] = from565(c0); out[1] = from565(c1);
    if (c0 > c1 || !allow_transparent) {
        for (int i = 0; i < 3; ++i) {
            const uint8_t* a = &out[0].r; const uint8_t* c = &out[1].r;
            (&out[2].r)[i] = uint8_t((2 * a[i] + c[i]) / 3);
            (&out[3].r)[i] = uint8_t((a[i] + 2 * c[i]) / 3);
        }
        out[2].a = out[3].a = 255;
    } else {
        for (int i = 0; i < 3; ++i) (&out[2].r)[i] = uint8_t(((&out[0].r)[i] + (&out[1].r)[i]) / 2);
        out[2].a = 255;
        out[3] = {0, 0, 0, 0};
    }
}
}  // namespace

const FormatInfo& format_info(uint32_t d3d) {
    for (const auto& f : table) if (f.d3d == d3d) return f;
    return unknown_format;
}

uint32_t level_pitch(const FormatInfo& f, uint32_t width) {
    const uint32_t blocks = (width + f.block_w - 1) / f.block_w;
    return std::max(1u, blocks) * f.block_bytes;
}
uint32_t level_bytes(const FormatInfo& f, uint32_t width, uint32_t height) {
    const uint32_t rows = (height + f.block_h - 1) / f.block_h;
    return level_pitch(f, width) * std::max(1u, rows);
}
uint32_t mip_dimension(uint32_t base, uint32_t level) { return std::max(1u, base >> level); }
uint32_t mip_count(uint32_t width, uint32_t height) {
    uint32_t levels = 1, m = std::max(width, height);
    while (m > 1) { m >>= 1; ++levels; }
    return levels;
}

gpu::Format gpu_format_for(const FormatInfo& f, const gpu::GpuCaps& caps) {
    if (f.native != GF::count && caps.texture_format[static_cast<size_t>(f.native)]) return f.native;
    if (f.depth) return f.native;
    if (f.native == GF::count) return f.fallback;
    // A format the GPU lacks (block compression on a phone, 16-bit packed formats): decoded to 8-bit RGBA.
    return GF::rgba8;
}

void decode_dxt(Convert kind, const uint8_t* src, uint32_t src_pitch, uint32_t width, uint32_t height, uint8_t* rgba, uint32_t rgba_pitch) {
    const uint32_t block_bytes = kind == Convert::dxt1 ? 8 : 16;
    for (uint32_t by = 0; by < (height + 3) / 4; ++by) {
        for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx) {
            const uint8_t* blk = src + by * src_pitch + bx * block_bytes;
            Color palette[4];
            uint8_t alpha[16];
            const uint8_t* color_block = blk;
            if (kind == Convert::dxt1) {
                decode_color_block(blk, palette, true);
                for (int i = 0; i < 16; ++i) alpha[i] = 255;
            } else if (kind == Convert::dxt3) {
                color_block = blk + 8;
                decode_color_block(color_block, palette, false);
                for (int i = 0; i < 16; ++i) alpha[i] = expand((blk[i / 2] >> ((i & 1) * 4)) & 15, 4);
            } else {
                color_block = blk + 8;
                decode_color_block(color_block, palette, false);
                uint8_t a[8] = {blk[0], blk[1]};
                if (a[0] > a[1]) { for (int i = 1; i < 7; ++i) a[1 + i] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7); }
                else { for (int i = 1; i < 5; ++i) a[1 + i] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = a[(bits >> (3 * i)) & 7];
            }
            uint32_t indices;
            std::memcpy(&indices, color_block + 4, 4);
            for (uint32_t py = 0; py < 4; ++py) {
                for (uint32_t px = 0; px < 4; ++px) {
                    const uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= width || y >= height) continue;
                    const Color c = palette[(indices >> (2 * (py * 4 + px))) & 3];
                    uint8_t* d = rgba + y * rgba_pitch + x * 4;
                    d[0] = c.r; d[1] = c.g; d[2] = c.b;
                    d[3] = kind == Convert::dxt1 ? c.a : alpha[py * 4 + px];
                }
            }
        }
    }
}

void convert_pixels(const FormatInfo& f, gpu::Format target, const uint8_t* src, uint32_t src_pitch, uint32_t width, uint32_t height, std::vector<std::byte>& out, uint32_t& out_pitch) {
    const bool bgra = target == GF::bgra8;
    const bool to_rgba8 = target == GF::rgba8 || target == GF::bgra8 || target == GF::rgba8s;
    if (!to_rgba8 || (f.native == target && f.convert != Convert::x8_opaque)) {   // same layout: copy rows
        const uint32_t row = level_pitch(f, width), rows = (height + f.block_h - 1) / f.block_h;
        out.resize(size_t(row) * rows);
        for (uint32_t y = 0; y < rows; ++y) std::memcpy(out.data() + size_t(y) * row, src + size_t(y) * src_pitch, row);
        out_pitch = row;
        return;
    }
    out_pitch = width * 4;
    out.resize(size_t(out_pitch) * height);
    uint8_t* dst = reinterpret_cast<uint8_t*>(out.data());
    if (f.convert == Convert::dxt1 || f.convert == Convert::dxt3 || f.convert == Convert::dxt5) {
        decode_dxt(f.convert, src, src_pitch, width, height, dst, out_pitch);
        if (bgra) for (uint32_t i = 0; i < width * height; ++i) std::swap(dst[i * 4], dst[i * 4 + 2]);
        return;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + size_t(y) * src_pitch;
        uint8_t* d = dst + size_t(y) * out_pitch;
        for (uint32_t x = 0; x < width; ++x, d += 4) {
            switch (f.convert) {
            case Convert::x8_opaque: {   // source and target share the channel order: copy and force alpha
                d[0] = s[x * 4]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 2]; d[3] = 255; break;
            }
            case Convert::v8u8: d[0] = s[x * 2]; d[1] = s[x * 2 + 1]; d[2] = 127; d[3] = 127; break;
            case Convert::r8g8b8: put(d, s[x * 3 + 2], s[x * 3 + 1], s[x * 3], 255, bgra); break;   // stored B,G,R
            case Convert::l8: put(d, s[x], s[x], s[x], 255, bgra); break;
            case Convert::a8l8: put(d, s[x * 2], s[x * 2], s[x * 2], s[x * 2 + 1], bgra); break;
            case Convert::a4l4: { const uint8_t v = s[x]; const uint8_t l = expand(v & 15, 4); put(d, l, l, l, expand(v >> 4, 4), bgra); break; }
            case Convert::l16: { const uint8_t l = s[x * 2 + 1]; put(d, l, l, l, 255, bgra); break; }
            case Convert::g16r16: put(d, s[x * 4 + 1], s[x * 4 + 3], 255, 255, bgra); break;   // blue reads as 1
            case Convert::v16u16: put(d, s[x * 4 + 1] ^ 0x80, s[x * 4 + 3] ^ 0x80, 255, 255, bgra); break;
            case Convert::a16b16g16r16: put(d, s[x * 8 + 1], s[x * 8 + 3], s[x * 8 + 5], s[x * 8 + 7], bgra); break;
            case Convert::r3g3b2: { const uint8_t v = s[x]; put(d, expand(v >> 5, 3), expand((v >> 2) & 7, 3), expand(v & 3, 2), 255, bgra); break; }
            case Convert::a8r3g3b2: { uint16_t v; std::memcpy(&v, s + x * 2, 2); put(d, expand((v >> 5) & 7, 3), expand((v >> 2) & 7, 3), expand(v & 3, 2), uint8_t(v >> 8), bgra); break; }
            case Convert::x4r4g4b4: { uint16_t v; std::memcpy(&v, s + x * 2, 2); put(d, expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4), 255, bgra); break; }
            case Convert::x1r5g5b5: { uint16_t v; std::memcpy(&v, s + x * 2, 2); put(d, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5), 255, bgra); break; }
            case Convert::a2r10g10b10: { uint32_t v; std::memcpy(&v, s + x * 4, 4); put(d, uint8_t(((v >> 20) & 1023) >> 2), uint8_t(((v >> 10) & 1023) >> 2), uint8_t((v & 1023) >> 2), expand(v >> 30, 2), bgra); break; }
            case Convert::a2b10g10r10: { uint32_t v; std::memcpy(&v, s + x * 4, 4); put(d, uint8_t((v & 1023) >> 2), uint8_t(((v >> 10) & 1023) >> 2), uint8_t(((v >> 20) & 1023) >> 2), expand(v >> 30, 2), bgra); break; }
            default: {
                // native 16-bit packed layouts decoded because the GPU lacks them: pick by the Direct3D format
                uint16_t v; std::memcpy(&v, s + x * 2, 2);
                if (f.d3d == fmt_r5g6b5) { const Color c = from565(v); put(d, c.r, c.g, c.b, 255, bgra); }
                else if (f.d3d == fmt_a1r5g5b5) put(d, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5), (v >> 15) ? 255 : 0, bgra);
                else if (f.d3d == fmt_a4r4g4b4) put(d, expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4), expand(v >> 12, 4), bgra);
                else put(d, 0, 0, 0, 255, bgra);
                break;
            }
            }
        }
    }
}

}  // namespace nue::d3d9
