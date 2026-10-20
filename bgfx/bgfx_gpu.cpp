// gpu::Gpu on bgfx. See nue/bgfx_gpu.hpp.
#include "nue/bgfx_gpu.hpp"
#include "shader_container.hpp"
#include "nue/shader/cross_compile.hpp"
#include "nue/window_fit.hpp"
#include "nue/shader/d3d9_shader.hpp"
#include <algorithm>
#include <bgfx/bgfx.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace nue::gpu {
namespace {

template <class T> struct Slots {
    std::vector<T> items{T{}};
    std::vector<uint32_t> free_list;
    uint32_t add(T v) {
        if (!free_list.empty()) { const uint32_t i = free_list.back(); free_list.pop_back(); items[i] = v; return i; }
        items.push_back(v);
        return uint32_t(items.size() - 1);
    }
    T* get(uint32_t id) { return id && id < items.size() ? &items[id] : nullptr; }
    void release(uint32_t id) { if (id && id < items.size()) { items[id] = T{}; free_list.push_back(id); } }
};

struct BufferEntry {
    bool alive = false, index = false, transient = false;
    bgfx::VertexBufferHandle vb = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ib = BGFX_INVALID_HANDLE;
    bgfx::TransientVertexBuffer tvb{};
    bgfx::TransientIndexBuffer tib{};
    uint32_t stride = 0;
};
struct TextureEntry {
    bool alive = false, render_target = false, depth = false, cube = false;
    bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
    uint32_t width = 0, height = 0, levels = 1, samples = 1;
    Format format = Format::rgba8;
};
struct ProgramEntry { bool alive = false; bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE; };

bgfx::TextureFormat::Enum texture_format(Format f) {
    using TF = bgfx::TextureFormat;
    switch (f) {
    case Format::rgba8: return TF::RGBA8; case Format::bgra8: return TF::BGRA8; case Format::r5g6b5: return TF::R5G6B5; case Format::a4r4g4b4: return TF::BGRA4;
    case Format::a1r5g5b5: return TF::BGR5A1; case Format::r8: return TF::R8; case Format::a8: return TF::A8; case Format::rg8: return TF::RG8;
    case Format::rgba16f: return TF::RGBA16F; case Format::rgba32f: return TF::RGBA32F; case Format::r16f: return TF::R16F; case Format::r32f: return TF::R32F;
    case Format::rg16f: return TF::RG16F; case Format::rg32f: return TF::RG32F; case Format::d16: return TF::D16; case Format::d24s8: return TF::D24S8;
    case Format::d32f: return TF::D32F; case Format::bc1: return TF::BC1; case Format::bc2: return TF::BC2; case Format::bc3: return TF::BC3;
    case Format::rgba8s: return TF::RGBA8S; case Format::astc4x4: return TF::ASTC4x4; case Format::etc2_rgba8: return TF::ETC2A1; default: return TF::RGBA8;
    }
}

uint64_t pack_state(const DrawDesc& d, uint32_t& blend_factor_rgba) {
    using namespace bgfx;
    uint64_t s = 0;
    const auto& b = d.blend;
    if (b.write_mask & 1) s |= BGFX_STATE_WRITE_R;
    if (b.write_mask & 2) s |= BGFX_STATE_WRITE_G;
    if (b.write_mask & 4) s |= BGFX_STATE_WRITE_B;
    if (b.write_mask & 8) s |= BGFX_STATE_WRITE_A;
    auto factor = [](BlendFactor f) -> uint64_t {
        switch (f) {
        case BlendFactor::zero: return BGFX_STATE_BLEND_ZERO; case BlendFactor::one: return BGFX_STATE_BLEND_ONE;
        case BlendFactor::src_color: return BGFX_STATE_BLEND_SRC_COLOR; case BlendFactor::inv_src_color: return BGFX_STATE_BLEND_INV_SRC_COLOR;
        case BlendFactor::src_alpha: return BGFX_STATE_BLEND_SRC_ALPHA; case BlendFactor::inv_src_alpha: return BGFX_STATE_BLEND_INV_SRC_ALPHA;
        case BlendFactor::dst_alpha: return BGFX_STATE_BLEND_DST_ALPHA; case BlendFactor::inv_dst_alpha: return BGFX_STATE_BLEND_INV_DST_ALPHA;
        case BlendFactor::dst_color: return BGFX_STATE_BLEND_DST_COLOR; case BlendFactor::inv_dst_color: return BGFX_STATE_BLEND_INV_DST_COLOR;
        case BlendFactor::src_alpha_sat: return BGFX_STATE_BLEND_SRC_ALPHA_SAT; case BlendFactor::constant: return BGFX_STATE_BLEND_FACTOR;
        case BlendFactor::inv_constant: return BGFX_STATE_BLEND_INV_FACTOR;
        }
        return BGFX_STATE_BLEND_ONE;
    };
    auto equation = [](BlendOp o) -> uint64_t {
        switch (o) {
        case BlendOp::subtract: return BGFX_STATE_BLEND_EQUATION_SUB; case BlendOp::reverse_subtract: return BGFX_STATE_BLEND_EQUATION_REVSUB;
        case BlendOp::min: return BGFX_STATE_BLEND_EQUATION_MIN; case BlendOp::max: return BGFX_STATE_BLEND_EQUATION_MAX; default: return BGFX_STATE_BLEND_EQUATION_ADD;
        }
    };
    if (b.enable) {
        s |= BGFX_STATE_BLEND_FUNC_SEPARATE(factor(b.src), factor(b.dst), factor(b.src_alpha), factor(b.dst_alpha));
        s |= BGFX_STATE_BLEND_EQUATION_SEPARATE(equation(b.op), equation(b.op_alpha));
    }
    blend_factor_rgba = ((b.constant_argb >> 16 & 0xFF) << 24) | ((b.constant_argb >> 8 & 0xFF) << 16) | ((b.constant_argb & 0xFF) << 8) | (b.constant_argb >> 24);
    const auto& ds = d.depth_stencil;
    if (ds.depth_test) {
        switch (ds.depth_func) {
        case Compare::never: s |= BGFX_STATE_DEPTH_TEST_NEVER; break; case Compare::less: s |= BGFX_STATE_DEPTH_TEST_LESS; break;
        case Compare::equal: s |= BGFX_STATE_DEPTH_TEST_EQUAL; break; case Compare::less_equal: s |= BGFX_STATE_DEPTH_TEST_LEQUAL; break;
        case Compare::greater: s |= BGFX_STATE_DEPTH_TEST_GREATER; break; case Compare::not_equal: s |= BGFX_STATE_DEPTH_TEST_NOTEQUAL; break;
        case Compare::greater_equal: s |= BGFX_STATE_DEPTH_TEST_GEQUAL; break; case Compare::always: s |= BGFX_STATE_DEPTH_TEST_ALWAYS; break;
        }
        if (ds.depth_write) s |= BGFX_STATE_WRITE_Z;
    }
    switch (d.raster.cull) { case Cull::cw: s |= BGFX_STATE_CULL_CW; break; case Cull::ccw: s |= BGFX_STATE_CULL_CCW; break; default: break; }
    switch (d.topology) {
    case Topology::triangle_strip: s |= BGFX_STATE_PT_TRISTRIP; break; case Topology::lines: s |= BGFX_STATE_PT_LINES; break;
    case Topology::line_strip: s |= BGFX_STATE_PT_LINESTRIP; break; case Topology::points: s |= BGFX_STATE_PT_POINTS; break; default: break;
    }
    return s;
}

uint32_t pack_stencil(const DepthStencilState& ds) {
    if (!ds.stencil) return BGFX_STENCIL_NONE;
    uint32_t f = BGFX_STENCIL_FUNC_REF(ds.stencil_ref) | BGFX_STENCIL_FUNC_RMASK(ds.stencil_read_mask);
    switch (ds.stencil_func) {
    case Compare::never: f |= BGFX_STENCIL_TEST_NEVER; break; case Compare::less: f |= BGFX_STENCIL_TEST_LESS; break; case Compare::equal: f |= BGFX_STENCIL_TEST_EQUAL; break;
    case Compare::less_equal: f |= BGFX_STENCIL_TEST_LEQUAL; break; case Compare::greater: f |= BGFX_STENCIL_TEST_GREATER; break;
    case Compare::not_equal: f |= BGFX_STENCIL_TEST_NOTEQUAL; break; case Compare::greater_equal: f |= BGFX_STENCIL_TEST_GEQUAL; break; case Compare::always: f |= BGFX_STENCIL_TEST_ALWAYS; break;
    }
    auto op_s = [](StencilOp o) -> uint32_t { switch (o) { case StencilOp::zero: return BGFX_STENCIL_OP_FAIL_S_ZERO; case StencilOp::replace: return BGFX_STENCIL_OP_FAIL_S_REPLACE;
        case StencilOp::incr_sat: return BGFX_STENCIL_OP_FAIL_S_INCRSAT; case StencilOp::decr_sat: return BGFX_STENCIL_OP_FAIL_S_DECRSAT; case StencilOp::invert: return BGFX_STENCIL_OP_FAIL_S_INVERT;
        case StencilOp::incr: return BGFX_STENCIL_OP_FAIL_S_INCR; case StencilOp::decr: return BGFX_STENCIL_OP_FAIL_S_DECR; default: return BGFX_STENCIL_OP_FAIL_S_KEEP; } };
    auto op_z = [](StencilOp o) -> uint32_t { switch (o) { case StencilOp::zero: return BGFX_STENCIL_OP_FAIL_Z_ZERO; case StencilOp::replace: return BGFX_STENCIL_OP_FAIL_Z_REPLACE;
        case StencilOp::incr_sat: return BGFX_STENCIL_OP_FAIL_Z_INCRSAT; case StencilOp::decr_sat: return BGFX_STENCIL_OP_FAIL_Z_DECRSAT; case StencilOp::invert: return BGFX_STENCIL_OP_FAIL_Z_INVERT;
        case StencilOp::incr: return BGFX_STENCIL_OP_FAIL_Z_INCR; case StencilOp::decr: return BGFX_STENCIL_OP_FAIL_Z_DECR; default: return BGFX_STENCIL_OP_FAIL_Z_KEEP; } };
    auto op_p = [](StencilOp o) -> uint32_t { switch (o) { case StencilOp::zero: return BGFX_STENCIL_OP_PASS_Z_ZERO; case StencilOp::replace: return BGFX_STENCIL_OP_PASS_Z_REPLACE;
        case StencilOp::incr_sat: return BGFX_STENCIL_OP_PASS_Z_INCRSAT; case StencilOp::decr_sat: return BGFX_STENCIL_OP_PASS_Z_DECRSAT; case StencilOp::invert: return BGFX_STENCIL_OP_PASS_Z_INVERT;
        case StencilOp::incr: return BGFX_STENCIL_OP_PASS_Z_INCR; case StencilOp::decr: return BGFX_STENCIL_OP_PASS_Z_DECR; default: return BGFX_STENCIL_OP_PASS_Z_KEEP; } };
    return f | op_s(ds.stencil_fail) | op_z(ds.stencil_depth_fail) | op_p(ds.stencil_pass);
}

uint64_t msaa_flag(uint32_t samples) {
    return samples >= 16 ? BGFX_TEXTURE_RT_MSAA_X16 : samples >= 8 ? BGFX_TEXTURE_RT_MSAA_X8 : samples >= 4 ? BGFX_TEXTURE_RT_MSAA_X4 : BGFX_TEXTURE_RT_MSAA_X2;
}

uint32_t sampler_flags(const SamplerDesc& s) {
    uint32_t f = 0;
    auto addr = [](uint8_t a, uint32_t mirror, uint32_t clamp, uint32_t border) -> uint32_t { return a == 1 || a == 4 ? mirror : a == 2 ? clamp : a == 3 ? border : 0; };
    f |= addr(s.address_u, BGFX_SAMPLER_U_MIRROR, BGFX_SAMPLER_U_CLAMP, BGFX_SAMPLER_U_BORDER);
    f |= addr(s.address_v, BGFX_SAMPLER_V_MIRROR, BGFX_SAMPLER_V_CLAMP, BGFX_SAMPLER_V_BORDER);
    f |= addr(s.address_w, BGFX_SAMPLER_W_MIRROR, BGFX_SAMPLER_W_CLAMP, BGFX_SAMPLER_W_BORDER);
    static const bool no_aniso = std::getenv("NUE_NO_ANISO") != nullptr;   // experiment: treat anisotropic as linear
    if (s.min == 0) f |= BGFX_SAMPLER_MIN_POINT; else if (s.min == 2 && !no_aniso) f |= BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC;   // anisotropic filtering needs both
    if (s.mag == 0 && s.min != 2) f |= BGFX_SAMPLER_MAG_POINT;
    if (s.mip == 0) f |= BGFX_SAMPLER_MIP_POINT;
    return f;
}

struct AttributeFormat { bgfx::AttribType::Enum type; uint8_t num; bool normalized, as_int; uint8_t bytes; };
bool attribute_format(uint8_t d3d_type, AttributeFormat& f) {
    using T = bgfx::AttribType;
    switch (d3d_type) {
    case 0: f = {T::Float, 1, false, false, 4}; return true;
    case 1: f = {T::Float, 2, false, false, 8}; return true;
    case 2: f = {T::Float, 3, false, false, 12}; return true;
    case 3: f = {T::Float, 4, false, false, 16}; return true;
    case 4: f = {T::Uint8, 4, true, false, 4}; return true;     // D3DCOLOR (the shader swaps B and R)
    case 5: f = {T::Uint8, 4, false, true, 4}; return true;     // UBYTE4: an integer attribute, the shader converts it to float
    case 6: f = {T::Int16, 2, false, true, 4}; return true;
    case 7: f = {T::Int16, 4, false, true, 8}; return true;
    case 8: f = {T::Uint8, 4, true, false, 4}; return true;
    case 9: f = {T::Int16, 2, true, false, 4}; return true;
    case 10: f = {T::Int16, 4, true, false, 8}; return true;
    case 11: f = {T::Int16, 2, true, false, 4}; return true;    // USHORT2N approximated
    case 12: f = {T::Int16, 4, true, false, 8}; return true;
    case 13: f = {T::Uint10, 4, false, false, 4}; return true;
    case 14: f = {T::Uint10, 4, true, false, 4}; return true;
    case 15: f = {T::Half, 2, false, false, 4}; return true;
    case 16: f = {T::Half, 4, false, false, 8}; return true;
    default: return false;
    }
}

constexpr uint32_t kMaxTransient = 0x7fffffff;

inline uint64_t now_ns() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }

class BgfxGpuImpl final : public BgfxGpu {
public:
    ~BgfxGpuImpl() override;
    bool init(const BgfxOptions& options, std::string& error);

    const GpuCaps& caps() override { return caps_; }
    const BgfxStats& stats() const override { return stats_; }
    Result<Buffer> create_static_buffer(Bytes initial, BufferKind kind) override;
    Result<Buffer> allocate_transient(uint32_t bytes, uint32_t stride, bool index, MutableBytes& out) override;
    Result<Texture> create_texture(uint32_t width, uint32_t height, uint32_t levels, Format, bool render_target, bool cube, uint32_t samples = 1) override;
    void set_backbuffer_samples(uint32_t samples) override { backbuffer_samples_ = std::max(samples, 1u); }
    void set_visible_area(int32_t x, int32_t y, uint32_t width, uint32_t height) override { visible_[0] = x; visible_[1] = y; visible_[2] = int32_t(width); visible_[3] = int32_t(height); visible_set_ = width && height; }
    void update_texture(Texture, uint32_t level, uint32_t face, const uint32_t* rect4, Bytes data, uint32_t row_pitch) override;
    Result<Program> create_program(const ProgramDesc&) override;
    void copy_texture(Texture dst, const int32_t* dst_rect4, Texture src, const int32_t* src_rect4, bool linear) override;
    void begin_pass(const PassDesc&) override;
    void draw(const DrawDesc&) override;
    void end_pass() override;
    Status present(uint32_t swap_interval) override;
    Status resize(uint32_t width, uint32_t height) override;
    Status read_back(Texture, uint32_t& width, uint32_t& height, std::vector<std::byte>& bgra) override;
    void destroy(Buffer) override;
    void destroy(Texture) override;
    void destroy(Program) override;
    void set_window_size(uint32_t width, uint32_t height) override;
    std::string profile_report() const override;
    std::string audit() const override;
    void fail(const char* what);
    void reset_profile() override { prof_draw_ns_ = prof_draws_ = prof_layout_ns_ = prof_uniform_ns_ = prof_texture_ns_ = prof_submit_ns_ = prof_frame_ns_ = prof_frames_ = 0; prof_gpu_ms_ = prof_render_cpu_ms_ = prof_wait_render_ms_ = prof_wait_submit_ms_ = 0; }

private:
    bgfx::VertexLayoutHandle layout_handle(const DrawDesc& d, uint32_t stream);
    bgfx::FrameBufferHandle framebuffer_for(const bgfx::TextureHandle& color, const bgfx::TextureHandle& depth, uint32_t layer = 0, uint32_t mip = 0);
    TextureEntry* texture_entry(Texture t) { return t == backbuffer ? &backbuffer_ : textures_.get(t.value); }
    bool build_blit_program();
    void draw_quad(uint32_t view, bgfx::TextureHandle source, uint32_t sampler, const float uv[4]);   // uv: u0, v0, u1, v1 of the source mapped to the view
    ShaderTarget shader_target() const { return renderer_ == bgfx::RendererType::Vulkan ? ShaderTarget::spirv : renderer_ == bgfx::RendererType::Direct3D11 || renderer_ == bgfx::RendererType::Direct3D12 ? ShaderTarget::dxbc : ShaderTarget::glsl330; }

    BgfxOptions options_;
    GpuCaps caps_;
    BgfxStats stats_;
    bgfx::RendererType::Enum renderer_ = bgfx::RendererType::Noop;
    Slots<BufferEntry> buffers_;
    Slots<TextureEntry> textures_;
    Slots<ProgramEntry> programs_;
    TextureEntry backbuffer_;
    std::map<uint64_t, bgfx::FrameBufferHandle> framebuffers_;
    std::unordered_map<uint64_t, bgfx::VertexLayoutHandle> layouts_;
    std::vector<bgfx::VertexLayout> layout_storage_;
    bgfx::VertexLayoutHandle dummy_vertex_layout_ = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_vs_ = BGFX_INVALID_HANDLE, u_ps_ = BGFX_INVALID_HANDLE, u_tex_[16];
    uint32_t window_width_ = 0, window_height_ = 0;
    uint32_t next_view_ = 0, current_view_ = 0, pass_draws_ = 0;
    bool pass_open_ = false;
    uint32_t pass_rect_[4] = {};
    uint32_t frame_ = 0;
    uint64_t prof_draw_ns_ = 0, prof_draws_ = 0, prof_layout_ns_ = 0, prof_uniform_ns_ = 0, prof_texture_ns_ = 0, prof_submit_ns_ = 0, prof_frame_ns_ = 0, prof_frames_ = 0;
    double prof_gpu_ms_ = 0, prof_render_cpu_ms_ = 0, prof_wait_render_ms_ = 0, prof_wait_submit_ms_ = 0;
    bool initialised_ = false;
    uint32_t backbuffer_samples_ = 1;
    int32_t visible_[4] = {};
    bool visible_set_ = false;
    bgfx::ProgramHandle blit_program_ = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readback_ = BGFX_INVALID_HANDLE;
    uint32_t readback_w_ = 0, readback_h_ = 0;
    std::vector<std::byte> const_scratch_;
    shader::TranslateResult blit_vs_, blit_ps_;
};

void BgfxGpuImpl::fail(const char* what) {
    ++stats_.failed_creates;
    if ((stats_.failed_creates <= 20 || stats_.failed_creates % 200 == 0) && options_.log) {
        char text[300];
        std::snprintf(text, sizeof text, "bgfx could not create: %s (failure %llu; %s)", what, (unsigned long long)stats_.failed_creates, audit().c_str());
        options_.log(text);
    }
}

std::string BgfxGpuImpl::audit() const {
    const bgfx::Stats* st = bgfx::getStats();
    const bgfx::Caps* caps = bgfx::getCaps();
    char text[700];
    if (!st || !caps) return "bgfx not running";
    std::snprintf(text, sizeof text,
                  "bgfx handles in use / limit: textures %u/%u, vertex buffers %u/%u, index buffers %u/%u, frame buffers %u/%u, vertex layouts %zu/%u, programs %u/%u, shaders %u/%u, uniforms %u/%u; "
                  "texture memory %.0f MB, render target memory %.0f MB; failed creations %llu; window back buffer %ux%u",
                  unsigned(st->numTextures), caps->limits.maxTextures, unsigned(st->numVertexBuffers), caps->limits.maxVertexBuffers, unsigned(st->numIndexBuffers), caps->limits.maxIndexBuffers,
                  unsigned(st->numFrameBuffers), caps->limits.maxFrameBuffers, layouts_.size(), caps->limits.maxVertexLayouts, unsigned(st->numPrograms), caps->limits.maxPrograms,
                  unsigned(st->numShaders), caps->limits.maxShaders, unsigned(st->numUniforms), caps->limits.maxUniforms, double(st->textureMemoryUsed) / 1048576.0, double(st->rtMemoryUsed) / 1048576.0,
                  (unsigned long long)stats_.failed_creates, unsigned(st->width), unsigned(st->height));
    return text;
}

std::string BgfxGpuImpl::profile_report() const {
    char text[512];
    const double n = double(std::max<uint64_t>(prof_draws_, 1)), f = double(std::max<uint64_t>(prof_frames_, 1));
    std::snprintf(text, sizeof text, "adapter per draw (ns): %.0f of which bgfx::submit %.0f; bgfx::frame() blocks %.2f ms per frame (%llu draws, %llu frames)\n", double(prof_draw_ns_) / n, double(prof_submit_ns_) / n, double(prof_frame_ns_) / f / 1e6, (unsigned long long)prof_draws_, (unsigned long long)prof_frames_);
    char more[256];
    std::snprintf(more, sizeof more, "bgfx per frame: GPU %.2f ms, render-thread CPU %.2f ms, API thread waits for the render thread %.2f ms, render thread waits for the API thread %.2f ms\n", prof_gpu_ms_ / f, prof_render_cpu_ms_ / f, prof_wait_render_ms_ / f, prof_wait_submit_ms_ / f);
    return std::string(text) + more;
}

BgfxGpuImpl::~BgfxGpuImpl() {
    if (!initialised_) return;
    bgfx::frame();
    bgfx::shutdown();
}

namespace {
// Routes bgfx's fatal errors and backend messages to the host's log (a fatal error would otherwise only reach the debugger and then abort silently).
struct LogCallback final : bgfx::CallbackI {
    void (*log)(const char*) = nullptr;
    void fatal(const char* file, uint16_t line, bgfx::Fatal::Enum code, const char* text) override {
        char buffer[1024];
        std::snprintf(buffer, sizeof buffer, "bgfx FATAL %d at %s:%u: %s", int(code), file ? file : "?", unsigned(line), text ? text : "");
        if (log) log(buffer);
        if (code != bgfx::Fatal::DebugCheck) std::abort();
    }
    void traceVargs(const char* file, uint16_t line, const char* format, va_list args) override {
        char buffer[1024];
        const int n = std::snprintf(buffer, sizeof buffer, "bgfx %s:%u: ", file ? file : "?", unsigned(line));
        if (n > 0 && size_t(n) < sizeof buffer) std::vsnprintf(buffer + n, sizeof buffer - size_t(n), format, args);
        for (char* p = buffer; *p; ++p) if (*p == 10 || *p == 13) *p = ' ';
        if (log) log(buffer);
    }
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t) override { return 0; }
    bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
    void cacheWrite(uint64_t, const void*, uint32_t) override {}
    void screenShot(const char*, uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, const void*, uint32_t, bool) override {}
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
};
LogCallback g_log_callback;
}  // namespace

bool BgfxGpuImpl::init(const BgfxOptions& options, std::string& error) {
    options_ = options;
    if (options.single_threaded) bgfx::renderFrame();
    bgfx::Init init;
    switch (options.renderer) {
    case Renderer::d3d11: init.type = bgfx::RendererType::Direct3D11; break;
    case Renderer::d3d12: init.type = bgfx::RendererType::Direct3D12; break;
    case Renderer::vulkan: init.type = bgfx::RendererType::Vulkan; break;
    case Renderer::opengl: init.type = bgfx::RendererType::OpenGL; break;
    case Renderer::metal: init.type = bgfx::RendererType::Metal; break;
    case Renderer::noop: init.type = bgfx::RendererType::Noop; break;
    default: init.type = bgfx::RendererType::Count; break;
    }
    init.swapChain.nwh = options.window;
    init.swapChain.width = options.window_width;
    init.swapChain.height = options.window_height;
    init.reset = (options.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE) | BGFX_RESET_MAXANISOTROPY;
    init.limits.maxTransientVbSize = options.transient_vertex_mb << 20;
    init.limits.maxTransientIbSize = options.transient_index_mb << 20;
    init.limits.numDrawCalls = 1 << 15;
    if (options.log) { g_log_callback.log = options.log; init.callback = &g_log_callback; }
    if (!bgfx::init(init)) { error = "bgfx::init failed"; return false; }
    initialised_ = true;
    renderer_ = bgfx::getRendererType();
    stats_.renderer_name = bgfx::getRendererName(renderer_);
    window_width_ = options.window_width; window_height_ = options.window_height;

    const bgfx::Caps* caps = bgfx::getCaps();
    caps_.name = stats_.renderer_name;
    caps_.max_texture_size = caps->limits.maxTextureSize;
    caps_.max_anisotropy = 16;
    caps_.origin_bottom_left = caps->originBottomLeft;
    caps_.homogeneous_depth = caps->homogeneousDepth;
    caps_.mrt = caps->limits.maxFBAttachments > 1;
    caps_.max_samples = (caps->formats[bgfx::TextureFormat::BGRA8] & BGFX_CAPS_FORMAT_TEXTURE_MSAA) ? 8 : 1;   // bgfx falls back to what the device supports
    for (size_t i = 0; i < size_t(Format::count); ++i) {
        const auto f = texture_format(Format(i));
        caps_.texture_format[i] = (caps->formats[f] & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0;
        if (std::getenv("NUE_LOG_CAPS")) std::fprintf(stderr, "format %zu (bgfx %d): flags %x -> %d%c", i, int(f), unsigned(caps->formats[f]), int(caps_.texture_format[i]), 10);
        if (std::getenv("NUE_NO_BC") && (Format(i) == Format::bc1 || Format(i) == Format::bc2 || Format(i) == Format::bc3)) caps_.texture_format[i] = false;   // debugging aid: force the decode to RGBA8
    }
    bgfx::VertexLayout dummy;
    dummy.begin().add(bgfx::Attrib::Position, 4, bgfx::AttribType::Uint8, true).end();
    dummy_vertex_layout_ = bgfx::createVertexLayout(dummy);
    u_vs_ = bgfx::createUniform("u_vs", bgfx::UniformType::Vec4, 2 + 256);
    u_ps_ = bgfx::createUniform("u_ps", bgfx::UniformType::Vec4, 2 + 224);
    for (int i = 0; i < 16; ++i) { char name[16]; std::snprintf(name, sizeof name, "s_tex%d", i); u_tex_[i] = bgfx::createUniform(name, bgfx::UniformType::Sampler, 1); }
    bgfx::setViewMode(0, bgfx::ViewMode::Sequential);
    return build_blit_program();
}

bool BgfxGpuImpl::build_blit_program() {
    using namespace shader;
    shader::Program vs, ps;
    vs.stage = Stage::vertex; vs.major = 1; vs.minor = 1;
    ps.stage = Stage::pixel; ps.major = 1; ps.minor = 1;
    auto mov = [](RegType dt, uint16_t di, RegType st, uint16_t si) {
        Instruction i; i.opcode = op_mov; i.has_dst = true; i.dst.type = dt; i.dst.index = di; i.src[0].type = st; i.src[0].index = si; i.src_count = 1; return i;
    };
    vs.code.push_back(mov(RegType::rast_out, 0, RegType::input, 0));
    vs.code.push_back(mov(RegType::texcoord_out, 0, RegType::input, 7));
    Instruction tex; tex.opcode = op_tex; tex.has_dst = true; tex.dst.type = RegType::addr_texture; tex.dst.index = 0;
    ps.code.push_back(tex);
    ps.code.push_back(mov(RegType::temp, 0, RegType::addr_texture, 0));
    TranslateOptions options;
    options.clamp_ps1_constants = false;
    if (!translate_to_spirv(vs, options, blit_vs_) || !translate_to_spirv(ps, options, blit_ps_)) return false;
    gpu::ProgramDesc desc{0xB11700, &blit_vs_, &blit_ps_};
    auto r = create_program(desc);
    if (!r) return false;
    blit_program_ = programs_.get(r.value.value)->handle;
    return true;
}

// ---- resources ------------------------------------------------------------------------------------------------------------------------------

Result<Buffer> BgfxGpuImpl::create_static_buffer(Bytes initial, BufferKind kind) {
    BufferEntry e;
    e.alive = true;
    const bgfx::Memory* mem = bgfx::copy(initial.data(), uint32_t(initial.size()));
    if (kind == BufferKind::vertex) {
        const bgfx::Memory* padded = mem;
        bgfx::VertexLayout layout;
        layout.begin().add(bgfx::Attrib::Position, 4, bgfx::AttribType::Uint8, true).end();
        e.vb = bgfx::createVertexBuffer(padded, layout);
        e.stride = 4;
        if (!bgfx::isValid(e.vb)) { fail("static vertex buffer"); return {Status::out_of_memory, {}}; }
    } else {
        e.index = true;
        e.ib = bgfx::createIndexBuffer(mem, kind == BufferKind::index32 ? BGFX_BUFFER_INDEX32 : BGFX_BUFFER_NONE);
        if (!bgfx::isValid(e.ib)) { fail("static index buffer"); return {Status::out_of_memory, {}}; }
    }
    return {Status::ok, Buffer{buffers_.add(e)}};
}

Result<Buffer> BgfxGpuImpl::allocate_transient(uint32_t bytes, uint32_t stride, bool index, MutableBytes& out) {
    BufferEntry e;
    e.alive = true; e.transient = true; e.index = index; e.stride = stride;
    if (index) {
        const bool wide = stride == 4;
        const uint32_t count = bytes / stride;
        if (bgfx::getAvailTransientIndexBuffer(count, wide) < count) { fail("transient index buffer exhausted (draws are skipped)"); return {Status::out_of_memory, {}}; }
        bgfx::allocTransientIndexBuffer(&e.tib, count, wide);
        out = MutableBytes(reinterpret_cast<std::byte*>(e.tib.data), size_t(count) * stride);
    } else {
        const uint32_t count = std::max(1u, bytes / std::max(stride, 1u));
        bgfx::VertexLayout layout;
        layout.begin().add(bgfx::Attrib::Position, 4, bgfx::AttribType::Uint8, true).end();
        layout.m_stride = uint16_t(stride);
        if (bgfx::getAvailTransientVertexBuffer(count, layout) < count) { fail("transient vertex buffer exhausted (draws are skipped)"); return {Status::out_of_memory, {}}; }
        bgfx::allocTransientVertexBuffer(&e.tvb, count, layout);
        out = MutableBytes(reinterpret_cast<std::byte*>(e.tvb.data), size_t(count) * stride);
    }
    return {Status::ok, Buffer{buffers_.add(e)}};
}

void BgfxGpuImpl::destroy(Buffer b) {
    BufferEntry* e = buffers_.get(b.value);
    if (!e) return;
    if (!e->transient) { if (bgfx::isValid(e->vb)) bgfx::destroy(e->vb); if (bgfx::isValid(e->ib)) bgfx::destroy(e->ib); }
    buffers_.release(b.value);
}

Result<Texture> BgfxGpuImpl::create_texture(uint32_t width, uint32_t height, uint32_t levels, Format format, bool render_target, bool cube, uint32_t samples) {
    TextureEntry t;
    t.alive = true; t.render_target = render_target; t.cube = cube; t.width = width; t.height = height; t.levels = std::max(levels, 1u); t.format = format;
    t.depth = format == Format::d16 || format == Format::d24s8 || format == Format::d32f;
    uint64_t flags = BGFX_SAMPLER_NONE;
    if (render_target) {
        // the multisample flags already contain the render-target bit and must not be OR-ed with BGFX_TEXTURE_RT (x8 | RT would read as x16)
        const bool ms = samples > 1 && !cube && caps_.max_samples > 1;
        flags |= (ms ? msaa_flag(std::min(samples, caps_.max_samples)) : BGFX_TEXTURE_RT) | (t.depth ? BGFX_TEXTURE_RT_WRITE_ONLY : BGFX_TEXTURE_BLIT_DST);
        t.samples = ms ? std::min(samples, caps_.max_samples) : 1;
    }
    if (t.levels > 1) flags |= BGFX_TEXTURE_MIP_COUNT(t.levels);
    const auto f = texture_format(format);
    t.handle = cube ? bgfx::createTextureCube(uint16_t(width), t.levels > 1, 1, f, flags)
                    : bgfx::createTexture2D(uint16_t(width), uint16_t(height), t.levels > 1, 1, f, flags);
    if (!bgfx::isValid(t.handle) && t.depth && !cube) {   // some Vulkan drivers have no 24-bit depth: fall back to 32-bit float depth, then 16-bit (stencil is lost)
        for (const bgfx::TextureFormat::Enum fallback : {bgfx::TextureFormat::D32F, bgfx::TextureFormat::D16}) {
            t.handle = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, fallback, flags);
            if (bgfx::isValid(t.handle)) break;
        }
    }
    if (!bgfx::isValid(t.handle)) { char what[160]; std::snprintf(what, sizeof what, "texture %ux%u levels %u format %d rt %d cube %d samples %u", width, height, t.levels, int(format), int(render_target), int(cube), samples); fail(what); return {Status::out_of_memory, {}}; }
    return {Status::ok, Texture{textures_.add(t)}};
}

void BgfxGpuImpl::update_texture(Texture id, uint32_t level, uint32_t face, const uint32_t* rect4, Bytes data, uint32_t row_pitch) {
    TextureEntry* t = textures_.get(id.value);
    if (!t || !bgfx::isValid(t->handle)) return;
    const uint32_t lw = std::max(1u, t->width >> level), lh = std::max(1u, t->height >> level);
    const uint32_t x = rect4 ? rect4[0] : 0, y = rect4 ? rect4[1] : 0, w = rect4 ? rect4[2] : lw, h = rect4 ? rect4[3] : lh;
    const bgfx::Memory* mem = bgfx::copy(data.data(), uint32_t(data.size()));
    (void)row_pitch;
    if (t->cube) bgfx::updateTextureCube(t->handle, 0, uint8_t(face), uint8_t(level), uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h), mem);
    else bgfx::updateTexture2D(t->handle, 0, uint8_t(level), uint16_t(x), uint16_t(y), uint16_t(w), uint16_t(h), mem);
}

void BgfxGpuImpl::destroy(Texture id) {
    TextureEntry* t = textures_.get(id.value);
    if (!t) return;
    for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
        const uint64_t key = it->first;
        if (uint16_t(key & 0xFFFF) == t->handle.idx || uint16_t(key >> 16) == t->handle.idx) { bgfx::destroy(it->second); it = framebuffers_.erase(it); } else ++it;
    }
    if (bgfx::isValid(t->handle)) bgfx::destroy(t->handle);
    textures_.release(id.value);
}

Result<Program> BgfxGpuImpl::create_program(const ProgramDesc& desc) {
    if (!desc.vertex || !desc.pixel) return {Status::invalid_argument, {}};
    ContainerResult vs, ps;
    const ShaderTarget target = shader_target();
    if (const char* dump = std::getenv("NUE_DUMP_GLSL")) {   // debugging aid: the translated shaders as readable GLSL
        for (int k = 0; k < 2; ++k) {
            const shader::TranslateResult& t = k ? *desc.pixel : *desc.vertex;
            std::string text, error;
            shader::CrossOptions co;
            if (shader::cross_compile(t.spirv, t.stage, shader::Target::glsl330, co, text, error)) {
                const std::string path = std::string(dump) + "/prog" + std::to_string(desc.key) + (k ? "_ps.glsl" : "_vs.glsl");
                if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(text.data(), 1, text.size(), f); std::fclose(f); }
            }
        }
    }
    if (!build_container(*desc.vertex, target, vs, options_.shader_cache_dir)) { std::fprintf(stderr, "[bgfx] vertex shader failed: %s\n", vs.error.c_str()); return {Status::unsupported, {}}; }
    if (!build_container(*desc.pixel, target, ps, options_.shader_cache_dir)) { std::fprintf(stderr, "[bgfx] pixel shader failed: %s\n", ps.error.c_str()); return {Status::unsupported, {}}; }
    stats_.cross_ms += vs.cross_ms + ps.cross_ms; stats_.compile_ms += vs.compile_ms + ps.compile_ms;
    stats_.shader_builds += 2;
    const bgfx::ShaderHandle vsh = bgfx::createShader(bgfx::copy(vs.bytes.data(), uint32_t(vs.bytes.size())));
    const bgfx::ShaderHandle fsh = bgfx::createShader(bgfx::copy(ps.bytes.data(), uint32_t(ps.bytes.size())));
    if (!bgfx::isValid(vsh) || !bgfx::isValid(fsh)) return {Status::unsupported, {}};
    ProgramEntry e;
    e.alive = true;
    e.handle = bgfx::createProgram(vsh, fsh, true);
    if (!bgfx::isValid(e.handle)) return {Status::unsupported, {}};
    ++stats_.programs;
    return {Status::ok, Program{programs_.add(e)}};
}

void BgfxGpuImpl::destroy(Program p) {
    ProgramEntry* e = programs_.get(p.value);
    if (!e) return;
    if (bgfx::isValid(e->handle)) bgfx::destroy(e->handle);
    programs_.release(p.value);
}

// ---- passes and draws -----------------------------------------------------------------------------------------------------------------------

bgfx::FrameBufferHandle BgfxGpuImpl::framebuffer_for(const bgfx::TextureHandle& color, const bgfx::TextureHandle& depth, uint32_t layer, uint32_t mip) {
    const uint64_t key = uint64_t(color.idx) | (uint64_t(bgfx::isValid(depth) ? depth.idx : 0xFFFF) << 16) | (uint64_t(layer) << 32) | (uint64_t(mip) << 40);
    auto it = framebuffers_.find(key);
    if (it != framebuffers_.end()) return it->second;
    bgfx::Attachment attachments[2];
    attachments[0].init(color, bgfx::Access::Write, uint16_t(layer), 1, uint16_t(mip), 0);
    if (bgfx::isValid(depth)) attachments[1].init(depth, bgfx::Access::Write, 0, 1, 0, 0);
    const bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(bgfx::isValid(depth) ? 2 : 1, attachments, false);
    if (!bgfx::isValid(fb)) { char what[160]; std::snprintf(what, sizeof what, "framebuffer (colour texture %u, depth texture %d, layer %u, mip %u)", unsigned(color.idx), bgfx::isValid(depth) ? int(depth.idx) : -1, layer, mip); fail(what); }
    if (bgfx::isValid(fb)) framebuffers_[key] = fb;   // an invalid one is not kept (it would be destroyed later); the pass that needs it is dropped
    return fb;
}

void BgfxGpuImpl::begin_pass(const PassDesc& p) {
    TextureEntry* color = texture_entry(p.colors[0]);
    if (!color) { pass_open_ = false; return; }
    TextureEntry* depth = p.depth_stencil ? textures_.get(p.depth_stencil.value) : nullptr;
    current_view_ = next_view_++;
    ++stats_.views;
    pass_open_ = true; pass_draws_ = 0;
    const bgfx::FrameBufferHandle fb = framebuffer_for(color->handle, depth ? depth->handle : bgfx::TextureHandle(BGFX_INVALID_HANDLE), p.color_layer[0], p.color_mip[0]);
    if (!bgfx::isValid(fb)) { pass_open_ = false; return; }   // e.g. attachments with different multisampling
    bgfx::setViewFrameBuffer(uint16_t(current_view_), fb);
    uint32_t rect[4] = {uint32_t(p.rect[0]), uint32_t(p.rect[1]), uint32_t(p.rect[2]), uint32_t(p.rect[3])};
    if (!rect[2] || !rect[3]) { rect[0] = rect[1] = 0; rect[2] = std::max(1u, color->width >> p.color_mip[0]); rect[3] = std::max(1u, color->height >> p.color_mip[0]); }
    std::memcpy(pass_rect_, rect, sizeof rect);
    bgfx::setViewRect(uint16_t(current_view_), int16_t(rect[0]), int16_t(rect[1]), uint16_t(rect[2]), uint16_t(rect[3]));
    uint16_t clear = 0;
    if (p.clear_color) clear |= BGFX_CLEAR_COLOR;
    if (p.clear_depth && depth) clear |= BGFX_CLEAR_DEPTH;
    if (p.clear_stencil && depth) clear |= BGFX_CLEAR_STENCIL;
    const uint32_t a = p.clear_argb;
    const uint32_t rgba = ((a >> 16 & 0xFF) << 24) | ((a >> 8 & 0xFF) << 16) | ((a & 0xFF) << 8) | (a >> 24);
    bgfx::setViewClear(uint16_t(current_view_), clear, rgba, p.clear_z, p.clear_stencil_value);
    bgfx::setViewMode(uint16_t(current_view_), bgfx::ViewMode::Sequential);
}

void BgfxGpuImpl::end_pass() {
    if (pass_open_ && pass_draws_ == 0) bgfx::touch(uint16_t(current_view_));
    pass_open_ = false;
}

bgfx::VertexLayoutHandle BgfxGpuImpl::layout_handle(const DrawDesc& d, uint32_t stream) {
    uint64_t key = 0xcbf29ce484222325ull ^ d.vertex_stride[stream];
    for (const auto& a : d.layout) if (a.stream == stream) { key = (key ^ (uint64_t(a.offset) | uint64_t(a.type) << 16 | uint64_t(a.semantic) << 24 | uint64_t(a.index) << 32)) * 0x100000001b3ull; }
    auto it = layouts_.find(key);
    if (it != layouts_.end()) return it->second;
    struct Item { uint16_t offset; bgfx::Attrib::Enum attrib; AttributeFormat f; };
    std::vector<Item> items;
    for (const auto& a : d.layout) {
        if (a.stream != stream) continue;
        AttributeFormat f;
        if (!attribute_format(a.type, f)) continue;
        items.push_back({a.offset, bgfx::Attrib::Enum(shader::bgfx_attribute_for(a.semantic, a.index)), f});
    }
    std::sort(items.begin(), items.end(), [](const Item& l, const Item& r) { return l.offset < r.offset; });
    bgfx::VertexLayout layout;
    layout.begin();
    uint32_t pos = 0;
    auto skip = [&](uint32_t n) { while (n) { const uint8_t step = uint8_t(std::min(n, 255u)); layout.skip(step); n -= step; } };
    uint32_t used_mask = 0;
    for (const Item& it2 : items) {
        if (used_mask & (1u << it2.attrib)) continue;
        used_mask |= 1u << it2.attrib;
        if (it2.offset > pos) skip(it2.offset - pos);
        layout.add(it2.attrib, it2.f.num, it2.f.type, it2.f.normalized, it2.f.as_int);
        pos = it2.offset + it2.f.bytes;
    }
    if (d.vertex_stride[stream] > pos) skip(d.vertex_stride[stream] - pos);
    layout.end();
    const bgfx::VertexLayoutHandle h = bgfx::createVertexLayout(layout);
    if (!bgfx::isValid(h)) { fail("vertex layout"); return h; }
    layouts_[key] = h;
    return h;
}

void BgfxGpuImpl::draw(const DrawDesc& d) {
    if (!pass_open_) return;
    const uint64_t t0 = now_ns();
    ProgramEntry* program = programs_.get(d.program.value);
    if (!program) return;
    uint32_t blend_factor = 0;
    const uint64_t state = pack_state(d, blend_factor);
    bgfx::setState(state, blend_factor);
    const uint32_t stencil = pack_stencil(d.depth_stencil);
    bgfx::setStencil(stencil, BGFX_STENCIL_NONE);
    if (d.raster.depth_bias != 0.0f || d.raster.slope_scale_bias != 0.0f) bgfx::setDepthControl(int32_t(d.raster.depth_bias * 16777216.0f), d.raster.slope_scale_bias);
    if (d.scissor_enabled) bgfx::setScissor(uint16_t(d.scissor[0]), uint16_t(d.scissor[1]), uint16_t(d.scissor[2]), uint16_t(d.scissor[3]));

    for (uint32_t s = 0; s < 4; ++s) {
        if (!d.vertex_buffer[s]) continue;
        BufferEntry* e = buffers_.get(d.vertex_buffer[s].value);
        if (!e) continue;
        const bgfx::VertexLayoutHandle lh = layout_handle(d, s);
        if (!bgfx::isValid(lh)) return;   // a draw without its vertex layout would read garbage
        const uint32_t stride = d.vertex_stride[s] ? d.vertex_stride[s] : 1;
        if (e->transient) bgfx::setVertexBuffer(uint8_t(s), &e->tvb, 0, d.vertex_count ? d.vertex_count : UINT32_MAX, lh);
        else bgfx::setVertexBuffer(uint8_t(s), e->vb, d.vertex_offset[s] / stride, d.vertex_count ? d.vertex_count : UINT32_MAX, lh);
    }
    if (d.index_buffer) {
        BufferEntry* e = buffers_.get(d.index_buffer.value);
        if (e) {
            if (e->transient) bgfx::setIndexBuffer(&e->tib, d.first_index, d.index_count);
            else bgfx::setIndexBuffer(e->ib, d.first_index, d.index_count);
        }
    }
    bgfx::setUniform(u_vs_, d.vertex_constants.data(), uint16_t(d.vertex_constants.size() / 16));
    bgfx::setUniform(u_ps_, d.pixel_constants.data(), uint16_t(d.pixel_constants.size() / 16));
    for (uint32_t i = 0; i < 16; ++i) {
        if (!d.textures[i]) continue;
        TextureEntry* t = textures_.get(d.textures[i].value);
        if (!t) continue;
        bgfx::setTexture(uint8_t(i), u_tex_[i], t->handle, sampler_flags(d.samplers[i]));
    }
    const uint64_t t3 = now_ns();
    bgfx::submit(uint16_t(current_view_), program->handle);
    const uint64_t t4 = now_ns();
    prof_submit_ns_ += t4 - t3; prof_draw_ns_ += t4 - t0; ++prof_draws_;
    ++pass_draws_;
    ++stats_.draws;
}

// ---- frame ----------------------------------------------------------------------------------------------------------------------------------

Status BgfxGpuImpl::resize(uint32_t width, uint32_t height) {
    if (backbuffer_.alive && backbuffer_.width == width && backbuffer_.height == height && backbuffer_.samples == std::min(backbuffer_samples_, std::max(caps_.max_samples, 1u))) return Status::ok;
    if (backbuffer_.alive) {
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
            if (uint16_t(it->first & 0xFFFF) == backbuffer_.handle.idx) { bgfx::destroy(it->second); it = framebuffers_.erase(it); } else ++it;
        }
        bgfx::destroy(backbuffer_.handle);
    }
    backbuffer_ = TextureEntry{};
    backbuffer_.alive = true; backbuffer_.render_target = true; backbuffer_.width = width; backbuffer_.height = height; backbuffer_.format = Format::bgra8;
    backbuffer_.samples = std::min(backbuffer_samples_, std::max(caps_.max_samples, 1u));
    backbuffer_.handle = bgfx::createTexture2D(uint16_t(width), uint16_t(height), false, 1, bgfx::TextureFormat::BGRA8,
                                               (backbuffer_.samples > 1 ? msaa_flag(backbuffer_.samples) : BGFX_TEXTURE_RT) | BGFX_TEXTURE_BLIT_DST | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
    if (bgfx::isValid(readback_)) { bgfx::destroy(readback_); readback_ = BGFX_INVALID_HANDLE; }
    return bgfx::isValid(backbuffer_.handle) ? Status::ok : Status::out_of_memory;
}

void BgfxGpuImpl::set_window_size(uint32_t width, uint32_t height) {
    window_width_ = width; window_height_ = height;
    bgfx::reset(options_.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);
    // bgfx 's swap chain follows the reset size given at init: a resized window needs a new swap chain description
    bgfx::SwapChain sc; sc.nwh = options_.window; sc.width = width; sc.height = height;
    bgfx::reset(options_.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE, &sc);
}

void BgfxGpuImpl::draw_quad(uint32_t view, bgfx::TextureHandle source, uint32_t sampler, const float uv[4]) {
    bgfx::TransientVertexBuffer tvb;
    bgfx::VertexLayout layout;
    layout.begin().add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float).add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float).end();
    bgfx::allocTransientVertexBuffer(&tvb, 3, layout);
    auto u = [&](float x) { return uv[0] + (uv[2] - uv[0]) * (0.5f + 0.5f * x); };
    auto v = [&](float y) { return uv[1] + (uv[3] - uv[1]) * (0.5f - 0.5f * y); };
    const float verts[3][6] = {{-1, -1, 0, 1, u(-1), v(-1)}, {3, -1, 0, 1, u(3), v(-1)}, {-1, 3, 0, 1, u(-1), v(3)}};
    std::memcpy(tvb.data, verts, sizeof verts);
    bgfx::setVertexBuffer(0, &tvb);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    const float vc[2][4] = {{1, 1, 0, 0}, {1, 0, 0, 0}};
    bgfx::setUniform(u_vs_, vc, 2);
    const float pc[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    bgfx::setUniform(u_ps_, pc, 2);
    bgfx::setTexture(0, u_tex_[0], source, sampler);
    bgfx::submit(uint16_t(view), blit_program_);
}

void BgfxGpuImpl::copy_texture(Texture dst_id, const int32_t* dr, Texture src_id, const int32_t* sr, bool linear) {
    end_pass();
    TextureEntry* d = texture_entry(dst_id);
    TextureEntry* s = texture_entry(src_id);
    if (!d || !s || !bgfx::isValid(d->handle) || !bgfx::isValid(s->handle)) return;
    const int32_t sx = sr ? sr[0] : 0, sy = sr ? sr[1] : 0, sw = sr ? sr[2] : int32_t(s->width), sh = sr ? sr[3] : int32_t(s->height);
    const int32_t dx = dr ? dr[0] : 0, dy = dr ? dr[1] : 0, dw = dr ? dr[2] : int32_t(d->width), dh = dr ? dr[3] : int32_t(d->height);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    const uint32_t view = next_view_++;
    if (sw == dw && sh == dh && s->format == d->format) {
        bgfx::TextureRegion to, from;
        to.init(d->handle, uint16_t(dx), uint16_t(dy), uint16_t(dw), uint16_t(dh));
        from.init(s->handle, uint16_t(sx), uint16_t(sy), uint16_t(sw), uint16_t(sh));
        bgfx::blit(uint16_t(view), to, from);
        return;
    }
    const bgfx::FrameBufferHandle fb = framebuffer_for(d->handle, BGFX_INVALID_HANDLE);
    bgfx::setViewFrameBuffer(uint16_t(view), fb);
    bgfx::setViewRect(uint16_t(view), int16_t(dx), int16_t(dy), uint16_t(dw), uint16_t(dh));
    bgfx::setViewClear(uint16_t(view), BGFX_CLEAR_NONE);
    bgfx::setViewMode(uint16_t(view), bgfx::ViewMode::Sequential);
    const float W = float(s->width), H = float(s->height);
    const float uv[4] = {float(sx) / W, float(sy) / H, float(sx + sw) / W, float(sy + sh) / H};
    draw_quad(view, s->handle, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | (linear ? 0 : BGFX_SAMPLER_POINT), uv);
}

Status BgfxGpuImpl::present(uint32_t) {
    end_pass();
    if (backbuffer_.alive && blit_program_.idx != bgfx::kInvalidHandle) {
        // The picture goes to the visible part of the window with its own aspect ratio (black bars when the window or the screen differs from it); a view
        // that clears the whole window comes first.
        const FitRect fit = visible_set_ ? fit_aspect(backbuffer_.width, backbuffer_.height, visible_[0], visible_[1], uint32_t(visible_[2]), uint32_t(visible_[3]))
                                         : fit_aspect(backbuffer_.width, backbuffer_.height, 0, 0, window_width_, window_height_);
        const uint32_t clear_view = next_view_++;
        bgfx::setViewFrameBuffer(uint16_t(clear_view), BGFX_INVALID_HANDLE);
        bgfx::setViewRect(uint16_t(clear_view), 0, 0, uint16_t(window_width_), uint16_t(window_height_));
        bgfx::setViewClear(uint16_t(clear_view), BGFX_CLEAR_COLOR, 0x000000ff, 1.0f, 0);
        bgfx::touch(uint16_t(clear_view));
        const uint32_t view = next_view_++;
        bgfx::setViewFrameBuffer(uint16_t(view), BGFX_INVALID_HANDLE);
        bgfx::setViewRect(uint16_t(view), uint16_t(std::max(fit.x, 0)), uint16_t(std::max(fit.y, 0)), uint16_t(fit.width), uint16_t(fit.height));
        bgfx::setViewClear(uint16_t(view), BGFX_CLEAR_NONE);
        bgfx::setViewMode(uint16_t(view), bgfx::ViewMode::Sequential);
        const float uv[4] = {0, 0, 1, 1};
        draw_quad(view, backbuffer_.handle, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP | (fit.width == backbuffer_.width && fit.height == backbuffer_.height ? BGFX_SAMPLER_POINT : 0), uv);
    }
    const uint64_t t_frame = now_ns();
    bgfx::frame();
    prof_frame_ns_ += now_ns() - t_frame; ++prof_frames_;
    if (const bgfx::Stats* st = bgfx::getStats()) {   // timings of the frame the backend finished last
        if (st->gpuTimerFreq) { stats_.gpu_ms = 1000.0 * double(st->gpuTimeEnd - st->gpuTimeBegin) / double(st->gpuTimerFreq); prof_gpu_ms_ += stats_.gpu_ms; }
        if (st->cpuTimerFreq) { stats_.render_cpu_ms = 1000.0 * double(st->cpuTimeEnd - st->cpuTimeBegin) / double(st->cpuTimerFreq); stats_.wait_render_ms = 1000.0 * double(st->waitRender) / double(st->cpuTimerFreq); stats_.wait_submit_ms = 1000.0 * double(st->waitSubmit) / double(st->cpuTimerFreq); }
        if (st->cpuTimerFreq) { prof_render_cpu_ms_ += 1000.0 * double(st->cpuTimeEnd - st->cpuTimeBegin) / double(st->cpuTimerFreq); prof_wait_render_ms_ += 1000.0 * double(st->waitRender) / double(st->cpuTimerFreq); prof_wait_submit_ms_ += 1000.0 * double(st->waitSubmit) / double(st->cpuTimerFreq); }
    }
    ++frame_;
    next_view_ = 0;
    // transient buffer entries die with the frame
    for (uint32_t i = 1; i < buffers_.items.size(); ++i) if (buffers_.items[i].alive && buffers_.items[i].transient) buffers_.release(i);
    return Status::ok;
}

Status BgfxGpuImpl::read_back(Texture id, uint32_t& width, uint32_t& height, std::vector<std::byte>& bgra) {
    TextureEntry* src = texture_entry(id);
    if (!src || src->depth) return Status::invalid_argument;
    if (src->format != Format::bgra8 && src->format != Format::rgba8) return Status::unsupported;
    end_pass();
    if (!bgfx::isValid(readback_) || readback_w_ != src->width || readback_h_ != src->height) {
        if (bgfx::isValid(readback_)) bgfx::destroy(readback_);
        readback_ = bgfx::createTexture2D(uint16_t(src->width), uint16_t(src->height), false, 1, texture_format(src->format), BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK | BGFX_SAMPLER_UVW_CLAMP);
        readback_w_ = src->width; readback_h_ = src->height;
    }
    const uint32_t view = next_view_++;
    bgfx::TextureRegion dst, from;
    dst.init(readback_);
    from.init(src->handle);
    bgfx::blit(uint16_t(view), dst, from);
    width = src->width; height = src->height;
    bgra.assign(size_t(width) * height * 4, std::byte{0});
    bgfx::TextureRegion region;
    region.init(readback_);
    const uint32_t ready = bgfx::read(region, bgra.data());
    uint32_t guard = 0;
    while (bgfx::frame() < ready && guard++ < 8) {}
    next_view_ = 0;
    if (src->format == Format::rgba8) for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    return Status::ok;
}

}  // namespace

std::unique_ptr<BgfxGpu> create_bgfx_gpu(const BgfxOptions& options, std::string& error) {
    auto gpu = std::make_unique<BgfxGpuImpl>();
    if (!gpu->init(options, error)) return nullptr;
    return gpu;
}

}  // namespace nue::gpu
