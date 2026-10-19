// The bgfx shader container (magic VSH/FSH + version 13): written at run time from the SPIR-V the translator produces, so no shaderc is needed.
// One container per stage and renderer: Vulkan takes the SPIR-V itself; Direct3D 11 takes DXBC (SPIR-V -> HLSL by SPIRV-Cross -> D3DCompile, Windows
// only); OpenGL(ES) and Metal take source text (SPIR-V -> GLSL / MSL).
#pragma once
#include "nue/shader/d3d9_to_spirv.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace nue::gpu {

enum class ShaderTarget { spirv, dxbc, glsl_es300, glsl330, msl };

struct ContainerResult {
    std::vector<uint8_t> bytes;
    std::string error;
    double cross_ms = 0, compile_ms = 0;   // SPIRV-Cross and driver/compiler time, for the measurements
    bool from_cache = false;               // the container came from the disk cache (no cross-compile, no compiler)
};

// `vs` selects the vertex magic. `uniform_vec4s` is the length of the stage's uniform block (fixup registers + constants).
// `cache_dir` (may be empty) is a folder where compiled containers are kept between runs, keyed by a hash of everything the container depends on: the
// Direct3D 11 compiler costs about 20 ms per shader, which would otherwise be a visible stall the first time every program is used.
bool build_container(const shader::TranslateResult& translated, ShaderTarget target, ContainerResult& out, const std::string& cache_dir = {});

// bgfx attribute enum (our shader path's attribute numbers) -> the id bgfx writes in containers.
uint16_t bgfx_attribute_id(uint32_t attribute);

}  // namespace nue::gpu
