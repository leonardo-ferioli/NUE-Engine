// The Direct3D 9 fixed-function pipeline as generated shaders: the vertex transform / colour / texture-coordinate / fog stage and the texture-stage
// combiners are built as decoded shader programs (nue::shader::Program) from the device state and go through the same translator as the game's own
// shaders. Constant register layout (both stages are filled by the layer, see LayerImpl::fill_ff_constants):
//   vertex: c0-c3 world*view*projection (columns), c4-c6 world*view columns x, y, z, c7 pre-transformed vertex scale/bias (2/w, -2/h, -1-2x/w, 1+2y/h),
//           c8 fog (a, b, -density*log2(e), density) with linear fog = d*a + b, c9-c11 columns of the inverse transpose of world*view (normals to camera space),
//           c48 global ambient, c49-c53 material (diffuse, ambient, specular, emissive, power.x), c56 + 7*light: diffuse, specular, ambient, position (camera space) + range,
//           direction (camera space, unit) + falloff, attenuation 0-2 + cos(phi/2), cos(theta/2) + 1/(cos(theta/2) - cos(phi/2)),
//           c16 + 4*stage: texture transform columns. Pre-transformed (XYZRHW) vertices skip the texture transform and coordinate generation, as the real runtime does.
//   pixel : c0 texture factor, c1 + stage: the stage's constant colour
// Lighting follows the fixed-function equations (ambient, diffuse, specular, point / spot / directional lights, colour material sources); normals are always normalised.
// Not implemented (reported as unsupported when met): vertex blending, point sprites, bump mapping.
#pragma once
#include "nue/shader/d3d9_shader.hpp"
#include <cstdint>
#include <vector>

namespace nue::d3d9 {

struct FfVertexInput { bool position = true, rhw = false, normal = false, diffuse = false, specular = false; uint8_t texcoords = 0; uint8_t texcoord_present_mask = 0; };

struct FfVertexKey {
    FfVertexInput in;
    uint8_t stages = 0;                    // stages whose coordinates the pixel stage reads
    uint8_t coord_index[8] = {};           // TEXCOORDINDEX & 0xFFFF per stage
    uint8_t transform_count[8] = {};       // D3DTTFF_COUNT (0 = no transform)
    bool lighting = false, specular_enable = false, local_viewer = false;
    uint8_t light_count = 0;               // enabled lights used (the first 8)
    uint8_t light_type[8] = {};            // 1 point, 2 spot, 3 directional
    uint8_t material_source[4] = {};       // diffuse, specular, ambient, emissive: 0 material, 1 vertex diffuse, 2 vertex specular
    uint8_t coord_gen[8] = {};             // TEXCOORDINDEX >> 16: 0 passthrough, 1 camera-space normal, 2 camera-space position, 3 reflection vector, 4 sphere map
    uint8_t fog_mode = 0;                  // 0 none, 1 linear, 2 exp, 3 exp2 (vertex fog)
    bool operator==(const FfVertexKey&) const = default;
    uint64_t hash() const;
};

struct FfPixelStage { uint8_t color_op = 1, color_arg0 = 1, color_arg1 = 2, color_arg2 = 1, alpha_op = 1, alpha_arg0 = 1, alpha_arg1 = 2, alpha_arg2 = 1, result_arg = 1; bool texture = false; };
struct FfPixelKey {
    FfPixelStage stage[8];
    uint8_t stage_count = 0;               // stages up to the first disabled one
    bool specular = false;
    bool operator==(const FfPixelKey&) const = default;
    uint64_t hash() const;
};

shader::Program build_ff_vertex(const FfVertexKey&);
shader::Program build_ff_pixel(const FfPixelKey&);

}  // namespace nue::d3d9
