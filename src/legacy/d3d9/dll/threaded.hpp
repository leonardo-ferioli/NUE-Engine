// The worker-thread mode of the dll (nue_d3d9.ini: worker_thread=1), like DXVK's command-stream thread: the game thread only records its Direct3D 9
// calls into a queue, and one worker thread replays them on the real layer and the bgfx adapter (which it also creates, so every bgfx call comes
// from one thread).
//
// What has to be answered at once is answered on the game thread without waiting, or the queue is drained first ("synchronous"):
//  - ids: resources get an id of the queue (a proxy) at once; the worker maps it to the layer's id when it creates the object. A creation the layer
//    refuses later leaves the proxy pointing at nothing (logged); formats are checked on the game thread so that an unknown format still fails at once.
//  - buffer locks: a lock returns memory of the queue's side (a copy of the buffer kept here, or a fresh block); unlock sends the written range to the
//    worker. A lock that must show what the buffer holds (no DISCARD, the buffer already has data, no copy kept yet) drains the queue once and keeps
//    a copy from then on (up to a memory budget).
//  - texture locks: a level that has no data yet, or a DISCARD lock, gets a fresh block here; a level that has data is read once from the worker
//    (after the queue is drained) into a copy kept here, and later locks are served from that copy; unlock sends the locked part. Over the budget, or
//    for automatic mipmaps, the level is locked on the worker and the game writes into the layer's own staging (the worker does not touch a locked level).
//  - Get* of shader constants, Reset, back buffer and depth surfaces, the audit, read-backs, the texture report and the release of the device.
// Back-pressure: Present waits until the worker has finished the previous frame (the game runs at most one frame ahead).
// While the game thread waits it still handles messages sent to its windows: DXGI may send them from the worker and would otherwise wait for ever.
#pragma once
#include "nue/bgfx_gpu.hpp"
#include "nue/d3d9_layer.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nue::facade {

class ThreadedLayer final : public d3d9::Layer {
public:
    explicit ThreadedLayer(uint64_t copy_budget_bytes);
    ~ThreadedLayer() override;
    // the real layer and adapter, made on the worker (inside sync); from then on only the worker touches them
    void attach(d3d9::Layer* layer, gpu::BgfxGpu* gpu) { real_ = layer; gpu_ = gpu; }
    void sync(const std::function<void()>& fn);     // runs fn on the worker once everything queued before it has run, and waits
    void post(std::function<void()> fn);            // runs fn on the worker in order, without waiting
    void stop();                                    // drains the queue and ends the worker
    gpu::BgfxStats gpu_stats() const;               // the adapter's counters as of the last frame the worker finished
    uint64_t worker_busy_us() const { return busy_us_.load(std::memory_order_relaxed); }

    // ---- Layer
    const d3d9::LayerStats& stats() const override;
    void flush() override;
    std::string report() const override;
    std::string audit() const override;
    void request_texture_report() override;
    std::string take_texture_report() override;
    void set_mark_missing_textures(bool on) override;
    bool mark_missing_textures() const override { return mark_missing_; }
    void reset_profile() override;
    std::string profile_report() const override;
    std::string recent_draws() const override;

    // ---- Device
    Status reset(const d3d9::DeviceParams&) override;
    Status test_cooperative_level() override { return Status::ok; }
    Status present() override;
    Result<d3d9::TextureId> create_texture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override;
    Result<d3d9::TextureId> create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override;
    Result<d3d9::BufferId> create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) override;
    Result<d3d9::BufferId> create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) override;
    Result<d3d9::SurfaceId> create_render_target(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, bool lockable) override;
    Result<d3d9::SurfaceId> create_depth_stencil(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample) override;
    Result<d3d9::DeclarationId> create_vertex_declaration(std::span<const uint8_t> elements) override;
    Result<d3d9::ShaderId> create_vertex_shader(std::span<const uint32_t> bytecode) override;
    Result<d3d9::ShaderId> create_pixel_shader(std::span<const uint32_t> bytecode) override;
    void destroy(d3d9::TextureId) override;
    void destroy(d3d9::BufferId) override;
    void destroy(d3d9::SurfaceId) override;
    void destroy(d3d9::DeclarationId) override;
    void destroy(d3d9::ShaderId) override;
    void destroy(d3d9::StateBlockId) override;
    Result<d3d9::LockedRegion> lock_buffer(d3d9::BufferId, uint32_t offset, uint32_t bytes, uint32_t flags) override;
    Status unlock_buffer(d3d9::BufferId) override;
    Result<d3d9::LockedRegion> lock_texture(d3d9::TextureId, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) override;
    Status unlock_texture(d3d9::TextureId, uint32_t level, uint32_t face) override;
    Result<d3d9::SurfaceId> texture_surface(d3d9::TextureId, uint32_t level, uint32_t face) override;
    Result<d3d9::SurfaceId> back_buffer(uint32_t swap_chain, uint32_t index) override;
    d3d9::SurfaceId auto_depth_stencil() override;
    Status stretch_rect(d3d9::SurfaceId from, const int32_t* from_rect4, d3d9::SurfaceId to, const int32_t* to_rect4, uint32_t filter) override;
    Status update_texture(d3d9::TextureId source, d3d9::TextureId destination) override;
    void set_render_state(uint32_t state, uint32_t value) override;
    void set_sampler_state(uint32_t sampler, uint32_t type, uint32_t value) override;
    void set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value) override;
    void set_transform(uint32_t type, const float* matrix16) override;
    void light_enable(uint32_t index, bool enable) override;
    void set_light(uint32_t index, const d3d9::LightDesc&) override;
    void set_material(const d3d9::MaterialDesc&) override;
    bool get_shader_constants_f(bool pixel, uint32_t start, d3d9::Float4* out, uint32_t count) const override;
    bool get_shader_constants_i(bool pixel, uint32_t start, d3d9::Int4* out, uint32_t count) const override;
    bool get_shader_constants_b(bool pixel, uint32_t start, uint32_t* out, uint32_t count) const override;
    void set_viewport(const uint32_t* x_y_w_h, float min_z, float max_z) override;
    void set_texture(uint32_t sampler, d3d9::TextureId) override;
    void set_render_target(uint32_t index, d3d9::SurfaceId) override;
    void set_depth_stencil(d3d9::SurfaceId) override;
    void set_vertex_declaration(d3d9::DeclarationId) override;
    void set_fvf(uint32_t) override;
    void set_vertex_shader(d3d9::ShaderId) override;
    void set_pixel_shader(d3d9::ShaderId) override;
    void set_vertex_shader_constants_f(uint32_t start, std::span<const d3d9::Float4>) override;
    void set_pixel_shader_constants_f(uint32_t start, std::span<const d3d9::Float4>) override;
    void set_vertex_shader_constants_i(uint32_t start, std::span<const d3d9::Int4>) override;
    void set_pixel_shader_constants_i(uint32_t start, std::span<const d3d9::Int4>) override;
    void set_vertex_shader_constants_b(uint32_t start, std::span<const uint32_t>) override;
    void set_pixel_shader_constants_b(uint32_t start, std::span<const uint32_t>) override;
    void set_stream_source(uint32_t stream, d3d9::BufferId, uint32_t offset, uint32_t stride) override;
    void set_indices(d3d9::BufferId) override;
    d3d9::StateBlockId begin_state_block() override;
    void end_state_block(d3d9::StateBlockId) override;
    void apply(d3d9::StateBlockId) override;
    void begin_scene() override {}
    void end_scene() override {}
    void clear(std::span<const int32_t> rects4, uint32_t flags, uint32_t argb, float z, uint32_t stencil) override;
    void draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) override;
    void draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) override;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
    d3d9::Layer* real_ = nullptr;
    gpu::BgfxGpu* gpu_ = nullptr;
    bool mark_missing_ = false;
    std::atomic<uint64_t> busy_us_{0};
    mutable d3d9::LayerStats stats_view_;
    friend struct Impl;
};

}  // namespace nue::facade
