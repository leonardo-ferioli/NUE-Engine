// Layer: fixed-function pipeline glue. Builds the key of the generated vertex / pixel programs from the state and fills their constants
// (register layout: see layer/fixed_function.hpp).
#include "layer/fixed_function.hpp"
#include "layer/layer.hpp"
#include <algorithm>
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
    for (uint32_t s = 0; s < 8; ++s) {
        key.coord_index[s] = uint8_t(state_.tss[s][tss_texcoordindex] & 0xFFFF);
        if (state_.tss[s][tss_texcoordindex] >> 16) note_unsupported("texture coordinate generation");
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
        if (t[tss_colorop] >= 22 && t[tss_colorop] <= 23) note_unsupported("bump-mapping texture operations");
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

void LayerImpl::fill_ff_vertex_constants(std::array<Float4, 48>& c) const {
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
    for (int s = 0; s < 8; ++s) {
        if (16 + 4 * s + 3 >= int(c.size())) break;   // only the first stages fit the constant block; later stages rarely transform
        for (int i = 0; i < 4; ++i) c[16 + 4 * s + i] = column(state_.texture[s], i);
    }
}

void LayerImpl::fill_ff_pixel_constants(std::array<Float4, 48>& c) const {
    c[0] = color_float4(state_.rs[rs_texturefactor]);
    for (int s = 0; s < 8; ++s) c[1 + s] = color_float4(state_.tss[s][tss_constant]);
}

}  // namespace nue::d3d9
