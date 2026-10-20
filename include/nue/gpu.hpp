// nue_d3d9, level B: the small GPU interface ("what a modern API does"), implemented by the bgfx adapter and by the null Gpu.
//
// The Direct3D 9 layer builds one DrawDesc per draw from its state shadow. There is no in-place buffer update on purpose: bgfx applies
// resource updates before the frame's draws, so data that changes between draws goes through `allocate_transient` or a new static buffer.
// Enumerations here are the modern ones (zero-based, no Direct3D numbering); the layer translates. Shaders arrive as SPIR-V modules in the
// conventions of nue/shader/d3d9_to_spirv.hpp (a uniform block per stage, textures at binding 2 + stage, samplers at 18 + stage); the backend
// turns them into whatever its renderer consumes.
#pragma once
#include "nue/shader/d3d9_to_spirv.hpp"
#include "nue/types.hpp"

namespace nue::gpu {

using Buffer = Id<struct BufferTag>;
using Texture = Id<struct TextureTag>;
using Program = Id<struct ProgramTag>;

// The swap chain's colour target (and, for passes that use it, the backend's own depth buffer), as a pass attachment.
inline constexpr Texture backbuffer{0xFFFFFFFFu};

enum class Format : uint8_t {
    rgba8, bgra8, r5g6b5, a4r4g4b4, a1r5g5b5, r8, a8, rg8, rgba16f, rgba32f, r16f, r32f, rg16f, rg32f, d16, d24s8, d32f, bc1, bc2, bc3, astc4x4, etc2_rgba8, rgba8s, count
};
enum class BufferKind : uint8_t { vertex, index16, index32 };
enum class Topology : uint8_t { points, lines, line_strip, triangles, triangle_strip };   // fans are converted to lists by the layer

// Vertex elements keep the Direct3D 9 numbering of type (D3DDECLTYPE_*) and semantic (D3DDECLUSAGE_*): they are the shader's input contract and
// the backend maps them to its own attributes. `bgra` marks D3DCOLOR (the shader swizzles it: see TranslateOptions::bgra_attributes).
struct VertexAttribute { uint8_t stream = 0; uint16_t offset = 0; uint8_t type = 0, semantic = 0, index = 0; };

struct SamplerDesc {
    uint8_t min = 1, mag = 1, mip = 1;                     // 0 point, 1 linear, 2 anisotropic (min only)
    bool use_mips = true;                                  // false: only the top level is sampled
    uint8_t address_u = 0, address_v = 0, address_w = 0;   // 0 wrap, 1 mirror, 2 clamp, 3 border, 4 mirror once
    uint8_t max_aniso = 1;
    float lod_bias = 0.0f;
    uint32_t border_argb = 0;
    friend bool operator==(const SamplerDesc&, const SamplerDesc&) = default;
};

enum class BlendFactor : uint8_t { zero, one, src_color, inv_src_color, src_alpha, inv_src_alpha, dst_alpha, inv_dst_alpha, dst_color, inv_dst_color, src_alpha_sat, constant, inv_constant };
enum class BlendOp : uint8_t { add, subtract, reverse_subtract, min, max };
enum class Compare : uint8_t { never, less, equal, less_equal, greater, not_equal, greater_equal, always };
enum class StencilOp : uint8_t { keep, zero, replace, incr_sat, decr_sat, invert, incr, decr };
enum class Cull : uint8_t { none, cw, ccw };

struct BlendState {
    bool enable = false;
    BlendFactor src = BlendFactor::one, dst = BlendFactor::zero, src_alpha = BlendFactor::one, dst_alpha = BlendFactor::zero;
    BlendOp op = BlendOp::add, op_alpha = BlendOp::add;
    uint8_t write_mask = 0xF;                 // bit 0 red .. bit 3 alpha
    uint32_t constant_argb = 0;
    friend bool operator==(const BlendState&, const BlendState&) = default;
};
struct DepthStencilState {
    bool depth_test = true, depth_write = true;
    Compare depth_func = Compare::less_equal;
    bool stencil = false;
    Compare stencil_func = Compare::always;
    StencilOp stencil_fail = StencilOp::keep, stencil_depth_fail = StencilOp::keep, stencil_pass = StencilOp::keep;
    uint8_t stencil_ref = 0, stencil_read_mask = 0xFF, stencil_write_mask = 0xFF;
    friend bool operator==(const DepthStencilState&, const DepthStencilState&) = default;
};
struct RasterState {
    Cull cull = Cull::ccw;                    // Direct3D 9's front face is clockwise: the default culls counter-clockwise
    bool wireframe = false;
    float depth_bias = 0.0f, slope_scale_bias = 0.0f;
    friend bool operator==(const RasterState&, const RasterState&) = default;
};

// One program: a vertex and a pixel module. `key` identifies it for caches and for the deterministic call hash of the null Gpu.
struct ProgramDesc {
    uint64_t key = 0;
    const shader::TranslateResult* vertex = nullptr;
    const shader::TranslateResult* pixel = nullptr;
};

struct DrawDesc {
    Program program;
    Buffer vertex_buffer[4];
    uint32_t vertex_offset[4] = {}, vertex_stride[4] = {};   // byte offset of the first vertex of the range, bytes per vertex
    Buffer index_buffer;
    uint32_t index_offset = 0;                               // byte offset into the index buffer
    bool index32 = false;
    std::span<const VertexAttribute> layout;
    Texture textures[16];
    SamplerDesc samplers[16];
    // Contents of the uniform block of each stage: `fixup` registers (viewport/depth/half-pixel for vertex shaders; alpha test and fog for pixel
    // shaders) followed by the float constant registers the program reads; packed float4.
    Bytes vertex_constants, pixel_constants;
    BlendState blend;
    DepthStencilState depth_stencil;
    RasterState raster;
    Topology topology = Topology::triangles;
    int32_t base_vertex = 0;                                 // added to every index
    uint32_t first_index = 0, index_count = 0, vertex_count = 0;
    int32_t scissor[4] = {};                                 // x, y, width, height
    bool scissor_enabled = false;
};

struct PassDesc {
    Texture colors[4];
    uint8_t color_layer[4] = {}, color_mip[4] = {};            // cube face / mip level of each colour attachment
    Texture depth_stencil;
    int32_t rect[4] = {};                                    // x, y, width, height of the area drawn to and cleared (zeros: the whole attachment)
    bool clear_color = false, clear_depth = false, clear_stencil = false;
    uint32_t clear_argb = 0;
    float clear_z = 1.0f;
    uint8_t clear_stencil_value = 0;
};

struct GpuCaps {
    bool texture_format[static_cast<size_t>(Format::count)] = {};
    uint32_t max_texture_size = 0, max_anisotropy = 1, max_samples = 1;   // max_samples: multisampled render targets (1 = none)
    bool instancing = false, mrt = false;
    bool origin_bottom_left = false;                         // OpenGL-style render targets
    bool homogeneous_depth = false;                          // clip-space depth in [-1, 1] (OpenGL)
    const char* name = "";
};

class Gpu {   // game thread; the backend's render thread hides behind it
public:
    virtual ~Gpu() = default;
    virtual const GpuCaps& caps() = 0;
    virtual Result<Buffer>  create_static_buffer(Bytes initial, BufferKind kind) = 0;   // immutable after creation
    // Memory valid for this frame only (dynamic data, user-pointer draws): `bytes` of vertices of `stride` bytes, or of indices (`index`, stride 2 or 4).
    // `out` receives the span to fill. Draw it with offset 0.
    virtual Result<Buffer>  allocate_transient(uint32_t bytes, uint32_t stride, bool index, MutableBytes& out) = 0;
    // `samples` > 1 makes a multisampled render target (colour or depth): it resolves when it is sampled or copied to a texture.
    virtual Result<Texture> create_texture(uint32_t width, uint32_t height, uint32_t levels, Format, bool render_target, bool cube, uint32_t samples = 1) = 0;
    virtual void            update_texture(Texture, uint32_t level, uint32_t face, const uint32_t* rect4, Bytes data, uint32_t row_pitch) = 0;
    virtual Result<Program> create_program(const ProgramDesc&) = 0;
    // Copies (and, when the sizes differ, stretches) a rectangle {x, y, w, h} of `src` into `dst`; null rectangles are the whole texture.
    virtual void copy_texture(Texture dst, const int32_t* dst_rect4, Texture src, const int32_t* src_rect4, bool linear) = 0;
    virtual void begin_pass(const PassDesc&) = 0;
    virtual void draw(const DrawDesc&) = 0;
    virtual void end_pass() = 0;
    virtual Status present(uint32_t swap_interval) = 0;
    virtual Status resize(uint32_t width, uint32_t height) = 0;
    virtual void   set_visible_area(int32_t x, int32_t y, uint32_t width, uint32_t height) { (void)x; (void)y; (void)width; (void)height; }   // the part of the window that is really on screen (client coordinates): present() fits the picture into it
    virtual void   set_backbuffer_samples(uint32_t samples) { (void)samples; }   // multisampling of the game's back buffer; takes effect at the next resize()
    // Copies the colour contents of a texture (or the last presented frame for `backbuffer`) as 32-bit BGRA; waits for the GPU (rare, slow).
    virtual Status read_back(Texture, uint32_t& width, uint32_t& height, std::vector<std::byte>& bgra) = 0;
    virtual void   destroy(Buffer) = 0;
    virtual void   destroy(Texture) = 0;
    virtual void   destroy(Program) = 0;
};

}  // namespace nue::gpu
