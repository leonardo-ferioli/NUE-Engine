// The Direct3D 9 layer on a nue_gfx context (include/nue_legacy/d3d9_gfx.hpp): the bgfx adapter in external_init mode on the engine's bgfx, frames
// submitted with nue_gfx_present, the window size from nue_gfx's resize notification, Direct3D 9 device loss from nue_gfx's state and generation.
#include "nue_legacy/d3d9_gfx.hpp"
#include "nue/bgfx_gpu.hpp"
#include "nue/d3d9_gfx_provider.hpp"
#include "nue/d3d9_layer.hpp"
#include <atomic>

namespace nue::d3d9 {
namespace {

class GfxDeviceImpl;

// Level A as the facade sees it: the layer, with the three lifecycle calls and the drawing calls guarded by the device state.
class Front final : public Device {
public:
    explicit Front(GfxDeviceImpl& owner) : o_(owner) {}
    Status reset(const DeviceParams& p) override;
    Status test_cooperative_level() override;
    Status present() override;

    Result<TextureId> create_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override { return l().create_texture(w, h, levels, usage, format, pool); }
    Result<TextureId> create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override { return l().create_cube_texture(edge, levels, usage, format, pool); }
    Result<BufferId> create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) override { return l().create_vertex_buffer(bytes, usage, fvf, pool); }
    Result<BufferId> create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) override { return l().create_index_buffer(bytes, usage, format, pool); }
    Result<SurfaceId> create_render_target(uint32_t w, uint32_t h, uint32_t format, uint32_t ms, bool lockable) override { return l().create_render_target(w, h, format, ms, lockable); }
    Result<SurfaceId> create_depth_stencil(uint32_t w, uint32_t h, uint32_t format, uint32_t ms) override { return l().create_depth_stencil(w, h, format, ms); }
    Result<DeclarationId> create_vertex_declaration(std::span<const uint8_t> e) override { return l().create_vertex_declaration(e); }
    Result<ShaderId> create_vertex_shader(std::span<const uint32_t> code) override { return l().create_vertex_shader(code); }
    Result<ShaderId> create_pixel_shader(std::span<const uint32_t> code) override { return l().create_pixel_shader(code); }
    void destroy(TextureId id) override { l().destroy(id); }
    void destroy(BufferId id) override { l().destroy(id); }
    void destroy(SurfaceId id) override { l().destroy(id); }
    void destroy(DeclarationId id) override { l().destroy(id); }
    void destroy(ShaderId id) override { l().destroy(id); }
    void destroy(StateBlockId id) override { l().destroy(id); }

    Result<LockedRegion> lock_buffer(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags) override { return l().lock_buffer(id, offset, bytes, flags); }
    Status unlock_buffer(BufferId id) override { return l().unlock_buffer(id); }
    Result<LockedRegion> lock_texture(TextureId id, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) override { return l().lock_texture(id, level, face, rect4, flags); }
    Status unlock_texture(TextureId id, uint32_t level, uint32_t face) override { return l().unlock_texture(id, level, face); }
    Result<SurfaceId> texture_surface(TextureId id, uint32_t level, uint32_t face) override { return l().texture_surface(id, level, face); }
    Result<SurfaceId> back_buffer(uint32_t swap_chain, uint32_t index) override { return l().back_buffer(swap_chain, index); }
    SurfaceId auto_depth_stencil() override { return l().auto_depth_stencil(); }
    Status stretch_rect(SurfaceId from, const int32_t* fr, SurfaceId to, const int32_t* tr, uint32_t filter) override;
    Status update_texture(TextureId source, TextureId destination) override { return l().update_texture(source, destination); }

    void set_render_state(uint32_t s, uint32_t v) override { l().set_render_state(s, v); }
    void set_sampler_state(uint32_t s, uint32_t t, uint32_t v) override { l().set_sampler_state(s, t, v); }
    void set_texture_stage_state(uint32_t s, uint32_t t, uint32_t v) override { l().set_texture_stage_state(s, t, v); }
    void set_transform(uint32_t type, const float* m) override { l().set_transform(type, m); }
    void light_enable(uint32_t index, bool enable) override { l().light_enable(index, enable); }
    void set_light(uint32_t index, const LightDesc& d) override { l().set_light(index, d); }
    bool get_shader_constants_f(bool pixel, uint32_t start, Float4* out, uint32_t count) const override { return l().get_shader_constants_f(pixel, start, out, count); }
    bool get_shader_constants_i(bool pixel, uint32_t start, Int4* out, uint32_t count) const override { return l().get_shader_constants_i(pixel, start, out, count); }
    bool get_shader_constants_b(bool pixel, uint32_t start, uint32_t* out, uint32_t count) const override { return l().get_shader_constants_b(pixel, start, out, count); }
    void set_material(const MaterialDesc& m) override { l().set_material(m); }
    void set_viewport(const uint32_t* xywh, float min_z, float max_z) override { l().set_viewport(xywh, min_z, max_z); }
    void set_texture(uint32_t sampler, TextureId id) override { l().set_texture(sampler, id); }
    void set_render_target(uint32_t index, SurfaceId id) override { l().set_render_target(index, id); }
    void set_depth_stencil(SurfaceId id) override { l().set_depth_stencil(id); }
    void set_vertex_declaration(DeclarationId id) override { l().set_vertex_declaration(id); }
    void set_fvf(uint32_t fvf) override { l().set_fvf(fvf); }
    void set_vertex_shader(ShaderId id) override { l().set_vertex_shader(id); }
    void set_pixel_shader(ShaderId id) override { l().set_pixel_shader(id); }
    void set_vertex_shader_constants_f(uint32_t start, std::span<const Float4> v) override { l().set_vertex_shader_constants_f(start, v); }
    void set_pixel_shader_constants_f(uint32_t start, std::span<const Float4> v) override { l().set_pixel_shader_constants_f(start, v); }
    void set_vertex_shader_constants_i(uint32_t start, std::span<const Int4> v) override { l().set_vertex_shader_constants_i(start, v); }
    void set_pixel_shader_constants_i(uint32_t start, std::span<const Int4> v) override { l().set_pixel_shader_constants_i(start, v); }
    void set_vertex_shader_constants_b(uint32_t start, std::span<const uint32_t> v) override { l().set_vertex_shader_constants_b(start, v); }
    void set_pixel_shader_constants_b(uint32_t start, std::span<const uint32_t> v) override { l().set_pixel_shader_constants_b(start, v); }
    void set_stream_source(uint32_t stream, BufferId id, uint32_t offset, uint32_t stride) override { l().set_stream_source(stream, id, offset, stride); }
    void set_indices(BufferId id) override { l().set_indices(id); }
    StateBlockId begin_state_block() override { return l().begin_state_block(); }
    void end_state_block(StateBlockId id) override { l().end_state_block(id); }
    void apply(StateBlockId id) override { l().apply(id); }

    void begin_scene() override { l().begin_scene(); }
    void end_scene() override { l().end_scene(); }
    void clear(std::span<const int32_t> rects4, uint32_t flags, uint32_t argb, float z, uint32_t stencil) override;
    void draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) override;
    void draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) override;

private:
    GfxDeviceImpl& o_;
    Layer& l() const;
};

class GfxDeviceImpl final : public GfxDevice {
public:
    GfxDeviceImpl(nue_gfx gfx) : gfx_(gfx), front_(*this) {}
    ~GfxDeviceImpl() override {
        nue_gfx_remove_listener(gfx_, &GfxDeviceImpl::listener, this);
        layer_.reset();   // the layer frees its objects through the adapter, then the adapter frees its own (bgfx stays: it is nue_gfx's)
        gpu_.reset();
    }

    bool init(const LayerOptions& layer_options, std::string& error) {
        nue_gfx_native native;
        NUE_STRUCT_INIT(&native);
        if (nue_gfx_get_native(gfx_, &native) != NUE_OK) { error = "nue_gfx_get_native failed"; return false; }
        if (!native.bgfx_initialized) { error = "the nue_gfx context is not MANAGED (bgfx is not initialised by nue_gfx_create)"; return false; }
        generation_ = native.generation;
        gpu::BgfxOptions options;
        options.external_init = true;
        options.submit_frame = &GfxDeviceImpl::submit_frame;
        options.submit_frame_user = this;
        options.window_width = uint32_t(native.pixel_width > 0 ? native.pixel_width : 1);
        options.window_height = uint32_t(native.pixel_height > 0 ? native.pixel_height : 1);
        options.log = layer_options.log;
        char dir[1024];
        size_t size = 0;
        if (nue_gfx_get_shader_cache_dir(gfx_, dir, sizeof dir, &size) == NUE_OK) options.shader_cache_dir = dir;
        gpu_ = gpu::create_bgfx_gpu(options, error);
        if (!gpu_) return false;
        LayerOptions lo = layer_options;
        if (lo.program_list.empty() && !options.shader_cache_dir.empty()) lo.program_list = options.shader_cache_dir + "/nue_programs.txt";
        layer_ = create_layer(*gpu_, lo);
        if (nue_gfx_add_listener(gfx_, &GfxDeviceImpl::listener, this) != NUE_OK) { error = "nue_gfx_add_listener failed"; return false; }
        return true;
    }

    Device& device() override { return front_; }
    GfxDeviceStats stats() const override { GfxDeviceStats s = stats_; s.generation = generation_; return s; }

    // ---- the lifecycle, on the render thread
    // nue_gfx's state, after letting it apply what other threads recorded (resume, resize) and after following a device it re-created.
    Status state() {
        nue_gfx_apply_pending(gfx_);
        follow_generation();
        switch (nue_gfx_get_state(gfx_)) {
        case NUE_GFX_STATE_OK: return game_must_reset_ ? Status::device_not_reset : Status::ok;
        case NUE_GFX_STATE_NEEDS_RESET: return Status::device_not_reset;
        default: return Status::device_lost;   // SUSPENDED, DEVICE_LOST, ORPHANED
        }
    }
    bool lost() const { return lost_; }
    Status test_cooperative_level() { const Status s = state(); lost_ = s != Status::ok; return s; }
    Status present() {
        const Status s = test_cooperative_level();
        if (s != Status::ok) { ++stats_.lost_presents; return Status::device_lost; }   // Direct3D 9: Present of a lost device fails with D3DERR_DEVICELOST, nothing is drawn
        uint32_t w = 0, h = 0;
        if (window_size_.take(w, h) && w && h) gpu_->set_window_size(w, h);   // external_init: only the letterbox moves; nue_gfx resized the swap chain
        const Status r = layer_->present();
        follow_generation();   // nue_gfx may re-create its device inside nue_gfx_present (a new surface)
        return r;
    }
    Status reset(const DeviceParams& p) {
        nue_gfx_apply_pending(gfx_);
        switch (nue_gfx_get_state(gfx_)) {
        case NUE_GFX_STATE_OK: break;
        case NUE_GFX_STATE_NEEDS_RESET: if (nue_gfx_reset_device(gfx_, NUE_FALSE) < 0) return Status::device_lost; break;
        default: return Status::device_lost;   // suspended: Direct3D 9's Reset fails while the device is lost
        }
        follow_generation();
        game_must_reset_ = false;
        lost_ = false;
        return layer_->reset(p);
    }
    void dropped() { ++stats_.lost_draws; }
    Layer& layer() override { return *layer_; }
    Layer& layer_ref() const { return *layer_; }

private:
    // A new nue_gfx device (generation changed): every GPU object is gone. The adapter and the layer make theirs again now, so that the next frame
    // can draw; the game still sees D3DERR_DEVICENOTRESET until it calls Reset (its default-pool resources are expected to be re-created then).
    void follow_generation() {
        nue_gfx_native native;
        NUE_STRUCT_INIT(&native);
        if (nue_gfx_get_native(gfx_, &native) != NUE_OK || native.generation == generation_ || !native.bgfx_initialized) return;
        generation_ = native.generation;
        ++stats_.device_resets;
        gpu_->device_reset();
        layer_->gpu_objects_lost();
        game_must_reset_ = true;
    }
    static uint32_t submit_frame(void* user) {
        uint32_t frame = 0;
        nue_gfx_present(static_cast<GfxDeviceImpl*>(user)->gfx_, &frame);
        return frame;
    }
    static void NUE_CALL listener(void* user, nue_gfx, nue_gfx_notification what, int32_t a, int32_t b) {   // any thread: record only
        auto* self = static_cast<GfxDeviceImpl*>(user);
        if (what == NUE_GFX_NOTIFY_RESIZED && a > 0 && b > 0) self->window_size_.post(uint32_t(a), uint32_t(b));
    }

    nue_gfx gfx_;
    Front front_;
    std::unique_ptr<gpu::BgfxGpu> gpu_;
    std::unique_ptr<Layer> layer_;
    WindowSizeMailbox window_size_;
    uint32_t generation_ = 0;
    bool game_must_reset_ = false, lost_ = false;
    GfxDeviceStats stats_;
};

Layer& Front::l() const { return o_.layer_ref(); }
Status Front::reset(const DeviceParams& p) { return o_.reset(p); }
Status Front::test_cooperative_level() { return o_.test_cooperative_level(); }
Status Front::present() { return o_.present(); }
Status Front::stretch_rect(SurfaceId from, const int32_t* fr, SurfaceId to, const int32_t* tr, uint32_t filter) {
    if (o_.lost()) { o_.dropped(); return Status::ok; }
    return l().stretch_rect(from, fr, to, tr, filter);
}
void Front::clear(std::span<const int32_t> rects4, uint32_t flags, uint32_t argb, float z, uint32_t stencil) {
    if (o_.lost()) { o_.dropped(); return; }
    l().clear(rects4, flags, argb, z, stencil);
}
void Front::draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) {
    if (o_.lost()) { o_.dropped(); return; }
    l().draw_indexed(primitive, base_vertex, min_vertex, vertex_count, start_index, primitive_count);
}
void Front::draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) {
    if (o_.lost()) { o_.dropped(); return; }
    l().draw_up(primitive, primitive_count, vertices, stride);
}

}  // namespace

std::unique_ptr<GfxDevice> create_gfx_device(nue_gfx gfx, const LayerOptions& options, std::string& error) {
    if (!gfx) { error = "no nue_gfx context"; return nullptr; }
    auto device = std::make_unique<GfxDeviceImpl>(gfx);
    if (!device->init(options, error)) return nullptr;
    return device;
}

}  // namespace nue::d3d9
