// IDirect3DDevice9 on nue::d3d9::Layer. Every state call is forwarded to the layer (which keeps the authoritative shadow); this class keeps its own
// copy only to answer the Get* calls, and holds the COM references Direct3D 9 holds on bound resources.
#include "facade.hpp"
#include "nue/shader/d3d9_shader.hpp"
#include <algorithm>
#include <cstring>

namespace nue::facade {
namespace {

UINT vertices_for(D3DPRIMITIVETYPE type, UINT primitives) {
    switch (type) {
    case D3DPT_POINTLIST: return primitives;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    case D3DPT_TRIANGLELIST: return primitives * 3;
    default: return primitives + 2;   // strip, fan
    }
}

d3d9::TextureId texture_id_of(IDirect3DBaseTexture9* base) {
    if (!base) return {};
    if (base->GetType() == D3DRTYPE_CUBETEXTURE) return static_cast<CubeTexture9*>(static_cast<IDirect3DCubeTexture9*>(base))->id();
    return static_cast<Texture9*>(static_cast<IDirect3DTexture9*>(base))->id();
}

void rect_to_array(const RECT* r, int32_t out[4]) { out[0] = r->left; out[1] = r->top; out[2] = r->right; out[3] = r->bottom; }

}  // namespace

HRESULT Device9::unsupported(const char* what) try { NUE_CALL(); 
    if (config().log_unsupported && unsupported_seen_[what]++ == 0) log("unsupported by the translator: %s", what);
    return trace_result(__func__, D3DERR_NOTAVAILABLE);
} catch (...) { return guard_failed(__func__); }

Device9::Device9(Direct3D9* d3d, std::shared_ptr<Core> core, const D3DDEVICE_CREATION_PARAMETERS& creation, const D3DPRESENT_PARAMETERS& pp, const D3DCAPS9& caps, HWND window)
    : d3d_(d3d), core_(std::move(core)), creation_(creation), pp_(pp), caps_(caps), window_(window) {
    d3d_->AddRef();
    // Direct3D 9 defaults for the render states a game reads back most
    render_state_[D3DRS_ZENABLE] = pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    render_state_[D3DRS_ZWRITEENABLE] = TRUE; render_state_[D3DRS_FILLMODE] = D3DFILL_SOLID; render_state_[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    render_state_[D3DRS_SRCBLEND] = D3DBLEND_ONE; render_state_[D3DRS_DESTBLEND] = D3DBLEND_ZERO; render_state_[D3DRS_CULLMODE] = D3DCULL_CCW;
    render_state_[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL; render_state_[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS; render_state_[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    render_state_[D3DRS_STENCILFAIL] = render_state_[D3DRS_STENCILZFAIL] = render_state_[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    render_state_[D3DRS_STENCILMASK] = render_state_[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF; render_state_[D3DRS_COLORWRITEENABLE] = 0xF;
    render_state_[D3DRS_LIGHTING] = TRUE; render_state_[D3DRS_CLIPPING] = TRUE; render_state_[D3DRS_BLENDOP] = D3DBLENDOP_ADD; render_state_[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    render_state_[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    const float one = 1.0f;
    std::memcpy(&render_state_[D3DRS_FOGEND], &one, 4); std::memcpy(&render_state_[D3DRS_FOGDENSITY], &one, 4);
    for (auto& s : sampler_state_) { s[D3DSAMP_ADDRESSU] = s[D3DSAMP_ADDRESSV] = s[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP; s[D3DSAMP_MAGFILTER] = s[D3DSAMP_MINFILTER] = D3DTEXF_POINT; s[D3DSAMP_MAXANISOTROPY] = 1; }
    for (auto& s : stage_state_) { s[D3DTSS_COLOROP] = D3DTOP_DISABLE; s[D3DTSS_ALPHAOP] = D3DTOP_DISABLE; s[D3DTSS_COLORARG1] = s[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE; s[D3DTSS_COLORARG2] = s[D3DTSS_ALPHAARG2] = D3DTA_CURRENT; s[D3DTSS_RESULTARG] = D3DTA_CURRENT; }
    stage_state_[0][D3DTSS_COLOROP] = D3DTOP_MODULATE; stage_state_[0][D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
    viewport_ = D3DVIEWPORT9{0, 0, pp.BackBufferWidth, pp.BackBufferHeight, 0.0f, 1.0f};
    window_w_ = pp.BackBufferWidth; window_h_ = pp.BackBufferHeight;
    assign<IDirect3DSurface9>(render_targets_[0], back_buffer_surface());
    if (auto* dev = core_->dev()) {
        const d3d9::SurfaceId depth = dev->auto_depth_stencil();
        if (depth) {
            auto_depth_ = new Surface9(child(), Surface9::Kind::depth_stencil, depth, pp.BackBufferWidth, pp.BackBufferHeight, pp.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE);
            assign<IDirect3DSurface9>(depth_stencil_, auto_depth_);
        }
    }
}

Device9::~Device9() { shutdown(); device_destroyed(); if (d3d_) d3d_->Release(); }

void Device9::shutdown() {
    for (auto& t : textures_) if (t) { t->Release(); t = nullptr; }
    for (auto& s : streams_) if (s) { s->Release(); s = nullptr; }
    for (auto& s : render_targets_) if (s) { s->Release(); s = nullptr; }
    if (indices_) { indices_->Release(); indices_ = nullptr; }
    if (depth_stencil_) { depth_stencil_->Release(); depth_stencil_ = nullptr; }
    if (declaration_) { declaration_->Release(); declaration_ = nullptr; }
    if (vertex_shader_) { vertex_shader_->Release(); vertex_shader_ = nullptr; }
    if (pixel_shader_) { pixel_shader_->Release(); pixel_shader_ = nullptr; }
    if (auto_depth_) { auto_depth_->Release(); auto_depth_ = nullptr; }
    if (back_buffer_) { back_buffer_->Release(); back_buffer_ = nullptr; }
    if (core_ && core_->alive) {
        if (core_->layer) {
            log("device released; what the translator could not do:\n%s", core_->layer->report().c_str());
            core_->layer->flush();
        }
        core_->alive = false;
        core_->layer.reset();
        core_->gpu.reset();
    }
}

Surface9* Device9::back_buffer_surface() {
    if (!back_buffer_) {
        auto* dev = core_->dev();
        if (!dev) return nullptr;
        auto r = dev->back_buffer(0, 0);
        if (!r) return nullptr;
        back_buffer_ = new Surface9(child(), Surface9::Kind::back_buffer, r.value, pp_.BackBufferWidth, pp_.BackBufferHeight, pp_.BackBufferFormat, D3DMULTISAMPLE_NONE);
    }
    return back_buffer_;
}

d3d9::DeviceParams Device9::params_from(const D3DPRESENT_PARAMETERS& pp, UINT& width, UINT& height) const {
    RECT client{};
    if (window_) GetClientRect(window_, &client);
    width = pp.BackBufferWidth ? pp.BackBufferWidth : std::max<LONG>(client.right - client.left, 1);
    height = pp.BackBufferHeight ? pp.BackBufferHeight : std::max<LONG>(client.bottom - client.top, 1);
    d3d9::DeviceParams p;
    p.back_width = width; p.back_height = height;
    p.back_format = pp.BackBufferFormat == D3DFMT_UNKNOWN ? uint32_t(D3DFMT_X8R8G8B8) : uint32_t(pp.BackBufferFormat);
    p.depth_format = pp.EnableAutoDepthStencil ? uint32_t(pp.AutoDepthStencilFormat) : 0;
    p.multisample = uint32_t(pp.MultiSampleType);
    p.buffers = pp.BackBufferCount ? pp.BackBufferCount : 1;
    p.windowed = pp.Windowed != FALSE;
    p.present_interval = pp.PresentationInterval;
    return p;
}

void Device9::apply_window_size() {
    if (!window_ || !core_->gpu) return;
    RECT client{};
    if (!GetClientRect(window_, &client)) return;
    const UINT w = UINT(std::max<LONG>(client.right - client.left, 0)), h = UINT(std::max<LONG>(client.bottom - client.top, 0));
    if (w && h && (w != window_w_ || h != window_h_)) { window_w_ = w; window_h_ = h; core_->gpu->set_window_size(w, h); }
}

// ---- device -----------------------------------------------------------------------------------------------------------------------------

HRESULT Device9::TestCooperativeLevel() try { NUE_CALL(); auto* dev = core_->dev(); return dev ? to_hresult(dev->test_cooperative_level()) : D3DERR_DEVICELOST; } catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetDirect3D(IDirect3D9** out) try { NUE_CALL(); if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = d3d_; d3d_->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetDeviceCaps(D3DCAPS9* caps) try { NUE_CALL(); if (!caps) return trace_result(__func__, D3DERR_INVALIDCALL); *caps = caps_; return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* params) try { NUE_CALL(); if (!params) return trace_result(__func__, D3DERR_INVALIDCALL); *params = creation_; return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetDisplayMode(UINT, D3DDISPLAYMODE* mode) try { NUE_CALL(); 
    if (!mode) return trace_result(__func__, D3DERR_INVALIDCALL);
    mode->Width = pp_.BackBufferWidth; mode->Height = pp_.BackBufferHeight; mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetSwapChain(UINT index, IDirect3DSwapChain9** out) try { NUE_CALL(); 
    if (!out || index != 0) return trace_result(__func__, D3DERR_INVALIDCALL);
    *out = new SwapChain9(this);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetRasterStatus(UINT, D3DRASTER_STATUS* status) try { NUE_CALL(); if (!status) return trace_result(__func__, D3DERR_INVALIDCALL); status->InVBlank = FALSE; status->ScanLine = 0; return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::Reset(D3DPRESENT_PARAMETERS* pp) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !pp) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (in_scene_) { dev->end_scene(); in_scene_ = false; }
    UINT w = 0, h = 0;
    const d3d9::DeviceParams params = params_from(*pp, w, h);
    const Status status = dev->reset(params);
    if (status != Status::ok) return to_hresult(status);
    apply_d3d9_fpu_mode(creation_.BehaviorFlags);
    pp_ = *pp;
    pp_.BackBufferWidth = w; pp_.BackBufferHeight = h;
    if (pp_.BackBufferFormat == D3DFMT_UNKNOWN) pp_.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!pp_.BackBufferCount) pp_.BackBufferCount = 1;
    *pp = pp_;
    if (pp_.hDeviceWindow) window_ = pp_.hDeviceWindow;
    window_w_ = 0; window_h_ = 0;
    apply_window_size();
    // a Reset makes the back buffer and the automatic depth buffer the targets again
    if (back_buffer_) { back_buffer_->Release(); back_buffer_ = nullptr; }
    if (auto_depth_) { auto_depth_->Release(); auto_depth_ = nullptr; }
    assign<IDirect3DSurface9>(render_targets_[0], back_buffer_surface());
    for (int i = 1; i < 4; ++i) assign<IDirect3DSurface9>(render_targets_[i], nullptr);
    const d3d9::SurfaceId depth = dev->auto_depth_stencil();
    if (depth) {
        auto_depth_ = new Surface9(child(), Surface9::Kind::depth_stencil, depth, w, h, pp_.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE);
        assign<IDirect3DSurface9>(depth_stencil_, auto_depth_);
    } else assign<IDirect3DSurface9>(depth_stencil_, nullptr);
    viewport_ = D3DVIEWPORT9{0, 0, w, h, 0.0f, 1.0f};
    log("Reset: %ux%u format %d depth %d interval %u", w, h, int(pp_.BackBufferFormat), int(pp_.AutoDepthStencilFormat), unsigned(pp_.PresentationInterval));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::Present(const RECT*, const RECT*, HWND, const RGNDATA*) try { NUE_CALL(); ++g_counter_present; 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_DEVICELOST);
    if (in_scene_) { dev->end_scene(); in_scene_ = false; }
    apply_window_size();
    return to_hresult(dev->present());
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetBackBuffer(UINT swap_chain, UINT index, D3DBACKBUFFER_TYPE, IDirect3DSurface9** out) try { NUE_CALL(); 
    if (!out || swap_chain != 0 || index != 0) return trace_result(__func__, D3DERR_INVALIDCALL);
    Surface9* s = back_buffer_surface();
    if (!s) return trace_result(__func__, D3DERR_INVALIDCALL);
    s->AddRef();
    *out = s;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

// ---- resource creation ------------------------------------------------------------------------------------------------------------------

HRESULT Device9::CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->create_texture(width, height, levels, usage, uint32_t(format), uint32_t(pool));
    if (!r) return to_hresult(r.status);
    *out = new Texture9(child(), r.value, width, height, levels, usage, format, pool);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->create_cube_texture(edge, levels, usage, uint32_t(format), uint32_t(pool));
    if (!r) return to_hresult(r.status);
    *out = new CubeTexture9(child(), r.value, edge, levels, usage, format, pool);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->create_vertex_buffer(length, usage, fvf, uint32_t(pool));
    if (!r) return to_hresult(r.status);
    *out = new VertexBuffer9(child(), r.value, length, usage, fvf, pool);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->create_index_buffer(length, usage, uint32_t(format), uint32_t(pool));
    if (!r) return to_hresult(r.status);
    *out = new IndexBuffer9(child(), r.value, length, usage, format, pool);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, DWORD, BOOL lockable, IDirect3DSurface9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->create_render_target(width, height, uint32_t(format), uint32_t(multisample), lockable != FALSE);
    if (!r) return to_hresult(r.status);
    *out = new Surface9(child(), Surface9::Kind::render_target, r.value, width, height, format, multisample);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateDepthStencilSurface(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, DWORD, BOOL, IDirect3DSurface9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->create_depth_stencil(width, height, uint32_t(format), uint32_t(multisample));
    if (!r) return to_hresult(r.status);
    *out = new Surface9(child(), Surface9::Kind::depth_stencil, r.value, width, height, format, multisample);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9** out, HANDLE*) try { NUE_CALL(); ++g_counter_create; 
    if (!out) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH) return unsupported("CreateOffscreenPlainSurface in the default pool");
    *out = new Surface9(child(), Surface9::Kind::system_memory, d3d9::SurfaceId{}, width, height, format, D3DMULTISAMPLE_NONE);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !elements || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    size_t count = 0;
    while (elements[count].Stream != 0xFF && count < 64) ++count;
    if (count >= 64) return trace_result(__func__, D3DERR_INVALIDCALL);
    std::vector<D3DVERTEXELEMENT9> copy(elements, elements + count + 1);   // with the end marker
    auto r = dev->create_vertex_declaration(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(copy.data()), copy.size() * sizeof(D3DVERTEXELEMENT9)));
    if (!r) return to_hresult(r.status);
    *out = new VertexDeclaration9(child(), r.value, std::move(copy));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreateVertexShader(const DWORD* function, IDirect3DVertexShader9** out) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !function || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    const size_t n = shader::token_count(reinterpret_cast<const uint32_t*>(function));
    if (!n) return trace_result(__func__, D3DERR_INVALIDCALL);
    std::vector<DWORD> code(function, function + n);
    auto r = dev->create_vertex_shader(std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(code.data()), code.size()));
    if (!r) return to_hresult(r.status);
    *out = new VertexShader9(child(), r.value, std::move(code));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::CreatePixelShader(const DWORD* function, IDirect3DPixelShader9** out) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !function || !out) return trace_result(__func__, D3DERR_INVALIDCALL);
    const size_t n = shader::token_count(reinterpret_cast<const uint32_t*>(function));
    if (!n) return trace_result(__func__, D3DERR_INVALIDCALL);
    std::vector<DWORD> code(function, function + n);
    auto r = dev->create_pixel_shader(std::span<const uint32_t>(reinterpret_cast<const uint32_t*>(code.data()), code.size()));
    if (!r) return to_hresult(r.status);
    *out = new PixelShader9(child(), r.value, std::move(code));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

// ---- surfaces ---------------------------------------------------------------------------------------------------------------------------

HRESULT Device9::StretchRect(IDirect3DSurface9* source, const RECT* source_rect, IDirect3DSurface9* destination, const RECT* destination_rect, D3DTEXTUREFILTERTYPE filter) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !source || !destination) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto* from = static_cast<Surface9*>(source);
    auto* to = static_cast<Surface9*>(destination);
    int32_t a[4], b[4];
    if (source_rect) rect_to_array(source_rect, a);
    if (destination_rect) rect_to_array(destination_rect, b);
    return to_hresult(dev->stretch_rect(from->id(), source_rect ? a : nullptr, to->id(), destination_rect ? b : nullptr, uint32_t(filter)));
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetRenderTargetData(IDirect3DSurface9* render_target, IDirect3DSurface9* destination) try { NUE_CALL(); 
    if (!render_target || !destination || !core_->gpu) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto* from = static_cast<Surface9*>(render_target);
    auto* to = static_cast<Surface9*>(destination);
    if (from->kind() != Surface9::Kind::back_buffer) return unsupported("GetRenderTargetData of a surface other than the back buffer");
    if (to->kind() != Surface9::Kind::system_memory) return trace_result(__func__, D3DERR_INVALIDCALL);
    core_->layer->flush();
    uint32_t w = 0, h = 0;
    std::vector<std::byte> bgra;
    if (core_->gpu->read_back(gpu::backbuffer, w, h, bgra) != Status::ok) return trace_result(__func__, D3DERR_NOTAVAILABLE);
    const UINT copy_w = std::min<UINT>(w, to->width()), copy_h = std::min<UINT>(h, to->height());
    auto& memory = to->memory();
    for (UINT y = 0; y < copy_h; ++y) std::memcpy(memory.data() + size_t(y) * to->width() * 4, bgra.data() + size_t(y) * w * 4, size_t(copy_w) * 4);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetRenderTarget(DWORD index, IDirect3DSurface9* target) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || index >= 4) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (index == 0 && !target) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto* s = static_cast<Surface9*>(target);
    dev->set_render_target(index, s ? s->id() : d3d9::SurfaceId{});
    assign<IDirect3DSurface9>(render_targets_[index], target);
    if (index == 0 && s) viewport_ = D3DVIEWPORT9{0, 0, s->width(), s->height(), 0.0f, 1.0f};   // Direct3D 9 resets the viewport
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetRenderTarget(DWORD index, IDirect3DSurface9** out) try { NUE_CALL(); 
    if (!out || index >= 4 || !render_targets_[index]) return trace_result(__func__, D3DERR_NOTFOUND);
    render_targets_[index]->AddRef();
    *out = render_targets_[index];
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetDepthStencilSurface(IDirect3DSurface9* depth) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_depth_stencil(depth ? static_cast<Surface9*>(depth)->id() : d3d9::SurfaceId{});
    assign<IDirect3DSurface9>(depth_stencil_, depth);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetDepthStencilSurface(IDirect3DSurface9** out) try { NUE_CALL(); 
    if (!out) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!depth_stencil_) { *out = nullptr; return trace_result(__func__, D3DERR_NOTFOUND); }
    depth_stencil_->AddRef();
    *out = depth_stencil_;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

// ---- scene, clear, draw -----------------------------------------------------------------------------------------------------------------

HRESULT Device9::BeginScene() try { NUE_CALL(); auto* dev = core_->dev(); if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL); dev->begin_scene(); in_scene_ = true; return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT Device9::EndScene() try { NUE_CALL(); auto* dev = core_->dev(); if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL); dev->end_scene(); in_scene_ = false; return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->clear(rects && count ? std::span<const int32_t>(reinterpret_cast<const int32_t*>(rects), size_t(count) * 4) : std::span<const int32_t>(), flags, color, z, stencil);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_vertex, UINT vertex_count, UINT start_index, UINT primitive_count) try { NUE_CALL(); ++g_counter_draw; 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->draw_indexed(uint32_t(type), base_vertex, min_vertex, vertex_count, start_index, primitive_count);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitive_count, const void* vertices, UINT stride) try { NUE_CALL(); ++g_counter_draw; 
    auto* dev = core_->dev();
    if (!dev || !vertices || !stride) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->draw_up(uint32_t(type), primitive_count, Bytes(static_cast<const std::byte*>(vertices), size_t(vertices_for(type, primitive_count)) * stride), stride);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

// ---- state ------------------------------------------------------------------------------------------------------------------------------

HRESULT Device9::SetTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !matrix) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!recording_block_) transforms_[DWORD(type)] = *matrix;
    dev->set_transform(DWORD(type), reinterpret_cast<const float*>(matrix));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetTransform(D3DTRANSFORMSTATETYPE type, D3DMATRIX* matrix) try { NUE_CALL(); 
    if (!matrix) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto it = transforms_.find(DWORD(type));
    if (it != transforms_.end()) { *matrix = it->second; return D3D_OK; }
    std::memset(matrix, 0, sizeof(*matrix));
    matrix->_11 = matrix->_22 = matrix->_33 = matrix->_44 = 1.0f;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::MultiplyTransform(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) try { NUE_CALL(); 
    if (!matrix) return trace_result(__func__, D3DERR_INVALIDCALL);
    D3DMATRIX current;
    GetTransform(type, &current);
    D3DMATRIX result{};
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { float sum = 0; for (int k = 0; k < 4; ++k) sum += current.m[r][k] * matrix->m[k][c]; result.m[r][c] = sum; }
    return SetTransform(type, &result);
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetViewport(const D3DVIEWPORT9* viewport) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !viewport) return trace_result(__func__, D3DERR_INVALIDCALL);
    viewport_ = *viewport;
    const uint32_t r[4] = {viewport->X, viewport->Y, viewport->Width, viewport->Height};
    dev->set_viewport(r, viewport->MinZ, viewport->MaxZ);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetViewport(D3DVIEWPORT9* viewport) try { NUE_CALL(); if (!viewport) return trace_result(__func__, D3DERR_INVALIDCALL); *viewport = viewport_; return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetLight(DWORD index, const D3DLIGHT9* light) try { NUE_CALL(); 
    if (!light) return trace_result(__func__, D3DERR_INVALIDCALL);
    lights_[index].first = *light;
    return D3D_OK;   // lights are stored; the layer draws fixed-function geometry unlit (see the coverage table)
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetLight(DWORD index, D3DLIGHT9* light) try { NUE_CALL(); 
    auto it = lights_.find(index);
    if (!light || it == lights_.end()) return trace_result(__func__, D3DERR_INVALIDCALL);
    *light = it->second.first;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::LightEnable(DWORD index, BOOL enable) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    lights_[index].second = enable;
    dev->light_enable(index, enable != FALSE);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetLightEnable(DWORD index, BOOL* enable) try { NUE_CALL(); 
    auto it = lights_.find(index);
    if (!enable || it == lights_.end()) return trace_result(__func__, D3DERR_INVALIDCALL);
    *enable = it->second.second;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!recording_block_ && UINT(state) < 256) render_state_[state] = value;
    dev->set_render_state(uint32_t(state), value);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetRenderState(D3DRENDERSTATETYPE state, DWORD* value) try { NUE_CALL(); 
    if (!value || UINT(state) >= 256) return trace_result(__func__, D3DERR_INVALIDCALL);
    *value = render_state_[state];
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (sampler >= 16 || UINT(type) >= 14) return D3D_OK;   // displacement-map and vertex-texture samplers
    if (!recording_block_) sampler_state_[sampler][type] = value;
    dev->set_sampler_state(sampler, uint32_t(type), value);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) try { NUE_CALL(); 
    if (!value || sampler >= 16 || UINT(type) >= 14) return trace_result(__func__, D3DERR_INVALIDCALL);
    *value = sampler_state_[sampler][type];
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (stage >= 8 || UINT(type) >= 33) return D3D_OK;
    if (!recording_block_) stage_state_[stage][type] = value;
    dev->set_texture_stage_state(stage, uint32_t(type), value);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) try { NUE_CALL(); 
    if (!value || stage >= 8 || UINT(type) >= 33) return trace_result(__func__, D3DERR_INVALIDCALL);
    *value = stage_state_[stage][type];
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (stage >= 16) return D3D_OK;
    dev->set_texture(stage, texture_id_of(texture));
    if (!recording_block_) assign<IDirect3DBaseTexture9>(textures_[stage], texture);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetTexture(DWORD stage, IDirect3DBaseTexture9** out) try { NUE_CALL(); 
    if (!out || stage >= 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    *out = textures_[stage];
    if (*out) (*out)->AddRef();
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetVertexDeclaration(IDirect3DVertexDeclaration9* declaration) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_vertex_declaration(declaration ? static_cast<VertexDeclaration9*>(declaration)->id() : d3d9::DeclarationId{});
    if (!recording_block_) { assign<IDirect3DVertexDeclaration9>(declaration_, declaration); fvf_ = 0; }
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetVertexDeclaration(IDirect3DVertexDeclaration9** out) try { NUE_CALL(); 
    if (!out) return trace_result(__func__, D3DERR_INVALIDCALL);
    *out = declaration_;
    if (*out) (*out)->AddRef();
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetFVF(DWORD fvf) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_fvf(fvf);
    if (!recording_block_) { fvf_ = fvf; assign<IDirect3DVertexDeclaration9>(declaration_, nullptr); }
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetVertexShader(IDirect3DVertexShader9* shader) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_vertex_shader(shader ? static_cast<VertexShader9*>(shader)->id() : d3d9::ShaderId{});
    if (!recording_block_) assign<IDirect3DVertexShader9>(vertex_shader_, shader);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetVertexShader(IDirect3DVertexShader9** out) try { NUE_CALL(); if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = vertex_shader_; if (*out) (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetPixelShader(IDirect3DPixelShader9* shader) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_pixel_shader(shader ? static_cast<PixelShader9*>(shader)->id() : d3d9::ShaderId{});
    if (!recording_block_) assign<IDirect3DPixelShader9>(pixel_shader_, shader);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetPixelShader(IDirect3DPixelShader9** out) try { NUE_CALL(); if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = pixel_shader_; if (*out) (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetVertexShaderConstantF(UINT start, const float* data, UINT count) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !data || start + count > 256) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!recording_block_) std::memcpy(&vs_f_[size_t(start) * 4], data, size_t(count) * 16);
    dev->set_vertex_shader_constants_f(start, std::span<const d3d9::Float4>(reinterpret_cast<const d3d9::Float4*>(data), count));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetVertexShaderConstantF(UINT start, float* data, UINT count) try { NUE_CALL(); if (!data || start + count > 256) return trace_result(__func__, D3DERR_INVALIDCALL); std::memcpy(data, &vs_f_[size_t(start) * 4], size_t(count) * 16); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetVertexShaderConstantI(UINT start, const int* data, UINT count) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!recording_block_) std::memcpy(vs_i_[start], data, size_t(count) * 16);
    dev->set_vertex_shader_constants_i(start, std::span<const d3d9::Int4>(reinterpret_cast<const d3d9::Int4*>(data), count));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetVertexShaderConstantI(UINT start, int* data, UINT count) try { NUE_CALL(); if (!data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL); std::memcpy(data, vs_i_[start], size_t(count) * 16); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetVertexShaderConstantB(UINT start, const BOOL* data, UINT count) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    uint32_t values[16];
    for (UINT i = 0; i < count; ++i) { values[i] = data[i] ? 1u : 0u; if (!recording_block_) vs_b_[start + i] = data[i]; }
    dev->set_vertex_shader_constants_b(start, std::span<const uint32_t>(values, count));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetVertexShaderConstantB(UINT start, BOOL* data, UINT count) try { NUE_CALL(); if (!data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL); std::memcpy(data, &vs_b_[start], size_t(count) * sizeof(BOOL)); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetPixelShaderConstantF(UINT start, const float* data, UINT count) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !data || start + count > 224) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!recording_block_) std::memcpy(&ps_f_[size_t(start) * 4], data, size_t(count) * 16);
    dev->set_pixel_shader_constants_f(start, std::span<const d3d9::Float4>(reinterpret_cast<const d3d9::Float4*>(data), count));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetPixelShaderConstantF(UINT start, float* data, UINT count) try { NUE_CALL(); if (!data || start + count > 224) return trace_result(__func__, D3DERR_INVALIDCALL); std::memcpy(data, &ps_f_[size_t(start) * 4], size_t(count) * 16); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetPixelShaderConstantI(UINT start, const int* data, UINT count) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!recording_block_) std::memcpy(ps_i_[start], data, size_t(count) * 16);
    dev->set_pixel_shader_constants_i(start, std::span<const d3d9::Int4>(reinterpret_cast<const d3d9::Int4*>(data), count));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetPixelShaderConstantI(UINT start, int* data, UINT count) try { NUE_CALL(); if (!data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL); std::memcpy(data, ps_i_[start], size_t(count) * 16); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetPixelShaderConstantB(UINT start, const BOOL* data, UINT count) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    uint32_t values[16];
    for (UINT i = 0; i < count; ++i) { values[i] = data[i] ? 1u : 0u; if (!recording_block_) ps_b_[start + i] = data[i]; }
    dev->set_pixel_shader_constants_b(start, std::span<const uint32_t>(values, count));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT Device9::GetPixelShaderConstantB(UINT start, BOOL* data, UINT count) try { NUE_CALL(); if (!data || start + count > 16) return trace_result(__func__, D3DERR_INVALIDCALL); std::memcpy(data, &ps_b_[start], size_t(count) * sizeof(BOOL)); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetStreamSource(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || stream >= 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_stream_source(stream, buffer ? static_cast<VertexBuffer9*>(buffer)->id() : d3d9::BufferId{}, offset, stride);
    if (!recording_block_) { assign<IDirect3DVertexBuffer9>(streams_[stream], buffer); stream_offset_[stream] = offset; stream_stride_[stream] = stride; }
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetStreamSource(UINT stream, IDirect3DVertexBuffer9** out, UINT* offset, UINT* stride) try { NUE_CALL(); 
    if (!out || stream >= 16) return trace_result(__func__, D3DERR_INVALIDCALL);
    *out = streams_[stream];
    if (*out) (*out)->AddRef();
    if (offset) *offset = stream_offset_[stream];
    if (stride) *stride = stream_stride_[stream];
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::SetIndices(IDirect3DIndexBuffer9* buffer) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->set_indices(buffer ? static_cast<IndexBuffer9*>(buffer)->id() : d3d9::BufferId{});
    if (!recording_block_) assign<IDirect3DIndexBuffer9>(indices_, buffer);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::GetIndices(IDirect3DIndexBuffer9** out) try { NUE_CALL(); if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = indices_; if (*out) (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }

// ---- state blocks -----------------------------------------------------------------------------------------------------------------------

HRESULT Device9::BeginStateBlock() try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || recording_block_) return trace_result(__func__, D3DERR_INVALIDCALL);
    recording_id_ = dev->begin_state_block();
    recording_block_ = true;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Device9::EndStateBlock(IDirect3DStateBlock9** out) try { NUE_CALL(); 
    auto* dev = core_->dev();
    if (!dev || !out || !recording_block_) return trace_result(__func__, D3DERR_INVALIDCALL);
    dev->end_state_block(recording_id_);
    recording_block_ = false;
    *out = new StateBlock9(child(), recording_id_);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

// A block created with CreateStateBlock captures the current state: replayed from the shadow into a recorded block (all three types capture the same
// set; the game uses Begin/End, which is exact).
HRESULT Device9::CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9** out) try { NUE_CALL(); ++g_counter_create; 
    auto* dev = core_->dev();
    if (!dev || !out || recording_block_) return trace_result(__func__, D3DERR_INVALIDCALL);
    const d3d9::StateBlockId id = dev->begin_state_block();
    for (UINT i = 0; i < 256; ++i) if (render_state_[i] || i == D3DRS_ZENABLE) dev->set_render_state(i, render_state_[i]);
    for (UINT s = 0; s < 16; ++s) for (UINT t = 1; t < 14; ++t) dev->set_sampler_state(s, t, sampler_state_[s][t]);
    for (UINT s = 0; s < 8; ++s) for (UINT t = 1; t < 33; ++t) dev->set_texture_stage_state(s, t, stage_state_[s][t]);
    dev->end_state_block(id);
    *out = new StateBlock9(child(), id);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

}  // namespace nue::facade
