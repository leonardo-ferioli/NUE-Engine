#include "nue/shader/cross_compile.hpp"
#include "spirv_glsl.hpp"
#include "spirv_hlsl.hpp"
#include "spirv_msl.hpp"
#include <exception>

namespace nue::shader {

const char* target_name(Target t) {
    switch (t) {
    case Target::glsl_es300: return "glsl_es300";
    case Target::glsl330: return "glsl330";
    case Target::msl: return "msl";
    case Target::hlsl50: return "hlsl50";
    }
    return "?";
}

bool cross_compile(const std::vector<uint32_t>& spirv, Stage stage, Target target, const CrossOptions& options, std::string& text, std::string& error) {
    try {
        switch (target) {
        case Target::glsl_es300: case Target::glsl330: {
            spirv_cross::CompilerGLSL compiler(spirv);
            auto glsl = compiler.get_common_options();
            glsl.es = target == Target::glsl_es300;
            glsl.version = target == Target::glsl_es300 ? 300 : 330;
            glsl.vertex.flip_vert_y = options.flip_vertical && stage == Stage::vertex;
            glsl.vertex.fixup_clipspace = options.fixup_clip_space && stage == Stage::vertex;
            glsl.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
            glsl.emit_uniform_buffer_as_plain_uniforms = true;   // bgfx's OpenGL backend finds uniforms by name
            glsl.enable_420pack_extension = false;
            compiler.set_common_options(glsl);
            // The SPIR-V keeps texture and sampler apart (Vulkan, Metal, Direct3D); OpenGL has combined samplers named after the stage.
            compiler.build_combined_image_samplers();
            for (auto& combined : compiler.get_combined_image_samplers()) {
                const uint32_t binding = compiler.get_decoration(combined.image_id, spv::DecorationBinding);
                compiler.set_name(combined.combined_id, "s_tex" + std::to_string(binding >= 2 ? binding - 2 : binding));
            }
            text = compiler.compile();
            return true;
        }
        case Target::msl: {
            spirv_cross::CompilerMSL compiler(spirv);
            auto msl = compiler.get_msl_options();
            msl.platform = spirv_cross::CompilerMSL::Options::macOS;
            msl.set_msl_version(2, 0);
            compiler.set_msl_options(msl);
            auto common = compiler.get_common_options();
            common.vertex.flip_vert_y = options.flip_vertical && stage == Stage::vertex;
            common.vertex.fixup_clipspace = options.fixup_clip_space && stage == Stage::vertex;
            compiler.set_common_options(common);
            text = compiler.compile();
            return true;
        }
        case Target::hlsl50: {
            spirv_cross::CompilerHLSL compiler(spirv);
            spirv_cross::CompilerHLSL::Options hlsl;
            hlsl.shader_model = 50;
            compiler.set_hlsl_options(hlsl);
            // Direct3D 11 has 16 sampler slots: the SPIR-V bindings (textures 2 + n, samplers 18 + n, constants 0/1) are folded onto b0, t0..t15, s0..s15.
            const spv::ExecutionModel model = stage == Stage::vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
            spirv_cross::HLSLResourceBinding ubo{};
            ubo.stage = model; ubo.desc_set = 0; ubo.binding = stage == Stage::vertex ? 0 : 1; ubo.cbv.register_space = 0; ubo.cbv.register_binding = 0;
            compiler.add_hlsl_resource_binding(ubo);
            for (uint32_t n = 0; n < 16; ++n) {
                spirv_cross::HLSLResourceBinding tex{};
                tex.stage = model; tex.desc_set = 0; tex.binding = 2 + n; tex.srv.register_space = 0; tex.srv.register_binding = n;
                compiler.add_hlsl_resource_binding(tex);
                spirv_cross::HLSLResourceBinding smp{};
                smp.stage = model; smp.desc_set = 0; smp.binding = 18 + n; smp.sampler.register_space = 0; smp.sampler.register_binding = n;
                compiler.add_hlsl_resource_binding(smp);
            }
            text = compiler.compile();
            return true;
        }
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    error = "unknown target";
    return false;
}

}  // namespace nue::shader
