#include "nue/null_gpu.hpp"
#include <algorithm>
#include <cstring>

namespace nue::d3d9 {
const char* method_name(Method m) {
    static const char* const names[] = {
        "reset", "test_cooperative_level", "present",
        "create_texture", "create_cube_texture", "create_vertex_buffer", "create_index_buffer", "create_render_target", "create_depth_stencil",
        "create_vertex_declaration", "create_vertex_shader", "create_pixel_shader",
        "destroy_texture", "destroy_buffer", "destroy_surface", "destroy_declaration", "destroy_shader", "destroy_state_block",
        "lock_buffer", "unlock_buffer", "lock_texture", "unlock_texture", "texture_surface", "get_back_buffer", "stretch_rect",
        "set_render_state", "set_sampler_state", "set_texture_stage_state", "set_transform", "set_viewport", "set_texture", "set_render_target", "set_depth_stencil",
        "set_vertex_declaration", "set_fvf", "set_vertex_shader", "set_pixel_shader", "set_vertex_shader_constants_f", "set_pixel_shader_constants_f",
        "set_stream_source", "set_indices", "begin_state_block", "end_state_block", "apply_state_block",
        "begin_scene", "end_scene", "clear", "draw_indexed", "draw_up", "light_enable",
        "set_vertex_shader_constants_i", "set_pixel_shader_constants_i", "set_vertex_shader_constants_b", "set_pixel_shader_constants_b"};
    static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Method::count), "method_name table out of step with Method");
    return static_cast<size_t>(m) < static_cast<size_t>(Method::count) ? names[static_cast<size_t>(m)] : "?";
}
}  // namespace nue::d3d9

namespace nue {

NullGpu::NullGpu() {
    for (auto& supported : capabilities.texture_format) supported = true;
    capabilities.max_texture_size = 4096;
    capabilities.max_anisotropy = 16;
    capabilities.mrt = true;
}

void NullGpu::mix(uint64_t value) {
    call_hash = (call_hash ^ value) * 0x100000001b3ull;
    call_hash ^= call_hash >> 31;
}

void NullGpu::mix_bytes(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); mix(w); }
    uint64_t tail = 0;
    for (size_t k = 0; i + k < size; ++k) tail |= uint64_t(p[i + k]) << (8 * k);
    mix(tail ^ (uint64_t(size) << 56));
}

Result<gpu::Buffer> NullGpu::create_static_buffer(Bytes initial, gpu::BufferKind kind) {
    ++buffers_created;
    static_buffer_bytes += initial.size();
    mix(0xB1 + uint64_t(kind));
    mix_bytes(initial.data(), initial.size());
    return {Status::ok, gpu::Buffer{++next_id}};
}

Result<gpu::Buffer> NullGpu::allocate_transient(uint32_t bytes, uint32_t stride, bool index, MutableBytes& out) {
    mix(index);
    transient.emplace_back(bytes);
    out = MutableBytes(transient.back().data(), transient.back().size());
    transient_bytes += bytes;
    ++transient_allocations;
    mix(0xB3);
    mix(bytes);
    mix(stride);
    const uint32_t id = ++next_id;
    transient_index[id] = transient.size() - 1;
    return {Status::ok, gpu::Buffer{id}};
}

Result<gpu::Texture> NullGpu::create_texture(uint32_t width, uint32_t height, uint32_t levels, gpu::Format format, bool render_target, bool cube) {
    ++textures_created;
    mix(0xC1);
    mix(width); mix(height); mix(levels); mix(uint64_t(format)); mix(render_target); mix(cube);
    return {Status::ok, gpu::Texture{++next_id}};
}

void NullGpu::update_texture(gpu::Texture texture, uint32_t level, uint32_t face, const uint32_t* rect4, Bytes data, uint32_t row_pitch) {
    ++texture_updates;
    texture_bytes += data.size();
    mix(0xC2);
    mix(texture.value); mix(level); mix(face); mix(row_pitch);
    if (rect4) for (int i = 0; i < 4; ++i) mix(rect4[i]);
    mix_bytes(data.data(), data.size());
}

Result<gpu::Program> NullGpu::create_program(const gpu::ProgramDesc& desc) {
    ++programs_created;
    mix(0xA1);
    mix(desc.key);
    for (const auto* m : {desc.vertex, desc.pixel}) {
        if (!m) { mix(0); continue; }
        mix_bytes(m->spirv.data(), m->spirv.size() * 4);
        mix(m->constant_registers);
        mix(m->sampler_mask);
    }
    return {Status::ok, gpu::Program{++next_id}};
}

void NullGpu::copy_texture(gpu::Texture dst, const int32_t* dst_rect4, gpu::Texture src, const int32_t* src_rect4, bool linear) {
    mix(0xC3); mix(dst.value); mix(src.value); mix(linear);
    for (int i = 0; i < 4; ++i) { mix(dst_rect4 ? uint32_t(dst_rect4[i]) : 0xFFFFFFFFu); mix(src_rect4 ? uint32_t(src_rect4[i]) : 0xFFFFFFFFu); }
    ++copies;
}

void NullGpu::begin_pass(const gpu::PassDesc& pass) {
    ++passes;
    mix(0xE0);
    for (const auto& c : pass.colors) mix(c.value);
    for (int i = 0; i < 4; ++i) mix(uint64_t(pass.color_layer[i]) | uint64_t(pass.color_mip[i]) << 8);
    mix(pass.depth_stencil.value);
    mix(uint64_t(pass.clear_color) | uint64_t(pass.clear_depth) << 1 | uint64_t(pass.clear_stencil) << 2);
    for (int i = 0; i < 4; ++i) mix(uint32_t(pass.rect[i]));
    mix(pass.clear_argb);
    mix_value(pass.clear_z);
    mix(pass.clear_stencil_value);
}

void NullGpu::draw(const gpu::DrawDesc& s) {
    ++draws;
    last_draw_program = s.program;
    mix(0xDD);
    mix(s.program.value);
    for (int i = 0; i < 4; ++i) {
        mix(s.vertex_buffer[i].value); mix(s.vertex_offset[i]); mix(s.vertex_stride[i]);
        if (auto it = transient_index.find(s.vertex_buffer[i].value); it != transient_index.end()) mix_bytes(transient[it->second].data(), transient[it->second].size());
    }
    mix(s.index_buffer.value); mix(s.index_offset); mix(s.index32);
    if (auto it = transient_index.find(s.index_buffer.value); it != transient_index.end()) mix_bytes(transient[it->second].data(), transient[it->second].size());
    for (const auto& a : s.layout) { mix(a.stream); mix(a.offset); mix(a.type); mix(a.semantic); mix(a.index); }
    for (int i = 0; i < 16; ++i) {
        mix(s.textures[i].value);
        if (!s.textures[i]) continue;
        const auto& sm = s.samplers[i];
        mix(uint64_t(sm.min) | uint64_t(sm.mag) << 8 | uint64_t(sm.mip) << 16 | uint64_t(sm.use_mips) << 24 | uint64_t(sm.address_u) << 32 | uint64_t(sm.address_v) << 40 | uint64_t(sm.address_w) << 48 | uint64_t(sm.max_aniso) << 56);
        mix_value(sm.lod_bias); mix(sm.border_argb);
    }
    mix_bytes(s.vertex_constants.data(), s.vertex_constants.size());
    mix_bytes(s.pixel_constants.data(), s.pixel_constants.size());
    constant_bytes += s.vertex_constants.size() + s.pixel_constants.size();
    const auto& b = s.blend;
    mix(uint64_t(b.enable) | uint64_t(b.src) << 8 | uint64_t(b.dst) << 16 | uint64_t(b.src_alpha) << 24 | uint64_t(b.dst_alpha) << 32 | uint64_t(b.op) << 40 | uint64_t(b.op_alpha) << 48 | uint64_t(b.write_mask) << 56);
    mix(b.constant_argb);
    const auto& d = s.depth_stencil;
    mix(uint64_t(d.depth_test) | uint64_t(d.depth_write) << 1 | uint64_t(d.depth_func) << 8 | uint64_t(d.stencil) << 16 | uint64_t(d.stencil_func) << 24 | uint64_t(d.stencil_fail) << 32 | uint64_t(d.stencil_depth_fail) << 40 | uint64_t(d.stencil_pass) << 48);
    mix(uint64_t(d.stencil_ref) | uint64_t(d.stencil_read_mask) << 8 | uint64_t(d.stencil_write_mask) << 16);
    mix(uint64_t(s.raster.cull) | uint64_t(s.raster.wireframe) << 8);
    mix_value(s.raster.depth_bias); mix_value(s.raster.slope_scale_bias);
    mix(uint64_t(s.topology)); mix(uint32_t(s.base_vertex)); mix(s.first_index); mix(s.index_count); mix(s.vertex_count);
    for (int i = 0; i < 4; ++i) mix(uint32_t(s.scissor[i]));
    mix(s.scissor_enabled);
    vertices_drawn += s.vertex_count;
    indices_drawn += s.index_count;
}

Status NullGpu::present(uint32_t interval) {
    ++frames;
    mix(0xF1);
    mix(interval);
    transient.clear();
    transient_index.clear();
    return Status::ok;
}

Result<d3d9::BufferId> NullDevice::create_vertex_buffer(uint32_t bytes, uint32_t, uint32_t, uint32_t) {
    count(d3d9::Method::create_vertex_buffer);
    const uint32_t id = ++next_id;
    buffers[id].assign(bytes, std::byte{0});
    return {Status::ok, d3d9::BufferId{id}};
}

Result<d3d9::BufferId> NullDevice::create_index_buffer(uint32_t bytes, uint32_t, uint32_t, uint32_t) {
    count(d3d9::Method::create_index_buffer);
    const uint32_t id = ++next_id;
    buffers[id].assign(bytes, std::byte{0});
    return {Status::ok, d3d9::BufferId{id}};
}

Result<d3d9::LockedRegion> NullDevice::lock_buffer(d3d9::BufferId id, uint32_t offset, uint32_t bytes, uint32_t) {
    count(d3d9::Method::lock_buffer);
    auto found = buffers.find(id.value);
    if (found == buffers.end()) return {Status::invalid_argument, {}};
    auto& data = found->second;
    if (bytes == 0) bytes = uint32_t(data.size()) - std::min<uint32_t>(offset, uint32_t(data.size()));
    if (uint64_t(offset) + bytes > data.size()) return {Status::invalid_argument, {}};
    return {Status::ok, d3d9::LockedRegion{data.data() + offset, bytes, 0}};
}

Result<d3d9::LockedRegion> NullDevice::lock_texture(d3d9::TextureId id, uint32_t, uint32_t, const int32_t*, uint32_t) {
    count(d3d9::Method::lock_texture);
    if (!textures.count(id.value)) return {Status::invalid_argument, {}};
    if (staging.size() < (16u << 20)) staging.assign(16u << 20, std::byte{0});
    return {Status::ok, d3d9::LockedRegion{staging.data(), 64 * 4, 0}};
}

}  // namespace nue
