// Null implementations for tests and headless replay: a Gpu that draws nothing (and hashes every call) and a Direct3D 9 device that counts calls.
#pragma once
#include "nue/d3d9_device.hpp"
#include "nue/gpu.hpp"
#include <deque>
#include <map>
#include <vector>

namespace nue {

// A Gpu that draws nothing and keeps a running hash of everything it is asked to do (every call with all its arguments and data, in order):
// two runs that issue the same Level B call sequence end with the same `call_hash`. The hash is how replay determinism is checked, and the
// counters are the cost measurements of a trace (draws, bytes uploaded, transient bytes...).
class NullGpu final : public gpu::Gpu {
public:
    NullGpu();
    const gpu::GpuCaps& caps() override { return capabilities; }
    Result<gpu::Buffer> create_static_buffer(Bytes initial, bool index_buffer) override;
    Result<gpu::Buffer> allocate_transient(uint32_t bytes, uint32_t stride, MutableBytes& out) override;
    Result<gpu::Texture> create_texture(uint32_t width, uint32_t height, uint32_t levels, gpu::Format format, bool render_target, bool cube) override;
    void update_texture(gpu::Texture, uint32_t level, uint32_t face, const uint32_t* rect4, Bytes data, uint32_t row_pitch) override;
    Result<gpu::Program> create_program(Bytes vertex_binary, Bytes pixel_binary) override;
    void begin_pass(const gpu::PassDesc&) override;
    void draw(const gpu::DrawDesc& state) override;
    void end_pass() override { mix(0xE1); ++pass_ends; }
    Status present(uint32_t interval) override;
    Status resize(uint32_t, uint32_t) override { return Status::ok; }
    void read_back(gpu::Texture, uint32_t, MutableBytes out) override { for (auto& b : out) b = std::byte{0}; }
    void destroy(gpu::Buffer) override { mix(0xD1); ++destroyed; }
    void destroy(gpu::Texture) override { mix(0xD2); ++destroyed; }
    void destroy(gpu::Program) override { mix(0xD3); ++destroyed; }

    gpu::GpuCaps capabilities;
    uint32_t next_id = 0;
    uint64_t draws = 0, frames = 0, passes = 0, pass_ends = 0, textures_created = 0, programs_created = 0, texture_updates = 0, texture_bytes = 0, destroyed = 0, buffers_created = 0,
             static_buffer_bytes = 0, transient_bytes = 0, transient_allocations = 0, vertices_drawn = 0, indices_drawn = 0, constant_bytes = 0;
    uint64_t call_hash = 0xcbf29ce484222325ull;
    gpu::Program last_draw_program;
    std::deque<std::vector<std::byte>> transient;
    std::map<uint32_t, size_t> transient_index;   // buffer id -> position in `transient` (this frame)

private:
    void mix(uint64_t value);
    void mix_bytes(const void* data, size_t size);
    template <class T> void mix_value(const T& v) { mix_bytes(&v, sizeof v); }
};

// A Direct3D 9 device that validates handle use, counts every call and keeps lock staging memory. It is the base of the recorder tests.
class NullDevice final : public d3d9::Device {
public:
    Status reset(const d3d9::DeviceParams& p) override { count(d3d9::Method::reset); params = p; return Status::ok; }
    Status test_cooperative_level() override { count(d3d9::Method::test_cooperative_level); return lost ? Status::device_lost : Status::ok; }
    Status present() override { count(d3d9::Method::present); ++frames; return Status::ok; }
    Result<d3d9::TextureId> create_texture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { count(d3d9::Method::create_texture); return make<d3d9::TextureId>(textures); }
    Result<d3d9::TextureId> create_cube_texture(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { count(d3d9::Method::create_cube_texture); return make<d3d9::TextureId>(textures); }
    Result<d3d9::BufferId> create_vertex_buffer(uint32_t bytes, uint32_t, uint32_t, uint32_t) override;
    Result<d3d9::BufferId> create_index_buffer(uint32_t bytes, uint32_t, uint32_t, uint32_t) override;
    Result<d3d9::SurfaceId> create_render_target(uint32_t, uint32_t, uint32_t, uint32_t, bool) override { count(d3d9::Method::create_render_target); return make<d3d9::SurfaceId>(surfaces); }
    Result<d3d9::SurfaceId> create_depth_stencil(uint32_t, uint32_t, uint32_t, uint32_t) override { count(d3d9::Method::create_depth_stencil); return make<d3d9::SurfaceId>(surfaces); }
    Result<d3d9::DeclarationId> create_vertex_declaration(std::span<const uint8_t>) override { count(d3d9::Method::create_vertex_declaration); return make<d3d9::DeclarationId>(declarations); }
    Result<d3d9::ShaderId> create_vertex_shader(std::span<const uint32_t>) override { count(d3d9::Method::create_vertex_shader); return make<d3d9::ShaderId>(shaders); }
    Result<d3d9::ShaderId> create_pixel_shader(std::span<const uint32_t>) override { count(d3d9::Method::create_pixel_shader); return make<d3d9::ShaderId>(shaders); }
    void destroy(d3d9::TextureId id) override { count(d3d9::Method::destroy_texture); textures.erase(id.value); }
    void destroy(d3d9::BufferId id) override { count(d3d9::Method::destroy_buffer); buffers.erase(id.value); }
    void destroy(d3d9::SurfaceId id) override { count(d3d9::Method::destroy_surface); surfaces.erase(id.value); }
    void destroy(d3d9::DeclarationId id) override { count(d3d9::Method::destroy_declaration); declarations.erase(id.value); }
    void destroy(d3d9::ShaderId id) override { count(d3d9::Method::destroy_shader); shaders.erase(id.value); }
    void destroy(d3d9::StateBlockId id) override { count(d3d9::Method::destroy_state_block); state_blocks.erase(id.value); }
    Result<d3d9::LockedRegion> lock_buffer(d3d9::BufferId, uint32_t offset, uint32_t bytes, uint32_t flags) override;
    Status unlock_buffer(d3d9::BufferId id) override { count(d3d9::Method::unlock_buffer); return buffers.count(id.value) ? Status::ok : Status::invalid_argument; }
    Result<d3d9::LockedRegion> lock_texture(d3d9::TextureId, uint32_t, uint32_t, const int32_t*, uint32_t) override;
    Status unlock_texture(d3d9::TextureId id, uint32_t, uint32_t) override { count(d3d9::Method::unlock_texture); return textures.count(id.value) ? Status::ok : Status::invalid_argument; }
    Result<d3d9::SurfaceId> texture_surface(d3d9::TextureId, uint32_t, uint32_t) override { count(d3d9::Method::texture_surface); return make<d3d9::SurfaceId>(surfaces); }
    Result<d3d9::SurfaceId> back_buffer(uint32_t, uint32_t) override { count(d3d9::Method::get_back_buffer); return make<d3d9::SurfaceId>(surfaces); }
    Status stretch_rect(d3d9::SurfaceId, const int32_t*, d3d9::SurfaceId, const int32_t*, uint32_t) override { count(d3d9::Method::stretch_rect); return Status::ok; }
    void set_render_state(uint32_t, uint32_t) override { count(d3d9::Method::set_render_state); }
    void set_sampler_state(uint32_t, uint32_t, uint32_t) override { count(d3d9::Method::set_sampler_state); }
    void set_texture_stage_state(uint32_t, uint32_t, uint32_t) override { count(d3d9::Method::set_texture_stage_state); }
    void set_transform(uint32_t, const float*) override { count(d3d9::Method::set_transform); }
    void light_enable(uint32_t, bool) override { count(d3d9::Method::light_enable); }
    void set_viewport(const uint32_t*, float, float) override { count(d3d9::Method::set_viewport); }
    void set_texture(uint32_t, d3d9::TextureId) override { count(d3d9::Method::set_texture); }
    void set_render_target(uint32_t, d3d9::SurfaceId) override { count(d3d9::Method::set_render_target); }
    void set_depth_stencil(d3d9::SurfaceId) override { count(d3d9::Method::set_depth_stencil); }
    void set_vertex_declaration(d3d9::DeclarationId) override { count(d3d9::Method::set_vertex_declaration); }
    void set_fvf(uint32_t) override { count(d3d9::Method::set_fvf); }
    void set_vertex_shader(d3d9::ShaderId) override { count(d3d9::Method::set_vertex_shader); }
    void set_pixel_shader(d3d9::ShaderId) override { count(d3d9::Method::set_pixel_shader); }
    void set_vertex_shader_constants_f(uint32_t, std::span<const d3d9::Float4>) override { count(d3d9::Method::set_vertex_shader_constants_f); }
    void set_pixel_shader_constants_f(uint32_t, std::span<const d3d9::Float4>) override { count(d3d9::Method::set_pixel_shader_constants_f); }
    void set_stream_source(uint32_t, d3d9::BufferId, uint32_t, uint32_t) override { count(d3d9::Method::set_stream_source); }
    void set_indices(d3d9::BufferId) override { count(d3d9::Method::set_indices); }
    d3d9::StateBlockId begin_state_block() override { count(d3d9::Method::begin_state_block); return make<d3d9::StateBlockId>(state_blocks).value; }
    void end_state_block(d3d9::StateBlockId) override { count(d3d9::Method::end_state_block); }
    void apply(d3d9::StateBlockId) override { count(d3d9::Method::apply_state_block); }
    void begin_scene() override { count(d3d9::Method::begin_scene); }
    void end_scene() override { count(d3d9::Method::end_scene); }
    void clear(std::span<const int32_t>, uint32_t, uint32_t, float, uint32_t) override { count(d3d9::Method::clear); }
    void draw_indexed(uint32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { count(d3d9::Method::draw_indexed); ++draws; }
    void draw_up(uint32_t, uint32_t, Bytes, uint32_t) override { count(d3d9::Method::draw_up); ++draws; }

    uint64_t calls[static_cast<size_t>(d3d9::Method::count)] = {};
    uint64_t draws = 0, frames = 0;
    bool lost = false;
    d3d9::DeviceParams params;
    size_t live_resources() const { return textures.size() + buffers.size() + surfaces.size() + declarations.size() + shaders.size() + state_blocks.size(); }

private:
    void count(d3d9::Method m) { ++calls[static_cast<size_t>(m)]; }
    template <class IdType> Result<IdType> make(std::map<uint32_t, bool>& table) { const uint32_t id = ++next_id; table[id] = true; return {Status::ok, IdType{id}}; }
    std::map<uint32_t, bool> textures, surfaces, declarations, shaders, state_blocks;
    std::map<uint32_t, std::vector<std::byte>> buffers;
    std::vector<std::byte> staging;
    uint32_t next_id = 0;
};

}  // namespace nue
