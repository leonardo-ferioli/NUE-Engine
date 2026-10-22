#pragma once
#include "internal.hpp"
#include <memory>
#include <new>

namespace nuetex {

struct Image {
    nue_tex_image_desc desc{};
    std::vector<uint8_t> data;
    std::vector<uint64_t> offsets;      // [layer][face][level]
    std::vector<uint64_t> sizes;

    uint32_t layers() const { return desc.layers; }
    uint32_t faces() const { return desc.faces; }
    uint32_t levels() const { return desc.mip_levels; }
    size_t index(uint32_t level, uint32_t layer, uint32_t face) const { return (size_t(layer) * desc.faces + face) * desc.mip_levels + level; }
    uint8_t* level_ptr(uint32_t level, uint32_t layer, uint32_t face) { return data.data() + offsets[index(level, layer, face)]; }
    const uint8_t* level_ptr(uint32_t level, uint32_t layer, uint32_t face) const { return data.data() + offsets[index(level, layer, face)]; }
    uint64_t level_size(uint32_t level, uint32_t layer, uint32_t face) const { return sizes[index(level, layer, face)]; }
    uint32_t w(uint32_t level) const { return nue_tex_mip_extent(desc.width, level); }
    uint32_t h(uint32_t level) const { return nue_tex_mip_extent(desc.height, level); }
    uint32_t d(uint32_t level) const { return nue_tex_mip_extent(desc.depth, level); }
};

inline Image* img(nue_tex_image h) { return reinterpret_cast<Image*>(h); }
inline nue_tex_image handle(Image* i) { return reinterpret_cast<nue_tex_image>(i); }

// Normalises the descriptor (zeros -> defaults), validates, allocates zeroed storage.
nue_result image_create(const nue_tex_image_desc& d, Image** out);
nue_result image_convert(const Image& src, Fmt dst_format, uint32_t dst_levels, const nue_tex_encode_params* enc, const nue_tex_mip_params* mips, Image** out);

// Mip generation helper: RGBA32F slices (depth stacked) -> next level.
struct FloatLevel { uint32_t w = 0, h = 0, d = 1; std::vector<float> px; };      // px = w*h*d*4
void mip_downsample(const FloatLevel& src, uint32_t nw, uint32_t nh, uint32_t nd, const nue_tex_mip_params& p, bool srgb, FloatLevel& out);
void mip_fix_alpha_coverage(FloatLevel& level, float target);
float alpha_coverage(const FloatLevel& l, float threshold);

}  // namespace nuetex
