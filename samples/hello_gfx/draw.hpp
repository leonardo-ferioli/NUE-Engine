// hello_gfx drawing helper: a coloured triangle and a textured quad with ONE pre-compiled program, header only so the local GPU test can reuse it.
//
// The shaders are bgfx's own pre-compiled "ocornut_imgui" pair (vs_/fs_ocornut_imgui.bin.h in third_party/bgfx.cmake/bgfx/examples/common/imgui, BSD-2-Clause,
// see docs/THIRD_PARTY_GFX.md): the .bin.h files already carry the binaries for Direct3D 11/12, Vulkan, Metal, OpenGL and OpenGL ES, so the sample needs no
// shader compiler at build time and runs on every renderer the platform has. Vertex: float2 position (pixels), float2 uv, 4 x u8 colour; the shader
// multiplies texel * colour and uses bgfx's u_viewProj.
#pragma once

#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>

#include "common/imgui/fs_ocornut_imgui.bin.h"
#include "common/imgui/vs_ocornut_imgui.bin.h"

#include <cstdint>
#include <cstring>

namespace hello_gfx {

static const bgfx::EmbeddedShader kShaders[] = {
    BGFX_EMBEDDED_SHADER(vs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER(fs_ocornut_imgui),
    BGFX_EMBEDDED_SHADER_END()
};

struct Vertex { float x, y, u, v; uint32_t abgr; };

struct Pipeline {
    bgfx::VertexLayout layout;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle sampler = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle white = BGFX_INVALID_HANDLE, checker = BGFX_INVALID_HANDLE;
    bool noop = false;

    bool init() {
        const bgfx::RendererType::Enum type = bgfx::getRendererType();
        noop = type == bgfx::RendererType::Noop;
        layout.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
              .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true).end();
        bgfx::ShaderHandle vs = bgfx::createEmbeddedShader(kShaders, type, "vs_ocornut_imgui");
        bgfx::ShaderHandle fs = bgfx::createEmbeddedShader(kShaders, type, "fs_ocornut_imgui");
        program = bgfx::createProgram(vs, fs, true);
        sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
        const uint32_t flags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
        const uint32_t one = 0xffffffffu;
        white = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, flags, bgfx::copy(&one, sizeof one));
        uint32_t px[8 * 8];
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) px[y * 8 + x] = ((x ^ y) & 1) ? 0xffe0e0e0u : 0xff303030u;
        checker = bgfx::createTexture2D(8, 8, false, 1, bgfx::TextureFormat::RGBA8, flags, bgfx::copy(px, sizeof px));
        return noop || bgfx::isValid(program);
    }

    void shutdown() {
        if (bgfx::isValid(checker)) bgfx::destroy(checker);
        if (bgfx::isValid(white)) bgfx::destroy(white);
        if (bgfx::isValid(sampler)) bgfx::destroy(sampler);
        if (bgfx::isValid(program)) bgfx::destroy(program);
        checker = white = BGFX_INVALID_HANDLE; sampler = BGFX_INVALID_HANDLE; program = BGFX_INVALID_HANDLE;
    }

    // Pixel coordinates, origin top-left, y down: fills view `view`'s transform.
    static void set_view_2d(bgfx::ViewId view, int w, int h) {
        const float proj[16] = {2.0f / float(w), 0, 0, 0, 0, -2.0f / float(h), 0, 0, 0, 0, 1, 0, -1, 1, 0, 1};
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        bgfx::setViewTransform(view, identity, proj);
    }

    // `count` vertices as a triangle list.
    void draw(bgfx::ViewId view, const Vertex* v, uint32_t count, bgfx::TextureHandle texture) const {
        if (bgfx::getAvailTransientVertexBuffer(count, layout) < count) return;
        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, count, layout);
        std::memcpy(tvb.data, v, sizeof(Vertex) * count);
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setTexture(0, sampler, texture);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(view, program);
    }

    void quad(bgfx::ViewId view, float x, float y, float w, float h, uint32_t abgr, bgfx::TextureHandle texture) const {
        const Vertex q[6] = {{x, y, 0, 0, abgr}, {x + w, y, 1, 0, abgr}, {x + w, y + h, 1, 1, abgr},
                             {x, y, 0, 0, abgr}, {x + w, y + h, 1, 1, abgr}, {x, y + h, 0, 1, abgr}};
        draw(view, q, 6, texture);
    }

    void triangle(bgfx::ViewId view, const float p[6], const uint32_t abgr[3]) const {
        const Vertex t[3] = {{p[0], p[1], 0.5f, 0.5f, abgr[0]}, {p[2], p[3], 0.5f, 0.5f, abgr[1]}, {p[4], p[5], 0.5f, 0.5f, abgr[2]}};
        draw(view, t, 3, white);
    }
};

}  // namespace hello_gfx
