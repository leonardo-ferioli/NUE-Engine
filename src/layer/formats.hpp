// Direct3D 9 surface formats: sizes, the GPU format each one maps to, and the conversions for formats a modern GPU API does not have.
#pragma once
#include "nue/gpu.hpp"
#include <cstdint>
#include <vector>

namespace nue::d3d9 {

// Direct3D 9 formats without an alpha channel (X8R8G8B8, R5G6B5, X1R5G5B5, R8G8B8, X4R4G4B4): alpha reads as 1 and writes to it are discarded.
inline bool format_has_alpha(uint32_t format) { return !(format == 22 || format == 23 || format == 24 || format == 20 || format == 30); }

enum class Convert : uint8_t {
    none,            // the GPU format has the same memory layout
    x8_opaque,       // X8R8G8B8 / X8B8G8R8: force alpha to 255 (bgra8 / rgba8)
    abgr8,           // A8B8G8R8 stored as RGBA8 bytes already (no conversion) - kept for completeness
    r8g8b8,          // 24-bit BGR -> BGRA8
    l8,              // luminance -> RGBA8 (l, l, l, 1)
    a8l8,            // alpha+luminance -> RGBA8
    a4l4,            // 4-bit alpha + luminance -> RGBA8
    r3g3b2, a8r3g3b2,
    x4r4g4b4,        // alpha forced to 15
    x1r5g5b5,        // alpha forced to 1
    g16r16, a16b16g16r16, l16, v8u8, q8w8v8u8, v16u16,   // 16-bit / bump formats approximated as RGBA8
    dxt1, dxt3, dxt5,   // block compressed: decoded to RGBA8 when the GPU lacks BCn
    a2b10g10r10, a2r10g10b10,
};

struct FormatInfo {
    uint32_t d3d = 0;
    const char* name = "?";
    uint8_t block_w = 1, block_h = 1, block_bytes = 0;   // block_bytes is bytes per pixel for uncompressed formats
    gpu::Format native = gpu::Format::count;             // GPU format with the same layout (count: none)
    gpu::Format fallback = gpu::Format::rgba8;            // what a converted texture uses
    Convert convert = Convert::none;
    bool depth = false, compressed = false, valid = false;
};

const FormatInfo& format_info(uint32_t d3d_format);

// Bytes of one row of blocks and of a whole level (width x height, mip-adjusted by the caller) in the Direct3D format.
uint32_t level_pitch(const FormatInfo&, uint32_t width);
uint32_t level_bytes(const FormatInfo&, uint32_t width, uint32_t height);
uint32_t mip_dimension(uint32_t base, uint32_t level);
uint32_t mip_count(uint32_t width, uint32_t height);

// The GPU format a texture of `d3d_format` is created with, given what the GPU supports.
gpu::Format gpu_format_for(const FormatInfo&, const gpu::GpuCaps&);
// Converts a rectangle of `width` x `height` texels from the Direct3D layout to `target` (the format gpu_format_for returned). `out_pitch` is the
// row pitch of the result in bytes. When no conversion is needed the data is copied unchanged.
void convert_pixels(const FormatInfo&, gpu::Format target, const uint8_t* src, uint32_t src_pitch, uint32_t width, uint32_t height, std::vector<std::byte>& out, uint32_t& out_pitch);

// BC1/BC2/BC3 block decoders (the portable fallback for GPUs without block compression).
void decode_dxt(Convert kind, const uint8_t* src, uint32_t src_pitch, uint32_t width, uint32_t height, uint8_t* rgba, uint32_t rgba_pitch);

}  // namespace nue::d3d9
