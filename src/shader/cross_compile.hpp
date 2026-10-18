// SPIR-V -> shading language text for the bgfx renderers that consume source (docs/PLATFORM_API.md, 3.4): GLSL ES 3.0 and GLSL 3.30 for
// OpenGL (ES), MSL for Metal, HLSL (shader model 5) for Direct3D 11/12. Vulkan takes the SPIR-V itself. Uses SPIRV-Cross (Apache-2.0).
#pragma once
#include "shader/d3d9_shader.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace nue::shader {

enum class Target { glsl_es300, glsl330, msl, hlsl50 };
const char* target_name(Target);

struct CrossOptions {
    bool flip_vertical = false;       // OpenGL-style framebuffers: flip clip-space y of vertex shaders
    bool fixup_clip_space = false;    // OpenGL clip-space depth [-1, 1] (Direct3D and Vulkan use [0, 1])
};

// Returns false and sets `error` when SPIRV-Cross rejects the module.
bool cross_compile(const std::vector<uint32_t>& spirv, Stage stage, Target target, const CrossOptions& options, std::string& text, std::string& error);

}  // namespace nue::shader
