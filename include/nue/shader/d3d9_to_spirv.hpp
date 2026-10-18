// Lowering of decoded Direct3D 9 shaders (d3d9_shader.hpp) to SPIR-V, in the interface conventions the bgfx shader container expects
// (docs/PLATFORM_API.md, 3.4): one uniform block per stage (set 0, binding 0 = vertex, 1 = pixel) holding a fixup vec4 and the float
// constants, textures at binding 2 + stage, samplers at binding 18 + stage, vertex inputs at the location of their bgfx attribute.
#pragma once
#include "nue/shader/d3d9_shader.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace nue::shader {

struct TranslateOptions {
    // Texture type bound to each sampler/stage when the shader itself does not say (pixel shaders 1.x): 1 = 2D, 2 = 3D, 3 = cube.
    uint8_t sampler_dim[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // D3DTTFF_PROJECTED per stage: 0 = not projected, 3 = divide by the third coordinate, 4 = divide by the fourth.
    uint8_t projected[16] = {};
    // Vertex shaders: the viewport, depth range and half-pixel offset of Direct3D 9 are applied to the clip-space position from the first two
    // registers of the uniform block (fix0 = scale x, scale y, bias x, bias y; fix1 = scale z, bias z): x' = x * sx + w * bx and so on.
    // The layer computes them from the viewport, the render target size and the pixel-centre rule (see Layer::fixup_registers).
    bool viewport_fixup = true;
    // Pixel shaders below 2.0 clamp constant registers to [-1, 1] when they are read.
    bool clamp_ps1_constants = true;
    // Vertex shaders: bit n set = the input bound to bgfx attribute n holds a D3DCOLOR (stored B,G,R,A): the shader reads it as R,G,B,A.
    uint32_t bgra_attributes = 0;
    // Pixel shaders: alpha test after the shader (D3DCMP_*: 0 = off, 1 never ... 8 always; reference in fix0.x) and fog (0 = off, 1 = linear from the
    // interpolated fog factor of the vertex stage, which is already the blend factor; fix0.yzw hold start, end, density for table fog, fix1 the colour).
    uint8_t alpha_func = 0;
    uint8_t fog_mode = 0;
    // The first constant registers of the uniform block that are not shader constants.
    static constexpr uint32_t fixup_registers = 2;
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
