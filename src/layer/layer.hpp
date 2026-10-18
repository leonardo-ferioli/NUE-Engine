// Private declaration of the Direct3D 9 layer (see nue/d3d9_layer.hpp for the public face).
#pragma once
#include "layer/d3d9_consts.hpp"
#include "layer/formats.hpp"
#include "nue/d3d9_layer.hpp"
#include "nue/shader/d3d9_shader.hpp"
#include "nue/shader/d3d9_to_spirv.hpp"
#include <array>
#include <bitset>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace nue::d3d9 {

struct DeclElement { uint8_t stream = 0; uint16_t offset = 0; uint8_t type = 0, usage = 0, usage_index = 0; };

struct Declaration {
    bool alive = true;
    std::vector<DeclElement> elements;
    std::vector<gpu::VertexAttribute> attributes;   // the same, in the form the Gpu takes
    uint32_t stream_mask = 0;
    uint32_t bgra_attributes = 0;                   // bgfx attribute bit per D3DCOLOR element
    uint32_t uint_attributes = 0, sint_attributes = 0;   // UBYTE4, SHORT2/SHORT4 elements: the GPU delivers integers
};

struct ShaderObject {
    bool alive = true;
    bool vertex = false, valid = false;
    std::vector<uint32_t> bytecode;
    shader::Program program;
    uint64_t hash = 0;
    std::string error;
};

struct BufferObject {
    bool index = false;
    uint32_t bytes = 0, usage = 0, format_or_fvf = 0, pool = 0;
    std::vector<std::byte> shadow;
    uint32_t version = 1;              // bumps at every unlock that wrote data
    gpu::Buffer gpu;                   // static copy of `version`
    uint32_t gpu_version = 0;
    uint32_t score = 0, score_frame = 0;   // how often the contents change after having been drawn from (dynamic-ness)
    bool used_since_update = false;
    bool locked = false;
    uint32_t lock_offset = 0, lock_bytes = 0, lock_flags = 0;
    bool alive = false;
};

struct TextureObject {
    bool alive = false, cube = false, render_target = false;
    uint32_t width = 0, height = 0, levels = 1, usage = 0, format = 0, pool = 0;
    const FormatInfo* info = nullptr;
    gpu::Format gpu_format = gpu::Format::rgba8;
    gpu::Texture gpu;
    struct Lock { bool active = false; std::vector<std::byte> staging; uint32_t pitch = 0, rect[4] = {}, flags = 0; };
    std::vector<Lock> locks;           // levels * faces
    uint32_t surface_ids[1] = {0};
};

struct SurfaceObject {
    enum class Kind : uint8_t { none, backbuffer, render_target, depth, texture_level } kind = Kind::none;
    uint32_t width = 0, height = 0, format = 0, texture = 0, level = 0, face = 0;
    gpu::Texture gpu;                  // own texture for render targets / depth surfaces
    bool alive = false;
};

struct Viewport { uint32_t x = 0, y = 0, width = 0, height = 0; float min_z = 0.0f, max_z = 1.0f; };
struct StreamBinding { uint32_t buffer = 0, offset = 0, stride = 0; };

constexpr uint32_t sampler_slots = 20;   // 16 pixel + 4 vertex samplers

struct DeviceState {
    std::array<uint32_t, 256> rs{};
    std::array<std::array<uint32_t, k::ss_count>, sampler_slots> ss{};
    std::array<std::array<uint32_t, k::tss_count>, 8> tss{};
    float view[16] = {}, projection[16] = {}, texture[8][16] = {}, world[16] = {};
    Viewport viewport;
    std::array<uint32_t, sampler_slots> textures{};
    std::array<StreamBinding, 16> streams{};
    uint32_t index_buffer = 0, declaration = 0, fvf = 0, vertex_shader = 0, pixel_shader = 0;
    std::array<Float4, 256> vs_constants{};
    std::array<Float4, 224> ps_constants{};
    std::array<uint32_t, 4> render_targets{};
    uint32_t depth_stencil = 0;
    uint32_t lights_enabled = 0;
};

struct StateBlock {
    DeviceState values;
    std::bitset<256> rs;
    std::bitset<sampler_slots * k::ss_count> ss;
    std::bitset<8 * k::tss_count> tss;
    std::bitset<sampler_slots> textures;
    std::bitset<16> streams;
    std::bitset<256> vs_constants;
    std::bitset<224> ps_constants;
    std::bitset<8> texture_transforms;
    bool index_buffer = false, declaration = false, fvf = false, vertex_shader = false, pixel_shader = false, viewport = false, view = false, projection = false, world = false;
    uint32_t lights_mask = 0, lights_value = 0;
};

class LayerImpl final : public Layer {
public:
    LayerImpl(gpu::Gpu& gpu, const LayerOptions& options);
    ~LayerImpl() override;
    const LayerStats& stats() const override { return stats_; }
    std::string report() const override;
    void flush() override { end_pass(); }

    // ---- Device
    Status reset(const DeviceParams&) override;
    Status test_cooperative_level() override { return Status::ok; }
    Status present() override;
    Result<TextureId> create_texture(uint32_t width, uint32_t height, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override;
    Result<TextureId> create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override;
    Result<BufferId> create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) override;
    Result<BufferId> create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) override;
    Result<SurfaceId> create_render_target(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample, bool lockable) override;
    Result<SurfaceId> create_depth_stencil(uint32_t width, uint32_t height, uint32_t format, uint32_t multisample) override;
    Result<DeclarationId> create_vertex_declaration(std::span<const uint8_t> elements) override;
    Result<ShaderId> create_vertex_shader(std::span<const uint32_t> bytecode) override;
    Result<ShaderId> create_pixel_shader(std::span<const uint32_t> bytecode) override;
    void destroy(TextureId) override;
    void destroy(BufferId) override;
    void destroy(SurfaceId) override;
    void destroy(DeclarationId) override;
    void destroy(ShaderId) override;
    void destroy(StateBlockId) override;
    Result<LockedRegion> lock_buffer(BufferId, uint32_t offset, uint32_t bytes, uint32_t flags) override;
    Status unlock_buffer(BufferId) override;
    Result<LockedRegion> lock_texture(TextureId, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) override;
    Status unlock_texture(TextureId, uint32_t level, uint32_t face) override;
    Result<SurfaceId> texture_surface(TextureId, uint32_t level, uint32_t face) override;
    Result<SurfaceId> back_buffer(uint32_t swap_chain, uint32_t index) override;
    Status stretch_rect(SurfaceId from, const int32_t* from_rect4, SurfaceId to, const int32_t* to_rect4, uint32_t filter) override;
    void set_render_state(uint32_t state, uint32_t value) override;
    void set_sampler_state(uint32_t sampler, uint32_t type, uint32_t value) override;
    void set_texture_stage_state(uint32_t stage, uint32_t type, uint32_t value) override;
    void set_transform(uint32_t type, const float* matrix16) override;
    void light_enable(uint32_t index, bool enable) override;
    void set_viewport(const uint32_t* x_y_w_h, float min_z, float max_z) override;
    void set_texture(uint32_t sampler, TextureId) override;
    void set_render_target(uint32_t index, SurfaceId) override;
    void set_depth_stencil(SurfaceId) override;
    void set_vertex_declaration(DeclarationId) override;
    void set_fvf(uint32_t) override;
    void set_vertex_shader(ShaderId) override;
    void set_pixel_shader(ShaderId) override;
    void set_vertex_shader_constants_f(uint32_t start, std::span<const Float4>) override;
    void set_pixel_shader_constants_f(uint32_t start, std::span<const Float4>) override;
    void set_stream_source(uint32_t stream, BufferId, uint32_t offset, uint32_t stride) override;
    void set_indices(BufferId) override;
    StateBlockId begin_state_block() override;
    void end_state_block(StateBlockId) override;
    void apply(StateBlockId) override;
    void begin_scene() override {}
    void end_scene() override {}
    void clear(std::span<const int32_t> rects4, uint32_t flags, uint32_t argb, float z, uint32_t stencil) override;
    void draw_indexed(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count) override;
    void draw_up(uint32_t primitive, uint32_t primitive_count, Bytes vertices, uint32_t stride) override;

private:
    template <class T> struct Table {
        std::vector<T> items = make_items();   // index 0 is "none"
        static std::vector<T> make_items() { std::vector<T> v; v.emplace_back(); return v; }
        std::vector<uint32_t> free_list;
        uint32_t add(T value) {
            if (!free_list.empty()) { const uint32_t i = free_list.back(); free_list.pop_back(); items[i] = std::move(value); return i; }
            items.push_back(std::move(value));
            return uint32_t(items.size() - 1);
        }
        static bool live(const BufferObject& x) { return x.alive; }
        static bool live(const TextureObject& x) { return x.alive; }
        static bool live(const SurfaceObject& x) { return x.alive; }
        static bool live(const Declaration& x) { return x.alive; }
        static bool live(const ShaderObject& x) { return x.alive; }
        static bool live(const std::unique_ptr<StateBlock>& x) { return x != nullptr; }
        T* get(uint32_t id) { return id && id < items.size() && live(items[id]) ? &items[id] : nullptr; }
        void release(uint32_t id) {
            if (!id || id >= items.size() || !live(items[id])) return;
            items[id] = T{};
            if constexpr (!std::is_same_v<T, std::unique_ptr<StateBlock>>) { /* default-constructed objects below are dead */ }
            kill(items[id]);
            free_list.push_back(id);
        }
        static void kill(BufferObject& x) { x.alive = false; }
        static void kill(TextureObject& x) { x.alive = false; }
        static void kill(SurfaceObject& x) { x.alive = false; }
        static void kill(Declaration& x) { x.alive = false; }
        static void kill(ShaderObject& x) { x.alive = false; }
        static void kill(std::unique_ptr<StateBlock>&) {}
    };

    void note_unsupported(const char* what);
    void bad_handle(const char* site) { ++stats_.bad_handles; note_unsupported((std::string("invalid handle in ") + site).c_str()); }
    void log(const std::string& message);
    void init_state(DeviceState&);

    // surfaces and passes
    struct Attachment { gpu::Texture color[4]; uint8_t layer[4] = {}, mip[4] = {}; gpu::Texture depth; uint32_t width = 0, height = 0; bool valid = false; };
    Attachment resolve_attachments();
    bool same_attachments(const gpu::PassDesc&, const Attachment&) const;
    void end_pass();
    void open_pass(const Attachment&);
    void ensure_pass();
    void recreate_auto_depth(uint32_t width, uint32_t height);

    // buffers
    struct VertexSource { gpu::Buffer buffer; uint32_t offset = 0, stride = 0; };
    void touch_buffer(BufferObject&);
    bool buffer_is_dynamic(BufferObject&);
    gpu::Buffer static_gpu_buffer(BufferObject&);
    gpu::Buffer transient_copy(BufferObject& source, uint32_t byte_offset, uint32_t bytes, uint32_t stride, uint32_t buffer_id);

    // shaders and programs
    struct DrawProgram { gpu::Program program; const shader::TranslateResult* vertex = nullptr; const shader::TranslateResult* pixel = nullptr; bool ok = false; };
    DrawProgram select_program(const Declaration* declaration, const DeviceState& st, ShaderObject* vs, ShaderObject* ps, bool vertex_fog_from_fixed_function);
    ShaderObject* fixed_function_vertex(const Declaration&);
    ShaderObject* fixed_function_pixel();
    void fill_ff_vertex_constants(std::array<Float4, 48>&) const;
    void fill_ff_pixel_constants(std::array<Float4, 48>&) const;
    const shader::TranslateResult* translate(ShaderObject&, uint32_t shader_id, const shader::TranslateOptions&, uint64_t options_key);
    void fill_vertex_fixup(float* fix, const Attachment&) const;
    void fill_pixel_fixup(float* fix) const;
    void collect_constants(std::vector<std::byte>& out, const Float4* regs, uint32_t available, const shader::TranslateResult&, const float* fix);

    // drawing
    void do_draw(uint32_t primitive, int32_t base_vertex, uint32_t min_vertex, uint32_t vertex_count, uint32_t start_index, uint32_t primitive_count,
                 const uint8_t* up_vertices, uint32_t up_bytes, uint32_t up_stride);
    void fill_states(gpu::DrawDesc&) const;
    gpu::SamplerDesc sampler_desc(uint32_t slot) const;
    gpu::Texture dummy_texture(bool cube);
    Declaration* declaration_for_draw(Declaration& scratch);

    gpu::Gpu& gpu_;
    LayerOptions options_;
    LayerStats stats_;
    DeviceState state_;
    DeviceParams params_;
    uint32_t frame_ = 0;

    Table<TextureObject> textures_;
    Table<BufferObject> buffers_;
    Table<SurfaceObject> surfaces_;
    Table<Declaration> declarations_;
    Table<ShaderObject> shaders_;
    Table<std::unique_ptr<StateBlock>> blocks_;
    StateBlock* recording_ = nullptr;
    uint32_t recording_id_ = 0;

    uint32_t backbuffer_surface_ = 0, auto_depth_surface_ = 0;
    gpu::Texture dummy_2d_, dummy_cube_;

    // current pass
    bool pass_open_ = false, pass_submitted_ = false;
    gpu::PassDesc pass_;
    Attachment pass_attachments_;

    // programs
    struct TranslationKey { uint64_t shader; uint64_t options; bool operator==(const TranslationKey&) const = default; };
    struct TranslationKeyHash { size_t operator()(const TranslationKey& k) const { return size_t((k.options * 0x9E3779B97F4A7C15ull) ^ (k.shader * 0xC2B2AE3D27D4EB4Full)); } };
    std::unordered_map<TranslationKey, std::unique_ptr<shader::TranslateResult>, TranslationKeyHash> translations_;
    std::unordered_map<uint64_t, gpu::Program> programs_;
    std::map<std::string, uint64_t> unsupported_;

    // per-frame transient dedupe: (buffer, version, offset, bytes) -> transient buffer
    struct TransientKey { uint32_t buffer, version, offset, bytes; bool operator<(const TransientKey& o) const { return std::tie(buffer, version, offset, bytes) < std::tie(o.buffer, o.version, o.offset, o.bytes); } };
    std::map<TransientKey, gpu::Buffer> transient_cache_;
    std::vector<gpu::Buffer> retired_buffers_;
    std::vector<std::byte> scratch_, vs_constants_scratch_, ps_constants_scratch_, index_scratch_;
    std::vector<gpu::Texture> pending_destroy_textures_;
    std::map<uint32_t, Declaration> fvf_declarations_;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderObject>> ff_shaders_;
    std::array<Float4, 48> ff_constants_{};
    std::map<uint32_t, uint32_t> remap_streams_;
    std::vector<gpu::VertexAttribute> layout_scratch_;
};

}  // namespace nue::d3d9
