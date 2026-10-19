// A d3d9::Device that forwards every call to the REAL Direct3D 9 runtime (Windows only, tool use): the ground truth against which the layer's output is
// compared draw by draw. It replays the same trace the layer replays, so a difference is the layer's, not the trace's.
#include "native_d3d9.hpp"
#include <d3d9.h>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <float.h>
#include <xmmintrin.h>
#include <cstring>
#include <vector>

namespace nue::d3d9 {
namespace {

template <class T> struct Pool {
    std::vector<T*> items{nullptr};
    uint32_t add(T* p) { items.push_back(p); return uint32_t(items.size() - 1); }
    T* get(uint32_t id) const { return id && id < items.size() ? items[id] : nullptr; }
    void release(uint32_t id) { if (id && id < items.size() && items[id]) { items[id]->Release(); items[id] = nullptr; } }
};

class NativeDevice final : public Device, public NativeCapture {
public:
    ~NativeDevice() override {
        if (device_) device_->Release();
        if (d3d_) d3d_->Release();
    }
    bool init(void* hwnd, std::string& error) {
        hwnd_ = static_cast<HWND>(hwnd);
        if (const char* dll = std::getenv("NUE_D3D9_DLL")) {   // test the drop-in d3d9.dll (nue_d3d9_dll) instead of the system runtime: same trace, through its COM objects
            HMODULE module = LoadLibraryA(dll);
            using Create = IDirect3D9*(WINAPI*)(UINT);
            auto create = module ? reinterpret_cast<Create>(GetProcAddress(module, "Direct3DCreate9")) : nullptr;
            if (!create) { error = std::string("cannot load Direct3DCreate9 from ") + dll; return false; }
            d3d_ = create(D3D_SDK_VERSION);
        } else d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d_) { error = "Direct3DCreate9 failed"; return false; }
        return true;
    }

    Status reset(const DeviceParams& p) override {
        D3DPRESENT_PARAMETERS pp{};
        pp.BackBufferWidth = p.back_width; pp.BackBufferHeight = p.back_height;
        pp.BackBufferFormat = D3DFORMAT(p.back_format ? p.back_format : D3DFMT_X8R8G8B8);
        pp.BackBufferCount = 1;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hwnd_;
        pp.Windowed = TRUE;
        pp.EnableAutoDepthStencil = p.depth_format ? TRUE : FALSE;
        pp.AutoDepthStencilFormat = D3DFORMAT(p.depth_format ? p.depth_format : D3DFMT_D24S8);
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        width_ = p.back_width; height_ = p.back_height;
        const bool fpu_report = std::getenv("NUE_FPU_REPORT") != nullptr;   // x87 control word and MXCSR around CreateDevice (a library that unmasks floating point exceptions crashes the game later)
        auto report = [&](const char* when) { if (fpu_report) std::fprintf(stderr, "FPU %s: x87 control %04x, MXCSR %08x\n", when, _control87(0, 0) & 0xFFFF, _mm_getcsr()); };
        report("before CreateDevice");
        if (!device_) {
            HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device_);
            if (FAILED(hr)) hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd_, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device_);
            report("after CreateDevice");
            return SUCCEEDED(hr) ? Status::ok : Status::unsupported;
        }
        return SUCCEEDED(device_->Reset(&pp)) ? Status::ok : Status::device_lost;
    }
    Status test_cooperative_level() override { return SUCCEEDED(device_->TestCooperativeLevel()) ? Status::ok : Status::device_lost; }
    Status present() override {
        device_->EndScene(); in_scene_ = false;
        const HRESULT hr = device_->Present(nullptr, nullptr, nullptr, nullptr);
        static unsigned frames = 0;
        if (std::getenv("NUE_FPU_REPORT") && (++frames == 1 || frames == 2 || frames == 30 || frames == 100)) std::fprintf(stderr, "FPU after present %u: x87 control %04x, MXCSR %08x\n", frames, _control87(0, 0) & 0xFFFF, _mm_getcsr());
        return SUCCEEDED(hr) ? Status::ok : Status::device_lost;
    }

    Result<TextureId> create_texture(uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(device_->CreateTexture(w, h, levels, usage, D3DFORMAT(format), D3DPOOL(pool), &t, nullptr))) return {Status::unsupported, {}};
        const uint32_t id = textures_.add(t);
        if (id < cube_.size()) cube_[id] = false;
        return {Status::ok, TextureId{id}};
    }
    Result<TextureId> create_cube_texture(uint32_t edge, uint32_t levels, uint32_t usage, uint32_t format, uint32_t pool) override {
        IDirect3DCubeTexture9* t = nullptr;
        if (FAILED(device_->CreateCubeTexture(edge, levels, usage, D3DFORMAT(format), D3DPOOL(pool), &t, nullptr))) return {Status::unsupported, {}};
        const uint32_t id = textures_.add(t);
        if (cube_.size() <= id) cube_.resize(id + 1, false);
        cube_[id] = true;
        return {Status::ok, TextureId{id}};
    }
    Result<BufferId> create_vertex_buffer(uint32_t bytes, uint32_t usage, uint32_t fvf, uint32_t pool) override {
        IDirect3DVertexBuffer9* b = nullptr;
        if (FAILED(device_->CreateVertexBuffer(bytes, usage, fvf, D3DPOOL(pool), &b, nullptr))) return {Status::unsupported, {}};
        return {Status::ok, BufferId{vbs_.add(b)}};
    }
    Result<BufferId> create_index_buffer(uint32_t bytes, uint32_t usage, uint32_t format, uint32_t pool) override {
        IDirect3DIndexBuffer9* b = nullptr;
        if (FAILED(device_->CreateIndexBuffer(bytes, usage, D3DFORMAT(format), D3DPOOL(pool), &b, nullptr))) return {Status::unsupported, {}};
        // buffer ids of the two kinds share a number space in the trace: keep them apart by using separate tables and tagging the id
        return {Status::ok, BufferId{ibs_.add(b) | 0x80000000u}};
    }
    Result<SurfaceId> create_render_target(uint32_t w, uint32_t h, uint32_t format, uint32_t ms, bool lockable) override {
        (void)ms;
        IDirect3DSurface9* s = nullptr;
        if (FAILED(device_->CreateRenderTarget(w, h, D3DFORMAT(format), D3DMULTISAMPLE_NONE, 0, lockable, &s, nullptr))) return {Status::unsupported, {}};
        return {Status::ok, SurfaceId{surfaces_.add(s)}};
    }
    Result<SurfaceId> create_depth_stencil(uint32_t w, uint32_t h, uint32_t format, uint32_t ms) override {
        (void)ms;
        IDirect3DSurface9* s = nullptr;
        if (FAILED(device_->CreateDepthStencilSurface(w, h, D3DFORMAT(format), D3DMULTISAMPLE_NONE, 0, FALSE, &s, nullptr))) return {Status::unsupported, {}};
        return {Status::ok, SurfaceId{surfaces_.add(s)}};
    }
    Result<DeclarationId> create_vertex_declaration(std::span<const uint8_t> e) override {
        IDirect3DVertexDeclaration9* d = nullptr;
        if (FAILED(device_->CreateVertexDeclaration(reinterpret_cast<const D3DVERTEXELEMENT9*>(e.data()), &d))) return {Status::unsupported, {}};
        return {Status::ok, DeclarationId{decls_.add(d)}};
    }
    Result<ShaderId> create_vertex_shader(std::span<const uint32_t> code) override {
        IDirect3DVertexShader9* s = nullptr;
        if (FAILED(device_->CreateVertexShader(reinterpret_cast<const DWORD*>(code.data()), &s))) return {Status::unsupported, {}};
        return {Status::ok, ShaderId{vss_.add(s)}};
    }
    Result<ShaderId> create_pixel_shader(std::span<const uint32_t> code) override {
        IDirect3DPixelShader9* s = nullptr;
        if (FAILED(device_->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()), &s))) return {Status::unsupported, {}};
        return {Status::ok, ShaderId{pss_.add(s) | 0x80000000u}};
    }
    void destroy(TextureId id) override { textures_.release(id.value); }
    void destroy(BufferId id) override { if (id.value & 0x80000000u) ibs_.release(id.value & 0x7FFFFFFFu); else vbs_.release(id.value); }
    void destroy(SurfaceId id) override { surfaces_.release(id.value); }
    void destroy(DeclarationId id) override { decls_.release(id.value); }
    void destroy(ShaderId id) override { if (id.value & 0x80000000u) pss_.release(id.value & 0x7FFFFFFFu); else vss_.release(id.value); }
    void destroy(StateBlockId id) override { blocks_.release(id.value); }

    Result<LockedRegion> lock_buffer(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags) override {
        void* data = nullptr;
        HRESULT hr;
        if (id.value & 0x80000000u) { auto* b = ibs_.get(id.value & 0x7FFFFFFFu); if (!b) return {Status::invalid_argument, {}}; hr = b->Lock(offset, bytes, &data, flags); }
        else { auto* b = vbs_.get(id.value); if (!b) return {Status::invalid_argument, {}}; hr = b->Lock(offset, bytes, &data, flags); }
        if (FAILED(hr)) return {Status::unsupported, {}};
        return {Status::ok, LockedRegion{static_cast<std::byte*>(data), bytes, 0}};
    }
    Status unlock_buffer(BufferId id) override {
        if (id.value & 0x80000000u) { if (auto* b = ibs_.get(id.value & 0x7FFFFFFFu)) b->Unlock(); }
        else if (auto* b = vbs_.get(id.value)) b->Unlock();
        return Status::ok;
    }
    Result<LockedRegion> lock_texture(TextureId id, uint32_t level, uint32_t face, const int32_t* rect4, uint32_t flags) override {
        IDirect3DBaseTexture9* base = textures_.get(id.value);
        if (!base) return {Status::invalid_argument, {}};
        RECT r{}; if (rect4) { r.left = rect4[0]; r.top = rect4[1]; r.right = rect4[2]; r.bottom = rect4[3]; }
        D3DLOCKED_RECT lr{};
        HRESULT hr;
        if (is_cube(id.value)) hr = static_cast<IDirect3DCubeTexture9*>(base)->LockRect(D3DCUBEMAP_FACES(face), level, &lr, rect4 ? &r : nullptr, flags);
        else hr = static_cast<IDirect3DTexture9*>(base)->LockRect(level, &lr, rect4 ? &r : nullptr, flags);
        if (FAILED(hr)) return {Status::unsupported, {}};
        return {Status::ok, LockedRegion{static_cast<std::byte*>(lr.pBits), uint32_t(lr.Pitch), 0}};
    }
    Status unlock_texture(TextureId id, uint32_t level, uint32_t face) override {
        IDirect3DBaseTexture9* base = textures_.get(id.value);
        if (!base) return Status::invalid_argument;
        if (is_cube(id.value)) static_cast<IDirect3DCubeTexture9*>(base)->UnlockRect(D3DCUBEMAP_FACES(face), level);
        else static_cast<IDirect3DTexture9*>(base)->UnlockRect(level);
        return Status::ok;
    }
    Result<SurfaceId> texture_surface(TextureId id, uint32_t level, uint32_t face) override {
        IDirect3DBaseTexture9* base = textures_.get(id.value);
        if (!base) return {Status::invalid_argument, {}};
        IDirect3DSurface9* s = nullptr;
        HRESULT hr = is_cube(id.value) ? static_cast<IDirect3DCubeTexture9*>(base)->GetCubeMapSurface(D3DCUBEMAP_FACES(face), level, &s) : static_cast<IDirect3DTexture9*>(base)->GetSurfaceLevel(level, &s);
        if (FAILED(hr)) return {Status::unsupported, {}};
        return {Status::ok, SurfaceId{surfaces_.add(s)}};
    }
    Result<SurfaceId> back_buffer(uint32_t chain, uint32_t index) override {
        IDirect3DSurface9* s = nullptr;
        if (FAILED(device_->GetBackBuffer(chain, index, D3DBACKBUFFER_TYPE_MONO, &s))) return {Status::unsupported, {}};
        return {Status::ok, SurfaceId{surfaces_.add(s)}};
    }
    Status stretch_rect(SurfaceId from, const int32_t* fr, SurfaceId to, const int32_t* tr, uint32_t filter) override {
        RECT a{}, b{};
        if (fr) { a.left = fr[0]; a.top = fr[1]; a.right = fr[2]; a.bottom = fr[3]; }
        if (tr) { b.left = tr[0]; b.top = tr[1]; b.right = tr[2]; b.bottom = tr[3]; }
        return SUCCEEDED(device_->StretchRect(surfaces_.get(from.value), fr ? &a : nullptr, surfaces_.get(to.value), tr ? &b : nullptr, D3DTEXTUREFILTERTYPE(filter))) ? Status::ok : Status::unsupported;
    }

    void set_render_state(uint32_t s, uint32_t v) override { device_->SetRenderState(D3DRENDERSTATETYPE(s), v); }
    void set_sampler_state(uint32_t s, uint32_t t, uint32_t v) override { device_->SetSamplerState(s, D3DSAMPLERSTATETYPE(t), v); }
    void set_texture_stage_state(uint32_t s, uint32_t t, uint32_t v) override { device_->SetTextureStageState(s, D3DTEXTURESTAGESTATETYPE(t), v); }
    void set_transform(uint32_t t, const float* m) override { device_->SetTransform(D3DTRANSFORMSTATETYPE(t), reinterpret_cast<const D3DMATRIX*>(m)); }
    void light_enable(uint32_t i, bool e) override { device_->LightEnable(i, e); }
    void set_viewport(const uint32_t* r, float a, float b) override { D3DVIEWPORT9 v{r[0], r[1], r[2], r[3], a, b}; device_->SetViewport(&v); }
    void set_texture(uint32_t s, TextureId id) override { device_->SetTexture(s, textures_.get(id.value)); }
    void set_render_target(uint32_t i, SurfaceId id) override { device_->SetRenderTarget(i, surfaces_.get(id.value)); }
    void set_depth_stencil(SurfaceId id) override { device_->SetDepthStencilSurface(surfaces_.get(id.value)); }
    void set_vertex_declaration(DeclarationId id) override { device_->SetVertexDeclaration(decls_.get(id.value)); }
    void set_fvf(uint32_t f) override { device_->SetFVF(f); }
    void set_vertex_shader(ShaderId id) override { device_->SetVertexShader(vss_.get(id.value)); }
    void set_pixel_shader(ShaderId id) override { device_->SetPixelShader(pss_.get(id.value & 0x7FFFFFFFu)); }
    void set_vertex_shader_constants_f(uint32_t start, std::span<const Float4> v) override { device_->SetVertexShaderConstantF(start, &v[0].x, UINT(v.size())); }
    void set_pixel_shader_constants_f(uint32_t start, std::span<const Float4> v) override { device_->SetPixelShaderConstantF(start, &v[0].x, UINT(v.size())); }
    void set_vertex_shader_constants_i(uint32_t start, std::span<const Int4> v) override { device_->SetVertexShaderConstantI(start, &v[0].x, UINT(v.size())); }
    void set_pixel_shader_constants_i(uint32_t start, std::span<const Int4> v) override { device_->SetPixelShaderConstantI(start, &v[0].x, UINT(v.size())); }
    void set_vertex_shader_constants_b(uint32_t start, std::span<const uint32_t> v) override { std::vector<BOOL> b(v.begin(), v.end()); device_->SetVertexShaderConstantB(start, b.data(), UINT(b.size())); }
    void set_pixel_shader_constants_b(uint32_t start, std::span<const uint32_t> v) override { std::vector<BOOL> b(v.begin(), v.end()); device_->SetPixelShaderConstantB(start, b.data(), UINT(b.size())); }
    void set_stream_source(uint32_t s, BufferId id, uint32_t off, uint32_t stride) override { device_->SetStreamSource(s, vbs_.get(id.value), off, stride); }
    void set_indices(BufferId id) override { device_->SetIndices(id.value ? ibs_.get(id.value & 0x7FFFFFFFu) : nullptr); }
    StateBlockId begin_state_block() override { device_->BeginStateBlock(); return StateBlockId{blocks_.add(nullptr)}; }
    void end_state_block(StateBlockId id) override { IDirect3DStateBlock9* sb = nullptr; if (SUCCEEDED(device_->EndStateBlock(&sb)) && id.value < blocks_.items.size()) blocks_.items[id.value] = sb; }
    void apply(StateBlockId id) override { if (auto* b = blocks_.get(id.value)) b->Apply(); }
    void begin_scene() override { if (!in_scene_) { device_->BeginScene(); in_scene_ = true; } }
    void end_scene() override {}
    void clear(std::span<const int32_t> rects, uint32_t flags, uint32_t argb, float z, uint32_t stencil) override {
        std::vector<D3DRECT> list;
        for (size_t i = 0; i + 4 <= rects.size(); i += 4) list.push_back(D3DRECT{rects[i], rects[i + 1], rects[i + 2], rects[i + 3]});
        begin_scene();
        device_->Clear(DWORD(list.size()), list.empty() ? nullptr : list.data(), flags, argb, z, stencil);
    }
    void draw_indexed(uint32_t prim, int32_t base, uint32_t min_v, uint32_t count, uint32_t start, uint32_t prims) override {
        if ((draw_count_++ < first_ || draw_count_ - 1 > last_) && draw_count_ - 1 != also_) return;
        begin_scene();
        device_->DrawIndexedPrimitive(D3DPRIMITIVETYPE(prim), base, min_v, count, start, prims);
    }
    void draw_up(uint32_t prim, uint32_t prims, Bytes v, uint32_t stride) override {
        if ((draw_count_++ < first_ || draw_count_ - 1 > last_) && draw_count_ - 1 != also_) return;
        begin_scene();
        device_->DrawPrimitiveUP(D3DPRIMITIVETYPE(prim), prims, v.data(), stride);
    }

    // ---- NativeCapture
    void set_draw_range(uint64_t first, uint64_t last, uint64_t also) override { first_ = first; last_ = last; also_ = also; }
    bool capture(std::vector<uint8_t>& bgra, uint32_t& w, uint32_t& h) override {
        IDirect3DSurface9 *back = nullptr, *sys = nullptr;
        if (FAILED(device_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return false;
        D3DSURFACE_DESC d; back->GetDesc(&d);
        bool ok = SUCCEEDED(device_->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && SUCCEEDED(device_->GetRenderTargetData(back, sys));
        if (ok) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                w = d.Width; h = d.Height;
                bgra.resize(size_t(w) * h * 4);
                for (uint32_t y = 0; y < h; ++y) std::memcpy(&bgra[size_t(y) * w * 4], static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(w) * 4);
                for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
                sys->UnlockRect();
            } else ok = false;
        }
        if (sys) sys->Release();
        back->Release();
        return ok;
    }
    Device& device() override { return *this; }

private:
    bool is_cube(uint32_t id) const { return id < cube_.size() && cube_[id]; }
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* device_ = nullptr;
    HWND hwnd_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    bool in_scene_ = false;
    uint64_t draw_count_ = 0, first_ = 0, last_ = ~0ull, also_ = ~0ull;
    Pool<IDirect3DBaseTexture9> textures_;
    Pool<IDirect3DVertexBuffer9> vbs_;
    Pool<IDirect3DIndexBuffer9> ibs_;
    Pool<IDirect3DSurface9> surfaces_;
    Pool<IDirect3DVertexDeclaration9> decls_;
    Pool<IDirect3DVertexShader9> vss_;
    Pool<IDirect3DPixelShader9> pss_;
    Pool<IDirect3DStateBlock9> blocks_;
    std::vector<bool> cube_;
};

}  // namespace

std::unique_ptr<NativeCapture> create_native_d3d9(void* hwnd, std::string& error) {
    auto d = std::make_unique<NativeDevice>();
    if (!d->init(hwnd, error)) return nullptr;
    return d;
}

}  // namespace nue::d3d9
