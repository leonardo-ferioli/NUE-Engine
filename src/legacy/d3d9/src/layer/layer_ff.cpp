// Layer: fixed-function pipeline glue. Builds the key of the generated vertex / pixel programs from the state and fills their constants
// (register layout: see layer/fixed_function.hpp).
#include "layer/fixed_function.hpp"
#include "layer/layer.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nue::d3d9 {
using namespace k;

namespace {
float as_float(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

void multiply(const float* a, const float* b, float* out) {   // row-major 4x4: out = a * b
    float r[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float v = 0; for (int q = 0; q < 4; ++q) v += a[i * 4 + q] * b[q * 4 + j]; r[i * 4 + j] = v; }
    std::memcpy(out, r, sizeof r);
}
Float4 column(const float* m, int col) { return Float4{m[col], m[4 + col], m[8 + col], m[12 + col]}; }
Float4 color_float4(uint32_t argb) { return Float4{float((argb >> 16) & 0xFF) / 255.0f, float((argb >> 8) & 0xFF) / 255.0f, float(argb & 0xFF) / 255.0f, float(argb >> 24) / 255.0f}; }
}  // namespace

ShaderObject* LayerImpl::fixed_function_vertex(const Declaration& decl) {
    FfVertexKey key;
    for (const DeclElement& e : decl.elements) {
        if (e.usage == du_position || e.usage == du_positiont) { key.in.position = true; key.in.rhw = e.usage == du_positiont; }
        else if (e.usage == du_normal) key.in.normal = true;
        else if (e.usage == du_color && e.usage_index == 0) key.in.diffuse = true;
        else if (e.usage == du_color && e.usage_index == 1) key.in.specular = true;
        else if (e.usage == du_texcoord && e.usage_index < 8) key.in.texcoord_present_mask |= uint8_t(1u << e.usage_index);
    }
    key.stages = 8;
    if (state_.rs[rs_lighting] && !key.in.rhw) {
        key.lighting = true;
        key.specular_enable = state_.rs[rs_specularenable] != 0;
        key.local_viewer = state_.rs[rs_localviewer] != 0;
        const bool vertex_colors = state_.rs[rs_colorvertex] != 0;
        const uint32_t sources[4] = {state_.rs[rs_diffusematerialsource], state_.rs[rs_specularmaterialsource], state_.rs[rs_ambientmaterialsource], state_.rs[rs_emissivematerialsource]};
        for (int i = 0; i < 4; ++i) key.material_source[i] = uint8_t(vertex_colors && sources[i] <= 2 ? sources[i] : 0);
        for (uint32_t i = 0; i < 32 && key.light_count < 8; ++i)
            if ((state_.lights_enabled & (1u << i)) && state_.lights_defined.test(i)) key.light_type[key.light_count++] = uint8_t(std::clamp(state_.lights[i].type, 1u, 3u));
    }
    for (uint32_t s = 0; s < 8; ++s) {
        key.coord_index[s] = uint8_t(state_.tss[s][tss_texcoordindex] & 0xFFFF);
        key.coord_gen[s] = uint8_t((state_.tss[s][tss_texcoordindex] >> 16) & 7);
        if (key.coord_gen[s] > 4) { key.coord_gen[s] = 0; note_unsupported_literal("texture coordinate generation mode"); }
        const uint32_t flags = state_.tss[s][tss_texturetransformflags] & 0xFF;
        key.transform_count[s] = uint8_t(flags >= 2 && flags <= 4 ? flags : 0);
    }
    if (state_.rs[rs_fogenable]) {
        const uint32_t mode = state_.rs[rs_fogtablemode] ? state_.rs[rs_fogtablemode] : state_.rs[rs_fogvertexmode];
        key.fog_mode = mode == 3 ? 1 : mode == 1 ? 2 : mode == 2 ? 3 : 0;
    }
    const uint64_t h = key.hash();
    auto it = ff_shaders_.find(h);
    if (it == ff_shaders_.end()) {
        auto obj = std::make_unique<ShaderObject>();
        obj->vertex = true; obj->valid = true; obj->hash = h;
        obj->program = build_ff_vertex(key);
        it = ff_shaders_.emplace(h, std::move(obj)).first;
    }
    return it->second.get();
}

ShaderObject* LayerImpl::fixed_function_pixel() {
    FfPixelKey key;
    key.specular = state_.rs[rs_specularenable] != 0;
    for (uint32_t s = 0; s < 8; ++s) {
        const auto& t = state_.tss[s];
        if (t[tss_colorop] == 1) break;   // D3DTOP_DISABLE ends the cascade
        FfPixelStage& st = key.stage[s];
        st.color_op = uint8_t(t[tss_colorop]); st.color_arg0 = uint8_t(t[tss_colorarg0]); st.color_arg1 = uint8_t(t[tss_colorarg1]); st.color_arg2 = uint8_t(t[tss_colorarg2]);
        st.alpha_op = uint8_t(t[tss_alphaop]); st.alpha_arg0 = uint8_t(t[tss_alphaarg0]); st.alpha_arg1 = uint8_t(t[tss_alphaarg1]); st.alpha_arg2 = uint8_t(t[tss_alphaarg2]);
        st.result_arg = uint8_t(t[tss_resultarg]);
        auto uses_texture = [](uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2) {
            const bool three = op == 25 || op == 26;
            return ((a1 & 7) == 2) || ((a2 & 7) == 2) || (three && (a0 & 7) == 2) || op == 13 || op == 15;
        };
        st.texture = uses_texture(st.color_op, st.color_arg0, st.color_arg1, st.color_arg2) || (st.alpha_op != 1 && uses_texture(st.alpha_op, st.alpha_arg0, st.alpha_arg1, st.alpha_arg2));
        if (t[tss_colorop] >= 22 && t[tss_colorop] <= 23) note_unsupported_literal("bump-mapping texture operations");
        key.stage_count = uint8_t(s + 1);
    }
    const uint64_t h = key.hash() ^ 0xFF00FF00FF00FF00ull;
    auto it = ff_shaders_.find(h);
    if (it == ff_shaders_.end()) {
        auto obj = std::make_unique<ShaderObject>();
        obj->vertex = false; obj->valid = true; obj->hash = h;
        obj->program = build_ff_pixel(key);
        it = ff_shaders_.emplace(h, std::move(obj)).first;
    }
    return it->second.get();
}

void LayerImpl::fill_ff_vertex_constants(std::array<Float4, 128>& c) const {
    float wv[16], wvp[16];
    multiply(state_.world, state_.view, wv);
    multiply(wv, state_.projection, wvp);
    for (int i = 0; i < 4; ++i) c[i] = column(wvp, i);
    for (int i = 0; i < 3; ++i) c[4 + i] = column(wv, i);
    const Viewport& v = state_.viewport;
    const float vw = float(std::max(v.width, 1u)), vh = float(std::max(v.height, 1u));
    c[7] = Float4{2.0f / vw, -2.0f / vh, -1.0f - 2.0f * float(v.x) / vw, 1.0f + 2.0f * float(v.y) / vh};
    const float start = as_float(state_.rs[rs_fogstart]), end = as_float(state_.rs[rs_fogend]), density = as_float(state_.rs[rs_fogdensity]);
    const float span = end - start != 0.0f ? end - start : 1.0f;
    c[8] = Float4{-1.0f / span, end / span, -density * 1.4426950f, density};
    {   // columns of the inverse transpose of the upper 3x3 of world*view: normals to camera space
        const float* m = wv;
        const float a = m[0], b = m[1], cc = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
        const float det = a * (e * i - f * h) - b * (d * i - f * g) + cc * (d * h - e * g);
        const float r = det != 0.0f ? 1.0f / det : 0.0f;
        const float inv[3][3] = {{(e * i - f * h) * r, (cc * h - b * i) * r, (b * f - cc * e) * r}, {(f * g - d * i) * r, (a * i - cc * g) * r, (cc * d - a * f) * r}, {(d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r}};
        // n' = n * (M^-1)^T, so n'_x = sum_i n_i * inv[0][i]: the matrix columns handed to the shader are the rows of the inverse
        for (int j = 0; j < 3; ++j) c[9 + j] = Float4{inv[j][0], inv[j][1], inv[j][2], 0.0f};
    }
    if (state_.rs[rs_lighting]) {   // lighting constants: global ambient, material, the enabled lights in camera space
        c[48] = color_float4(state_.rs[rs_ambient]);
        const MaterialDesc& m = state_.material;
        c[49] = Float4{m.diffuse[0], m.diffuse[1], m.diffuse[2], m.diffuse[3]};
        c[50] = Float4{m.ambient[0], m.ambient[1], m.ambient[2], m.ambient[3]};
        c[51] = Float4{m.specular[0], m.specular[1], m.specular[2], m.specular[3]};
        c[52] = Float4{m.emissive[0], m.emissive[1], m.emissive[2], m.emissive[3]};
        c[53] = Float4{m.power, 0, 0, 0};
        uint32_t slot = 0;
        const float* V = state_.view;
        auto to_view = [&](const float* p, float w, float out[3]) { for (int j = 0; j < 3; ++j) out[j] = p[0] * V[j] + p[1] * V[4 + j] + p[2] * V[8 + j] + w * V[12 + j]; };
        for (uint32_t i = 0; i < 32 && slot < 8; ++i) {
            if (!(state_.lights_enabled & (1u << i)) || !state_.lights_defined.test(i)) continue;
            const LightDesc& l = state_.lights[i];
            const uint32_t b = 56 + 7 * slot++;
            float pos[3], dir[3];
            to_view(l.position, 1.0f, pos);
            to_view(l.direction, 0.0f, dir);
            const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const float inv = len > 0.0f ? 1.0f / len : 0.0f;
            const float ct = std::cos(l.theta * 0.5f), cp = std::cos(l.phi * 0.5f);
            c[b + 0] = Float4{l.diffuse[0], l.diffuse[1], l.diffuse[2], l.diffuse[3]};
            c[b + 1] = Float4{l.specular[0], l.specular[1], l.specular[2], l.specular[3]};
            c[b + 2] = Float4{l.ambient[0], l.ambient[1], l.ambient[2], l.ambient[3]};
            c[b + 3] = Float4{pos[0], pos[1], pos[2], l.range};
            c[b + 4] = Float4{dir[0] * inv, dir[1] * inv, dir[2] * inv, l.falloff};
            c[b + 5] = Float4{l.attenuation[0], l.attenuation[1], l.attenuation[2], cp};
            c[b + 6] = Float4{ct, ct - cp != 0.0f ? 1.0f / (ct - cp) : 0.0f, 0, 0};
        }
    }
    for (int s = 0; s < 8; ++s) {
        if (16 + 4 * s + 3 >= int(c.size())) break;   // only the first stages fit the constant block; later stages rarely transform
        for (int i = 0; i < 4; ++i) c[16 + 4 * s + i] = column(state_.texture[s], i);
    }
}

void LayerImpl::fill_ff_pixel_constants(std::array<Float4, 128>& c) const {
    c[0] = color_float4(state_.rs[rs_texturefactor]);
    for (int s = 0; s < 8; ++s) c[1 + s] = color_float4(state_.tss[s][tss_constant]);
}

}  // namespace nue::d3d9
