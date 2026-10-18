// Layer: the state shadow (plain stores; nothing reaches the GPU before a draw), state blocks, render target bookkeeping and passes, clears.
#include "layer/layer.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace nue::d3d9 {
using namespace k;

void LayerImpl::set_render_state(uint32_t state, uint32_t value) {
    if (state >= 256) return;
    if (recording_) { recording_->values.rs[state] = value; recording_->rs.set(state); return; }
    state_.rs[state] = value;
}

static uint32_t sampler_slot(uint32_t sampler) {
    if (sampler < 16) return sampler;
    if (sampler >= 257 && sampler < 261) return 16 + (sampler - 257);
    return ~0u;
}

void LayerImpl::set_sampler_state(uint32_t sampler, uint32_t type, uint32_t value) {
    const uint32_t slot = sampler_slot(sampler);
    if (slot == ~0u || type >= ss_count) return;
    if (recording_) { recording_->values.ss[slot][type] = value; recording_->ss.set(slot * ss_count + type); return; }
    state_.ss[slot][type] = value;
}

void LayerImpl::set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value) {
    if (stage >= 8 || type >= tss_count) return;
    if (recording_) { recording_->values.tss[stage][type] = value; recording_->tss.set(stage * tss_count + type); return; }
    state_.tss[stage][type] = value;
}

void LayerImpl::set_transform(uint32_t type, const float* m) {
    float* target = nullptr;
    DeviceState& st = recording_ ? recording_->values : state_;
    if (type == ts_view) { target = st.view; if (recording_) recording_->view = true; }
    else if (type == ts_projection) { target = st.projection; if (recording_) recording_->projection = true; }
    else if (type >= ts_texture0 && type < ts_texture0 + 8) { target = st.texture[type - ts_texture0]; if (recording_) recording_->texture_transforms.set(type - ts_texture0); }
    else if (type == ts_world) { target = st.world; if (recording_) recording_->world = true; }
    else { note_unsupported("set_transform: world matrix beyond 0"); return; }
    std::memcpy(target, m, 64);
}

void LayerImpl::light_enable(uint32_t index, bool enable) {
    if (index >= 32) return;
    if (recording_) { recording_->lights_mask |= 1u << index; if (enable) recording_->lights_value |= 1u << index; else recording_->lights_value &= ~(1u << index); return; }
    if (enable) state_.lights_enabled |= 1u << index; else state_.lights_enabled &= ~(1u << index);
}

void LayerImpl::set_viewport(const uint32_t* r, float min_z, float max_z) {
    const Viewport v{r[0], r[1], r[2], r[3], min_z, max_z};
    if (recording_) { recording_->values.viewport = v; recording_->viewport = true; return; }
    state_.viewport = v;
}

void LayerImpl::set_texture(uint32_t sampler, TextureId id) {
    const uint32_t slot = sampler_slot(sampler);
    if (slot == ~0u) return;
    if (id.value && !textures_.get(id.value)) { bad_handle("set_texture"); return; }
    if (recording_) { recording_->values.textures[slot] = id.value; recording_->textures.set(slot); return; }
    state_.textures[slot] = id.value;
}

void LayerImpl::set_render_target(uint32_t index, SurfaceId id) {
    if (index >= 4) return;
    if (id.value && !surfaces_.get(id.value)) { bad_handle("set_render_target"); return; }
    state_.render_targets[index] = id.value;
}

void LayerImpl::set_depth_stencil(SurfaceId id) {
    if (id.value && !surfaces_.get(id.value)) { bad_handle("set_depth_stencil"); return; }
    state_.depth_stencil = id.value;
}

void LayerImpl::set_vertex_declaration(DeclarationId id) {
    if (recording_) { recording_->values.declaration = id.value; recording_->values.fvf = 0; recording_->declaration = true; return; }
    state_.declaration = id.value;
    state_.fvf = 0;
}

void LayerImpl::set_fvf(uint32_t fvf) {
    if (recording_) { recording_->values.fvf = fvf; recording_->values.declaration = 0; recording_->fvf = true; return; }
    state_.fvf = fvf;
    state_.declaration = 0;
}

void LayerImpl::set_vertex_shader(ShaderId id) {
    if (recording_) { recording_->values.vertex_shader = id.value; recording_->vertex_shader = true; return; }
    state_.vertex_shader = id.value;
}

void LayerImpl::set_pixel_shader(ShaderId id) {
    if (recording_) { recording_->values.pixel_shader = id.value; recording_->pixel_shader = true; return; }
    state_.pixel_shader = id.value;
}

void LayerImpl::set_vertex_shader_constants_f(uint32_t start, std::span<const Float4> values) {
    DeviceState& st = recording_ ? recording_->values : state_;
    for (size_t i = 0; i < values.size() && start + i < st.vs_constants.size(); ++i) {
        st.vs_constants[start + i] = values[i];
        if (recording_) recording_->vs_constants.set(start + i);
    }
}

void LayerImpl::set_pixel_shader_constants_f(uint32_t start, std::span<const Float4> values) {
    DeviceState& st = recording_ ? recording_->values : state_;
    for (size_t i = 0; i < values.size() && start + i < st.ps_constants.size(); ++i) {
        st.ps_constants[start + i] = values[i];
        if (recording_) recording_->ps_constants.set(start + i);
    }
}

void LayerImpl::set_stream_source(uint32_t stream, BufferId id, uint32_t offset, uint32_t stride) {
    if (stream >= 16) return;
    if (id.value && !buffers_.get(id.value)) { bad_handle("set_stream_source"); return; }
    const StreamBinding b{id.value, offset, stride};
    if (recording_) { recording_->values.streams[stream] = b; recording_->streams.set(stream); return; }
    state_.streams[stream] = b;
}

void LayerImpl::set_indices(BufferId id) {
    if (id.value && !buffers_.get(id.value)) { bad_handle("set_indices"); return; }
    if (recording_) { recording_->values.index_buffer = id.value; recording_->index_buffer = true; return; }
    state_.index_buffer = id.value;
}

// ---- state blocks: calls between Begin and End are recorded, not applied (as Direct3D 9 does) ------------------------------------------------

StateBlockId LayerImpl::begin_state_block() {
    auto block = std::make_unique<StateBlock>();
    init_state(block->values);
    recording_id_ = blocks_.add(std::move(block));
    recording_ = blocks_.get(recording_id_)->get();
    return StateBlockId{recording_id_};
}

void LayerImpl::end_state_block(StateBlockId) {
    recording_ = nullptr;
    recording_id_ = 0;
}

void LayerImpl::apply(StateBlockId id) {
    auto* slot = blocks_.get(id.value);
    if (!slot) { bad_handle("apply"); return; }
    const StateBlock& b = **slot;
    for (uint32_t i = 0; i < 256; ++i) if (b.rs.test(i)) state_.rs[i] = b.values.rs[i];
    for (uint32_t s = 0; s < sampler_slots; ++s) for (uint32_t t = 0; t < ss_count; ++t) if (b.ss.test(s * ss_count + t)) state_.ss[s][t] = b.values.ss[s][t];
    for (uint32_t s = 0; s < 8; ++s) for (uint32_t t = 0; t < tss_count; ++t) if (b.tss.test(s * tss_count + t)) state_.tss[s][t] = b.values.tss[s][t];
    for (uint32_t s = 0; s < sampler_slots; ++s) if (b.textures.test(s)) state_.textures[s] = b.values.textures[s];
    for (uint32_t s = 0; s < 16; ++s) if (b.streams.test(s)) state_.streams[s] = b.values.streams[s];
    for (uint32_t i = 0; i < 256; ++i) if (b.vs_constants.test(i)) state_.vs_constants[i] = b.values.vs_constants[i];
    for (uint32_t i = 0; i < 224; ++i) if (b.ps_constants.test(i)) state_.ps_constants[i] = b.values.ps_constants[i];
    for (uint32_t i = 0; i < 8; ++i) if (b.texture_transforms.test(i)) std::memcpy(state_.texture[i], b.values.texture[i], 64);
    if (b.index_buffer) state_.index_buffer = b.values.index_buffer;
    if (b.declaration) { state_.declaration = b.values.declaration; state_.fvf = 0; }
    if (b.fvf) { state_.fvf = b.values.fvf; state_.declaration = 0; }
    if (b.vertex_shader) state_.vertex_shader = b.values.vertex_shader;
    if (b.pixel_shader) state_.pixel_shader = b.values.pixel_shader;
    if (b.viewport) state_.viewport = b.values.viewport;
    if (b.view) std::memcpy(state_.view, b.values.view, 64);
    if (b.projection) std::memcpy(state_.projection, b.values.projection, 64);
    if (b.world) std::memcpy(state_.world, b.values.world, 64);
    state_.lights_enabled = (state_.lights_enabled & ~b.lights_mask) | (b.lights_value & b.lights_mask);
}

// ---- render targets and passes --------------------------------------------------------------------------------------------------------------

LayerImpl::Attachment LayerImpl::resolve_attachments() {
    Attachment a;
    for (uint32_t i = 0; i < 4; ++i) {
        SurfaceObject* s = surfaces_.get(state_.render_targets[i]);
        if (!s) continue;
        gpu::Texture tex;
        switch (s->kind) {
        case SurfaceObject::Kind::backbuffer: tex = gpu::backbuffer; break;
        case SurfaceObject::Kind::render_target: tex = s->gpu; break;
        case SurfaceObject::Kind::texture_level: if (auto* t = textures_.get(s->texture)) tex = t->gpu; break;
        default: break;
        }
        a.color[i] = tex;
        if (i == 0) { a.width = s->width; a.height = s->height; }
    }
    a.valid = bool(a.color[0]);
    if (SurfaceObject* d = surfaces_.get(state_.depth_stencil)) if (d->kind == SurfaceObject::Kind::depth) a.depth = d->gpu;
    return a;
}

bool LayerImpl::same_attachments(const gpu::PassDesc& p, const Attachment& a) const {
    for (int i = 0; i < 4; ++i) if (!(p.colors[i] == a.color[i])) return false;
    return p.depth_stencil == a.depth;
}

void LayerImpl::end_pass() {
    if (!pass_open_) return;
    if (pass_submitted_) gpu_.end_pass();
    else if (pass_.clear_color || pass_.clear_depth || pass_.clear_stencil) { gpu_.begin_pass(pass_); gpu_.end_pass(); ++stats_.passes; }
    pass_open_ = pass_submitted_ = false;
}

void LayerImpl::open_pass(const Attachment& a) {
    pass_ = gpu::PassDesc{};
    for (int i = 0; i < 4; ++i) pass_.colors[i] = a.color[i];
    pass_.depth_stencil = a.depth;
    pass_attachments_ = a;
    pass_open_ = true;
    pass_submitted_ = false;
}

void LayerImpl::ensure_pass() {
    Attachment a = resolve_attachments();
    if (pass_open_ && same_attachments(pass_, a)) {
        pass_attachments_ = a;
    } else {
        end_pass();
        if (!a.valid) return;
        open_pass(a);
    }
    if (!pass_submitted_) { gpu_.begin_pass(pass_); pass_submitted_ = true; ++stats_.passes; }
}

void LayerImpl::clear(std::span<const int32_t> rects, uint32_t flags, uint32_t argb, float z, uint32_t stencil) {
    ++stats_.clears;
    const Attachment a = resolve_attachments();
    if (!a.valid) return;
    if (!a.depth) flags &= ~(clear_zbuffer | clear_stencil);
    if (!flags) return;
    const int32_t full[4] = {0, 0, int32_t(a.width), int32_t(a.height)};
    // Direct3D 9 clears the viewport when no rectangles are given.
    int32_t area[4] = {int32_t(state_.viewport.x), int32_t(state_.viewport.y), int32_t(state_.viewport.x + state_.viewport.width), int32_t(state_.viewport.y + state_.viewport.height)};
    if (!state_.viewport.width || !state_.viewport.height) std::memcpy(area, full, sizeof full);
    auto clip = [&](int32_t* r) { r[0] = std::max(r[0], 0); r[1] = std::max(r[1], 0); r[2] = std::min(r[2], full[2]); r[3] = std::min(r[3], full[3]); };
    std::vector<std::array<int32_t, 4>> list;
    if (rects.size() >= 4) {
        for (size_t i = 0; i + 4 <= rects.size(); i += 4) {
            std::array<int32_t, 4> r{rects[i], rects[i + 1], rects[i + 2], rects[i + 3]};
            clip(r.data());
            // a rectangle outside the viewport is not clamped by Direct3D 9 but the viewport bounds the clear of the whole target
            list.push_back(r);
        }
    } else {
        clip(area);
        list.push_back({area[0], area[1], area[2], area[3]});
    }
    for (auto& r : list) {
        if (r[2] <= r[0] || r[3] <= r[1]) continue;
        const bool whole = r[0] <= 0 && r[1] <= 0 && r[2] >= full[2] && r[3] >= full[3];
        auto apply_clear = [&](gpu::PassDesc& p) {
            if (flags & clear_target) { p.clear_color = true; p.clear_argb = argb; }
            if (flags & clear_zbuffer) { p.clear_depth = true; p.clear_z = z; }
            if (flags & clear_stencil) { p.clear_stencil = true; p.clear_stencil_value = uint8_t(stencil); }
        };
        if (whole) {
            if (pass_open_ && !pass_submitted_ && same_attachments(pass_, a)) { apply_clear(pass_); continue; }   // folded into the pending pass
            end_pass();
            open_pass(a);
            apply_clear(pass_);
        } else {
            ++stats_.partial_clears;
            end_pass();
            open_pass(a);
            pass_.rect[0] = r[0]; pass_.rect[1] = r[1]; pass_.rect[2] = r[2] - r[0]; pass_.rect[3] = r[3] - r[1];
            apply_clear(pass_);
            end_pass();   // a clear of a sub-rectangle gets a pass of its own: later draws must not inherit the rectangle
        }
    }
}

}  // namespace nue::d3d9
