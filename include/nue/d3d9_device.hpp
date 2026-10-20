// Platform API, graphics level A: the Direct3D 9-semantics device ("what the game says").
//
// Handle-based, no guest pointers, no COM. It is the point where calls are recorded and replayed (docs/PLATFORM_API.md, 3.3).
// Implementations: the Direct3D 9 layer (production), the recorder (wraps another device) and the null device (tests).
// Numeric arguments are Direct3D 9's published constants (D3DRS_*, D3DFMT_*, D3DPT_*, D3DUSAGE_*, D3DPOOL_*, ...) as plain integers.
#pragma once
#include "nue/types.hpp"

namespace nue::d3d9 {

using TextureId = Id<struct TextureTag>;
using BufferId = Id<struct BufferTag>;
using SurfaceId = Id<struct SurfaceTag>;
using DeclarationId = Id<struct DeclarationTag>;
using ShaderId = Id<struct ShaderTag>;
using StateBlockId = Id<struct StateBlockTag>;

struct Float4 { float x, y, z, w; };
// D3DLIGHT9 / D3DMATERIAL9 without the Windows types (type: 1 point, 2 spot, 3 directional).
struct LightDesc { uint32_t type = 3; float diffuse[4] = {1, 1, 1, 0}, specular[4] = {}, ambient[4] = {}, position[3] = {}, direction[3] = {0, 0, 1}, range = 0, falloff = 0, attenuation[3] = {}, theta = 0, phi = 0; };
struct MaterialDesc { float diffuse[4] = {1, 1, 1, 1}, ambient[4] = {1, 1, 1, 1}, specular[4] = {}, emissive[4] = {}, power = 0; };
struct Int4 { int32_t x, y, z, w; };

// Memory the guest writes into between lock and unlock. It lies in the guest arena (the shim converts it to a guest address); the layer
// owns it and reads the written range at unlock. Never GPU memory: a lock cannot wait for the GPU.
struct LockedRegion { std::byte* data = nullptr; uint32_t pitch = 0, slice_pitch = 0; };

struct DeviceParams {
    uint32_t back_width = 0, back_height = 0, back_format = 0, depth_format = 0, multisample = 0, buffers = 1;
    uint32_t multisample_quality = 0xFFFFFFFFu;   // D3DPRESENT_PARAMETERS::MultiSampleQuality; 0xFFFFFFFF = not known (the traces do not record it)
    bool windowed = true;
    uint32_t present_interval = 0;
};

// Lock flags (D3DLOCK_*).
enum LockFlags : uint32_t { lock_read_only = 0x10, lock_no_overwrite = 0x1000, lock_discard = 0x2000, lock_no_dirty_update = 0x8000 };

class Device {   // game thread only
public:
    virtual ~Device() = default;

    // lifecycle
    virtual Status reset(const DeviceParams&) = 0;
    virtual Status test_cooperative_level() = 0;            // ok | device_lost
    virtual Status present() = 0;

    // resources
    virtual Result<TextureId> create_texture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) = 0;
    virtual Result<TextureId> create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) = 0;
    virtual Result<BufferId>  create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) = 0;
    virtual Result<BufferId>  create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) = 0;
    virtual Result<SurfaceId> create_render_target(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, bool lockable) = 0;
    virtual Result<SurfaceId> create_depth_stencil(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample) = 0;
    virtual Result<DeclarationId> create_vertex_declaration(std::span<const uint8_t> elements) = 0;   // D3DVERTEXELEMENT9 array up to D3DDECL_END
    virtual Result<ShaderId>  create_vertex_shader(std::span<const uint32_t> bytecode) = 0;
    virtual Result<ShaderId>  create_pixel_shader(std::span<const uint32_t> bytecode) = 0;
    virtual void destroy(TextureId) = 0;
    virtual void destroy(BufferId) = 0;
    virtual void destroy(SurfaceId) = 0;
    virtual void destroy(DeclarationId) = 0;
    virtual void destroy(ShaderId) = 0;
    virtual void destroy(StateBlockId) = 0;

    // locks: always answered from CPU staging memory; the transfer happens at unlock
    virtual Result<LockedRegion> lock_buffer(BufferId, uint32_t offset, uint32_t bytes, uint32_t flags) = 0;
    virtual Status unlock_buffer(BufferId) = 0;
    virtual Result<LockedRegion> lock_texture(TextureId, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) = 0;
    virtual Status unlock_texture(TextureId, uint32_t level, uint32_t face) = 0;
    virtual Result<SurfaceId> texture_surface(TextureId, uint32_t level, uint32_t face) = 0;
    virtual Result<SurfaceId> back_buffer(uint32_t swap_chain, uint32_t index) = 0;
    virtual SurfaceId auto_depth_stencil() { return {}; }   // the depth surface the device made for the back buffer (none when the game asked for none)
    virtual Status stretch_rect(SurfaceId from, const int32_t* from_rect4, SurfaceId to, const int32_t* to_rect4, uint32_t filter) = 0;
    virtual Status update_texture(TextureId source, TextureId destination) { (void)source; (void)destination; return Status::unsupported; }   // system-memory / managed -> default pool, every level the destination has

    // state: plain stores into the shadow; nothing reaches the GPU until a draw
    virtual void set_render_state(uint32_t state, uint32_t value) = 0;
    virtual void set_sampler_state(uint32_t sampler, uint32_t type, uint32_t value) = 0;
    virtual void set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value) = 0;
    virtual void set_transform(uint32_t type, const float* matrix16) = 0;
    virtual void light_enable(uint32_t index, bool enable) = 0;
    virtual void set_light(uint32_t index, const LightDesc&) { (void)index; }
    virtual void set_material(const MaterialDesc&) {}
    virtual void set_viewport(const uint32_t* x_y_w_h, float min_z, float max_z) = 0;
    virtual void set_texture(uint32_t sampler, TextureId) = 0;
    virtual void set_render_target(uint32_t index, SurfaceId) = 0;
    virtual void set_depth_stencil(SurfaceId) = 0;
    virtual void set_vertex_declaration(DeclarationId) = 0;
    virtual void set_fvf(uint32_t) = 0;
    virtual void set_vertex_shader(ShaderId) = 0;
    virtual void set_pixel_shader(ShaderId) = 0;
    virtual void set_vertex_shader_constants_f(uint32_t start, std::span<const Float4>) = 0;
    virtual void set_pixel_shader_constants_f(uint32_t start, std::span<const Float4>) = 0;
    virtual void set_vertex_shader_constants_i(uint32_t start, std::span<const Int4>) = 0;
    virtual void set_pixel_shader_constants_i(uint32_t start, std::span<const Int4>) = 0;
    virtual void set_vertex_shader_constants_b(uint32_t start, std::span<const uint32_t>) = 0;   // one value per register (0 or 1)
    virtual void set_pixel_shader_constants_b(uint32_t start, std::span<const uint32_t>) = 0;
    virtual void set_stream_source(uint32_t stream, BufferId, uint32_t offset, uint32_t stride) = 0;
    virtual void set_indices(BufferId) = 0;
    virtual StateBlockId begin_state_block() = 0;
    virtual void end_state_block(StateBlockId) = 0;
    virtual void apply(StateBlockId) = 0;

    // scene and draws
    virtual void begin_scene() = 0;
    virtual void end_scene() = 0;
    virtual void clear(std::span<const int32_t> rects4, uint32_t flags, uint32_t argb, float z, uint32_t stencil) = 0;
    virtual void draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) = 0;
    virtual void draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) = 0;
};

// Every Device method, for call counting, tracing and recording.
enum class Method : uint8_t {
    reset, test_cooperative_level, present,
    create_texture, create_cube_texture, create_vertex_buffer, create_index_buffer, create_render_target, create_depth_stencil,
    create_vertex_declaration, create_vertex_shader, create_pixel_shader,
    destroy_texture, destroy_buffer, destroy_surface, destroy_declaration, destroy_shader, destroy_state_block,
    lock_buffer, unlock_buffer, lock_texture, unlock_texture, texture_surface, get_back_buffer, stretch_rect,
    set_render_state, set_sampler_state, set_texture_stage_state, set_transform, set_viewport, set_texture, set_render_target, set_depth_stencil,
    set_vertex_declaration, set_fvf, set_vertex_shader, set_pixel_shader, set_vertex_shader_constants_f, set_pixel_shader_constants_f,
    set_stream_source, set_indices, begin_state_block, end_state_block, apply_state_block,
    begin_scene, end_scene, clear, draw_indexed, draw_up, light_enable,
    set_vertex_shader_constants_i, set_pixel_shader_constants_i, set_vertex_shader_constants_b, set_pixel_shader_constants_b,
    count
};
const char* method_name(Method);

}  // namespace nue::d3d9
