// Lowering of decoded Direct3D 9 shaders (d3d9_shader.hpp) to SPIR-V, in the interface conventions the bgfx shader container expects
// (docs/PLATFORM_API.md, 3.4): one uniform block per stage (set 0, binding 0 = vertex, 1 = pixel) holding a fixup vec4 and the float
// constants, textures at binding 2 + stage, samplers at binding 18 + stage, vertex inputs at the location of their bgfx attribute.
#pragma once
#include "shader/d3d9_shader.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace nue::shader {

struct TranslateOptions {
    // Texture type bound to each sampler/stage when the shader itself does not say (pixel shaders 1.x): 1 = 2D, 2 = 3D, 3 = cube.
    uint8_t sampler_dim[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // D3DTTFF_PROJECTED per stage: 0 = not projected, 3 = divide by the third coordinate, 4 = divide by the fourth.
    uint8_t projected[16] = {};
    // Vertex shaders: move clip-space xy by u_fix.xy * w, the Direct3D 9 pixel-centre offset. The layer sets u_fix = (+1/width, -1/height):
    // measured against the real Direct3D 9 device (the opposite sign, and no shift, both differ from it by up to 7/255).
    bool half_pixel_fixup = true;
    // Pixel shaders below 2.0 clamp constant registers to [-1, 1] when they are read.
    bool clamp_ps1_constants = true;
    // OpenGL(ES): the constants are plain uniforms (bgfx finds uniforms by name), not a uniform block; the other renderers use the block.
    bool plain_uniforms = false;
};

struct VertexInput { uint16_t reg; uint32_t usage, usage_index; uint32_t attribute; };   // attribute = bgfx::Attrib::Enum (also the SPIR-V location)

struct TranslateResult {
    std::vector<uint32_t> spirv;
    Stage stage = Stage::vertex;
    uint32_t constant_registers = 0;       // float constants the uniform block holds (the "c" array length)
    uint32_t sampler_mask = 0;             // stages sampled
    std::vector<VertexInput> inputs;       // vertex shaders
    uint32_t interpolants_read = 0;        // pixel shaders: bit 0/1 = v0/v1 (colours), bit 2+n = texcoord n
    std::string error;
};

// bgfx::Attrib::Enum for a vertex declaration usage (D3DDECLUSAGE) and index, and the attribute's name in bgfx's OpenGL backend ("a_position"...).
uint32_t bgfx_attribute_for(uint32_t usage, uint32_t usage_index);
const char* bgfx_attribute_name(uint32_t attribute);

bool translate_to_spirv(const Program& program, const TranslateOptions& options, TranslateResult& out);

}  // namespace nue::shader
