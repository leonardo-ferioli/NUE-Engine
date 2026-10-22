// Private declaration of the Direct3D 9 layer (see nue/d3d9_layer.hpp for the public face).
#pragma once
#include "layer/d3d9_consts.hpp"
#include "layer/formats.hpp"
#include "nue/d3d9_layer.hpp"
#include "nue/shader/d3d9_shader.hpp"
#include "nue/shader/d3d9_to_spirv.hpp"
#include <array>
#include <bitset>
#include <deque>
#include <string>
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
    // Precomputed for draws: the attributes with their stream renumbered by rank among the used streams (draws bind the used streams to GPU slots 0..3 in
    // that order) and with every stream 0 (user-pointer draws).
    std::vector<gpu::VertexAttribute> remapped, flat0;
    uint8_t stream_order[4] = {}, stream_count = 0;
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
    std::vector<std::vector<std::byte>> contents;   // kept copy per face*levels+level (the whole level, in the game's format): what a Lock reads back; system-memory textures live only here
    uint64_t hash0 = 0;                // hash of the last whole write of level 0 / face 0
    bool zero0 = false, used = false;  // that write was all zero bytes; a draw has sampled this texture
    uint32_t partial_writes0 = 0, uploads = 0, last_upload_frame = 0, updates_after_use = 0, relocks = 0, written_frame = 0xFFFFFFFFu, stale_frame = 0xFFFFFFFFu;
    uint32_t uploaded_levels = 0;      // bit per level of face 0 (any face for cubes) that has received data; render targets count as uploaded
    bool warned_empty = false;
    uint32_t surface_ids[1] = {0};
};

struct SurfaceObject {
    enum class Kind : uint8_t { none, backbuffer, render_target, depth, texture_level } kind = Kind::none;
    uint32_t width = 0, height = 0, format = 0, texture = 0, level = 0, face = 0;
    uint32_t samples = 1;              // render targets and depth surfaces: the sample count of their texture
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
    std::array<Int4, 16> vs_int{}, ps_int{};
    std::array<uint32_t, 16> vs_bool{}, ps_bool{};
    std::array<uint32_t, 4> render_targets{};
    uint32_t depth_stencil = 0;
    uint32_t lights_enabled = 0;
    std::array<LightDesc, 32> lights{};
    std::bitset<32> lights_defined;
    MaterialDesc material;
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
    std::bitset<16> vs_int, ps_int, vs_bool, ps_bool;
    std::bitset<8> texture_transforms;
    bool index_buffer = false, declaration = false, fvf = false, vertex_shader = false, pixel_shader = false, viewport = false, view = false, projection = false, world = false;
    uint32_t lights_mask = 0, lights_value = 0;
    // what the block holds as short lists, built when recording ends: applying a block (the game does it hundreds of times per frame) then touches only these entries instead of
    // testing every bit of every set
    struct Compiled {
        bool ready = false;
        std::vector<std::pair<uint16_t, uint32_t>> rs;
        std::vector<std::tuple<uint8_t, uint8_t, uint32_t>> ss, tss;
        std::vector<std::pair<uint8_t, uint32_t>> textures;
        std::vector<std::pair<uint8_t, StreamBinding>> streams;
        std::vector<std::pair<uint16_t, Float4>> vs_constants, ps_constants;
    } compiled;
};

void finalize_declaration(Declaration&);

class LayerImpl final : public Layer {
public:
    LayerImpl(gpu::Gpu& gpu, const LayerOptions& options);
    ~LayerImpl() override;
    const LayerStats& stats() const override { return stats_; }
    std::string report() const override;
    void flush() override { end_pass(); }
    std::string profile_report() const override;
    std::string audit() const override;
    void request_texture_report() override { capture_request_ = true; }
    std::string take_texture_report() override { std::string r; if (report_ready_) { r.swap(texture_report_); report_ready_ = false; } return r; }
    void set_mark_missing_textures(bool on) override { mark_missing_ = on; }
    bool mark_missing_textures() const override { return mark_missing_; }
    Status update_texture(TextureId source, TextureId destination) override;
    void note_create_failure(const char* what, uint32_t width, uint32_t height, uint32_t levels, uint32_t format);
    SurfaceId auto_depth_stencil() override { return SurfaceId{auto_depth_surface_}; }
    void reset_profile() override { for (auto& p : prof_) p = 0; }
    std::string recent_draws() const override { std::string s; for (const auto& d : recent_) s += d + char(10); return s; }

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
    void set_light(uint32_t index, const LightDesc&) override;
    void set_material(const MaterialDesc&) override;
    bool get_shader_constants_f(bool pixel, uint32_t start, Float4* out, uint32_t count) const override;
    bool get_shader_constants_i(bool pixel, uint32_t start, Int4* out, uint32_t count) const override;
    bool get_shader_constants_b(bool pixel, uint32_t start, uint32_t* out, uint32_t count) const override;
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
    void set_vertex_shader_constants_i(uint32_t start, std::span<const Int4>) override;
    void set_pixel_shader_constants_i(uint32_t start, std::span<const Int4>) override;
    void set_vertex_shader_constants_b(uint32_t start, std::span<const uint32_t>) override;
    void set_pixel_shader_constants_b(uint32_t start, std::span<const uint32_t>) override;
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
    void note_unsupported_literal(const char* what);   // for string literals: counted by address, no string built
    std::vector<std::pair<const char*, uint64_t>> unsupported_literals_;
    void bad_handle(const char* site) { ++stats_.bad_handles; note_unsupported((std::string("invalid handle in ") + site).c_str()); }
    void log(const std::string& message);
    void init_state(DeviceState&);

    // surfaces and passes
    struct Attachment { gpu::Texture color[4]; uint8_t layer[4] = {}, mip[4] = {}; gpu::Texture depth; uint32_t width = 0, height = 0; bool valid = false; bool alpha = true; /* colour 0 has an alpha channel (Direct3D 9: X8R8G8B8 / R5G6B5 do not: it reads as 1) */ };
    void init_opaque_target(gpu::Texture target);   // clears a target without an alpha channel to alpha 1 (it is never written afterwards)
    Attachment resolve_attachments();
    bool same_attachments(const gpu::PassDesc&, const Attachment&) const;
    void end_pass();
    void open_pass(const Attachment&);
    void ensure_pass();
    void recreate_auto_depth(uint32_t width, uint32_t height);
    uint32_t samples_for(uint32_t multisample_type, uint32_t quality) const;
    uint32_t surface_samples(uint32_t multisample_type) const;
    uint32_t backbuffer_samples_ = 1;
    bool resized_for_samples_ = false;

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
    void fill_ff_vertex_constants(std::array<Float4, 128>&) const;
    void fill_ff_pixel_constants(std::array<Float4, 128>&) const;
    const shader::TranslateResult* translate(ShaderObject&, uint32_t shader_id, const shader::TranslateOptions&, uint64_t options_key);
    void fill_vertex_fixup(float* fix, const Attachment&) const;
    void fill_pixel_fixup(float* fix) const;
    void collect_constants(std::vector<std::byte>& out, const Float4* regs, uint32_t available, const shader::TranslateResult&, const float* fix, const Int4* ints = nullptr, const uint32_t* bools = nullptr);

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
    // texture debugging
    struct TextureUse { uint32_t texture, stage_mask, draws, first_draw, first_stage, first_ps; bool stale; };
    std::vector<TextureUse> uses_;
    std::string texture_report_;
    bool mark_missing_ = false, capture_request_ = false, capturing_ = false, report_ready_ = false;
    uint64_t capture_first_draw_ = 0, copy_bytes_ = 0;
    gpu::Texture checker_2d_, checker_cube_;
    gpu::Texture checker_texture(bool cube);
    void note_texture_use(uint32_t id, uint32_t stage, uint32_t pixel_shader, bool stale);
    std::string build_texture_report();
    void upload_level_region(TextureObject& t, uint32_t level, uint32_t face, const uint32_t rect[4], bool whole, const std::byte* data, size_t bytes, uint32_t pitch, uint32_t w, uint32_t h);
    void retain_copy(TextureObject& t, uint32_t index, uint32_t level, const uint32_t rect[4], bool whole, std::vector<std::byte>& staging, uint32_t pitch);

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
    // what the open pass was resolved from: while the bound targets and the surfaces are the same, a draw does not resolve the attachments again
    struct PassKey { std::array<uint32_t, 4> targets{}; uint32_t depth = 0; uint64_t generation = 0; bool operator==(const PassKey&) const = default; };
    PassKey pass_key_;
    uint64_t surface_generation_ = 1;
    // Direct3D 9 lets a depth surface be larger than the colour target (its top-left part is used); bgfx and Direct3D 11 need equal sizes and sample
    // counts. Such a pass gets a depth surface of the target's size and samples, kept per (width, height, format, samples).
    std::map<uint64_t, gpu::Texture> matching_depth_;
    gpu::Texture matching_depth(uint32_t width, uint32_t height, uint32_t format, uint32_t samples);
    // shader constants: a version bumped by every change, and what the last draw collected (a draw with the same program, version and fix-ups reuses it)
    uint64_t vs_version_ = 1, ps_version_ = 1;
    struct ConstantsKey { const void* program = nullptr; uint64_t version = 0; float fix[8] = {}; bool operator==(const ConstantsKey&) const = default; };
    ConstantsKey last_vs_key_, last_ps_key_;   // bumped whenever a surface or texture is created, destroyed or the device is reset

    // programs
    struct TranslationKey { uint64_t shader; uint64_t options; bool operator==(const TranslationKey&) const = default; };
    struct TranslationKeyHash { size_t operator()(const TranslationKey& k) const { return size_t((k.options * 0x9E3779B97F4A7C15ull) ^ (k.shader * 0xC2B2AE3D27D4EB4Full)); } };
    std::unordered_map<TranslationKey, std::unique_ptr<shader::TranslateResult>, TranslationKeyHash> translations_;
    std::unordered_map<uint64_t, gpu::Program> programs_;
    std::map<std::string, uint64_t> unsupported_;

    // per-frame transient dedupe: (buffer, version, offset, bytes) -> transient buffer
    // A fixed-size direct-mapped table (a lost entry only costs a second copy), valid for the frame whose number it carries.
    struct TransientEntry { uint32_t buffer = 0, version = 0, offset = 0, bytes = 0; uint64_t frame = ~0ull; gpu::Buffer gpu; };
    std::vector<TransientEntry> transient_cache_ = std::vector<TransientEntry>(8192);
    std::vector<gpu::Buffer> retired_buffers_;
    std::vector<std::byte> scratch_, vs_constants_scratch_, ps_constants_scratch_, index_scratch_;
    std::vector<gpu::Texture> pending_destroy_textures_;
    std::map<uint32_t, Declaration> fvf_declarations_;
    std::deque<std::string> recent_;
    uint64_t prof_[8] = {};   // ns: total, program, sources, textures, constants, states+gpu draw, (unused), draws profiled
    uint64_t clock_ns() const;
    std::unordered_map<uint64_t, std::unique_ptr<ShaderObject>> ff_shaders_;
    std::array<Float4, 128> ff_constants_{};
    // The program of the last draw, valid while nothing that selects it changed (program_epoch_ is bumped by the setters of such state).
    struct ProgramCache { bool valid = false; uint64_t epoch = 0; const Declaration* decl = nullptr; ShaderObject* vs = nullptr; ShaderObject* ps = nullptr; bool ff_vertex = false, ff_pixel = false; DrawProgram program; };
    ProgramCache program_cache_;
    uint64_t program_epoch_ = 1;
    uint32_t cube_mask_ = 0;      // bit per sampler slot: the bound texture is a cube map
    bool program_uses_ff_ = false;
};

}  // namespace nue::d3d9
