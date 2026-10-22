// The resource objects of the facade: textures, surfaces, buffers, declarations, shaders, state blocks, swap chain.
#include "facade.hpp"
#include <algorithm>
#include <cstring>

namespace nue::facade {

HRESULT to_hresult(Status status) {
    if (status != Status::ok) trace_result(g_last_call ? g_last_call : "?", status == Status::unsupported ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL);
    switch (status) {
    case Status::ok: return D3D_OK;
    case Status::unsupported: return D3DERR_NOTAVAILABLE;
    case Status::out_of_memory: return D3DERR_OUTOFVIDEOMEMORY;
    case Status::device_lost: return D3DERR_DEVICELOST;
    default: return D3DERR_INVALIDCALL;
    }
}

HRESULT RefCounted::query_interface_impl(IUnknown* self, REFIID riid, void** out, std::initializer_list<const IID*> ids) {
    if (!out) return E_POINTER;
    for (const IID* id : ids) {
        if (IsEqualGUID(riid, *id)) { *out = self; self->AddRef(); return S_OK; }
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

namespace {

uint32_t bytes_per_pixel(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8L8: case D3DFMT_R16F: case D3DFMT_L16: return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: return 1;
    case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_A16B16G16R16: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    default: return 4;
    }
}

void fill_desc(D3DSURFACE_DESC* d, D3DFORMAT format, D3DRESOURCETYPE type, DWORD usage, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms, UINT w, UINT h) {
    d->Format = format; d->Type = type; d->Usage = usage; d->Pool = pool; d->MultiSampleType = ms; d->MultiSampleQuality = 0; d->Width = w; d->Height = h;
}

UINT level_size(UINT size, UINT level) { return std::max(1u, size >> level); }

}  // namespace

// ---- Texture9 ----------------------------------------------------------------------------------------------------------------------------

Texture9::Texture9(ChildInfo i, d3d9::TextureId id, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : info(std::move(i)), id_(id), width_(w), height_(h), levels_(levels ? levels : 1), usage_(usage), format_(format), pool_(pool) {
    if (!levels) { UINT m = std::max(w, h); levels_ = 1; while (m > 1) { m >>= 1; ++levels_; } }
    surfaces_.assign(levels_, nullptr);
}

Texture9::~Texture9() {
    for (IDirect3DSurface9* s : surfaces_) delete static_cast<Surface9*>(s);
    if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); }
}

HRESULT Texture9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Texture9::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) try {
    if (!desc || level >= levels_) return trace_result(__func__, D3DERR_INVALIDCALL);
    fill_desc(desc, format_, D3DRTYPE_SURFACE, usage_, pool_, D3DMULTISAMPLE_NONE, level_size(width_, level), level_size(height_, level));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Texture9::GetSurfaceLevel(UINT level, IDirect3DSurface9** out) try {
    if (!out || level >= levels_) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto* dev = info.core->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (!surfaces_[level]) {
        auto r = dev->texture_surface(id_, level, 0);
        if (!r) return to_hresult(r.status);
        auto* s = new Surface9(info, Surface9::Kind::texture_level, r.value, level_size(width_, level), level_size(height_, level), format_, D3DMULTISAMPLE_NONE);
        s->set_container(static_cast<IDirect3DTexture9*>(this), id_, level, 0, usage_, pool_);
        surfaces_[level] = s;
    }
    surfaces_[level]->AddRef();   // forwarded to this texture
    *out = surfaces_[level];
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Texture9::LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) try {
    auto* dev = info.core->dev();
    if (!dev || !locked || level >= levels_) return trace_result(__func__, D3DERR_INVALIDCALL);
    const int32_t r[4] = {rect ? rect->left : 0, rect ? rect->top : 0, rect ? rect->right : 0, rect ? rect->bottom : 0};
    auto lock = dev->lock_texture(id_, level, 0, rect ? r : nullptr, flags);
    if (!lock) return lock.status == Status::unsupported ? D3DERR_INVALIDCALL : to_hresult(lock.status);
    locked->pBits = lock.value.data; locked->Pitch = INT(lock.value.pitch);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Texture9::UnlockRect(UINT level) try {
    auto* dev = info.core->dev();
    if (!dev || level >= levels_) return trace_result(__func__, D3DERR_INVALIDCALL);
    return to_hresult(dev->unlock_texture(id_, level, 0));
} catch (...) { return guard_failed(__func__); }

// ---- CubeTexture9 ------------------------------------------------------------------------------------------------------------------------

CubeTexture9::CubeTexture9(ChildInfo i, d3d9::TextureId id, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool)
    : info(std::move(i)), id_(id), edge_(edge), levels_(levels ? levels : 1), usage_(usage), format_(format), pool_(pool) {
    if (!levels) { UINT m = edge; levels_ = 1; while (m > 1) { m >>= 1; ++levels_; } }
    surfaces_.assign(size_t(levels_) * 6, nullptr);
}

CubeTexture9::~CubeTexture9() {
    for (IDirect3DSurface9* s : surfaces_) delete static_cast<Surface9*>(s);
    if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); }
}

HRESULT CubeTexture9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT CubeTexture9::GetLevelDesc(UINT level, D3DSURFACE_DESC* desc) try {
    if (!desc || level >= levels_) return trace_result(__func__, D3DERR_INVALIDCALL);
    fill_desc(desc, format_, D3DRTYPE_SURFACE, usage_, pool_, D3DMULTISAMPLE_NONE, level_size(edge_, level), level_size(edge_, level));
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT CubeTexture9::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** out) try {
    if (!out || level >= levels_ || UINT(face) >= 6) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto* dev = info.core->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    IDirect3DSurface9*& slot = surfaces_[size_t(face) * levels_ + level];
    if (!slot) {
        auto r = dev->texture_surface(id_, level, UINT(face));
        if (!r) return to_hresult(r.status);
        auto* s = new Surface9(info, Surface9::Kind::texture_level, r.value, level_size(edge_, level), level_size(edge_, level), format_, D3DMULTISAMPLE_NONE);
        s->set_container(static_cast<IDirect3DCubeTexture9*>(this), id_, level, UINT(face), usage_, pool_);
        slot = s;
    }
    slot->AddRef();
    *out = slot;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT CubeTexture9::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) try {
    auto* dev = info.core->dev();
    if (!dev || !locked || level >= levels_ || UINT(face) >= 6) return trace_result(__func__, D3DERR_INVALIDCALL);
    const int32_t r[4] = {rect ? rect->left : 0, rect ? rect->top : 0, rect ? rect->right : 0, rect ? rect->bottom : 0};
    auto lock = dev->lock_texture(id_, level, UINT(face), rect ? r : nullptr, flags);
    if (!lock) return trace_result(__func__, D3DERR_INVALIDCALL);
    locked->pBits = lock.value.data; locked->Pitch = INT(lock.value.pitch);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT CubeTexture9::UnlockRect(D3DCUBEMAP_FACES face, UINT level) try {
    auto* dev = info.core->dev();
    if (!dev || level >= levels_ || UINT(face) >= 6) return trace_result(__func__, D3DERR_INVALIDCALL);
    return to_hresult(dev->unlock_texture(id_, level, UINT(face)));
} catch (...) { return guard_failed(__func__); }

// ---- Surface9 ----------------------------------------------------------------------------------------------------------------------------

Surface9::Surface9(ChildInfo i, Kind kind, d3d9::SurfaceId id, UINT w, UINT h, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms)
    : info(std::move(i)), kind_(kind), id_(id), width_(w), height_(h), format_(format), multisample_(ms) {
    if (kind == Kind::system_memory) memory_.assign(size_t(w) * h * bytes_per_pixel(format), 0);
}

Surface9::~Surface9() {
    if (kind_ == Kind::back_buffer) return;   // the layer owns it
    if (id_) if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); }
}

ULONG Surface9::release() {
    if (container_) return container_->Release();
    const ULONG r = --refs_;
    if (!r) delete this;
    return r;
}

HRESULT Surface9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }

HRESULT Surface9::GetContainer(REFIID riid, void** out) try {
    if (!out) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (container_) return container_->QueryInterface(riid, out);
    return E_NOINTERFACE;
} catch (...) { return guard_failed(__func__); }

HRESULT Surface9::GetDesc(D3DSURFACE_DESC* desc) try {
    if (!desc) return trace_result(__func__, D3DERR_INVALIDCALL);
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    if (kind_ == Kind::render_target || kind_ == Kind::back_buffer) usage = D3DUSAGE_RENDERTARGET;
    if (kind_ == Kind::depth_stencil) usage = D3DUSAGE_DEPTHSTENCIL;
    if (kind_ == Kind::system_memory) pool = D3DPOOL_SYSTEMMEM;
    if (kind_ == Kind::texture_level) { usage = texture_usage_; pool = texture_pool_; }
    fill_desc(desc, format_, D3DRTYPE_SURFACE, usage, pool, multisample_, width_, height_);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Surface9::LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) try {
    if (!locked) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (kind_ == Kind::system_memory) {
        const UINT bpp = bytes_per_pixel(format_), pitch = width_ * bpp;
        const UINT x = rect ? UINT(rect->left) : 0, y = rect ? UINT(rect->top) : 0;
        if (x >= width_ || y >= height_) return trace_result(__func__, D3DERR_INVALIDCALL);
        locked->pBits = memory_.data() + size_t(y) * pitch + size_t(x) * bpp;
        locked->Pitch = INT(pitch);
        locked_ = true;
        return D3D_OK;
    }
    if (kind_ != Kind::texture_level) return trace_result(__func__, D3DERR_INVALIDCALL);   // render targets cannot be locked
    auto* dev = info.core->dev();
    if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL);
    const int32_t r[4] = {rect ? rect->left : 0, rect ? rect->top : 0, rect ? rect->right : 0, rect ? rect->bottom : 0};
    auto lock = dev->lock_texture(texture_, level_, face_, rect ? r : nullptr, flags);
    if (!lock) return trace_result(__func__, D3DERR_INVALIDCALL);
    locked->pBits = lock.value.data; locked->Pitch = INT(lock.value.pitch);
    locked_ = true;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

HRESULT Surface9::UnlockRect() try {
    if (!locked_) return trace_result(__func__, D3DERR_INVALIDCALL);
    locked_ = false;
    if (kind_ == Kind::system_memory) return D3D_OK;
    auto* dev = info.core->dev();
    return dev ? to_hresult(dev->unlock_texture(texture_, level_, face_)) : D3DERR_INVALIDCALL;
} catch (...) { return guard_failed(__func__); }

// ---- buffers -----------------------------------------------------------------------------------------------------------------------------

VertexBuffer9::~VertexBuffer9() { if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); } }
HRESULT VertexBuffer9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT VertexBuffer9::Lock(UINT offset, UINT size, void** data, DWORD flags) try {
    auto* dev = info.core->dev();
    if (!dev || !data) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->lock_buffer(id_, offset, size, flags);
    if (!r) return trace_result(__func__, D3DERR_INVALIDCALL);
    *data = r.value.data;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT VertexBuffer9::Unlock() try { auto* dev = info.core->dev(); return dev ? to_hresult(dev->unlock_buffer(id_)) : D3DERR_INVALIDCALL; } catch (...) { return guard_failed(__func__); }
HRESULT VertexBuffer9::GetDesc(D3DVERTEXBUFFER_DESC* d) try {
    if (!d) return trace_result(__func__, D3DERR_INVALIDCALL);
    d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = usage_; d->Pool = pool_; d->Size = length_; d->FVF = fvf_;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

IndexBuffer9::~IndexBuffer9() { if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); } }
HRESULT IndexBuffer9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT IndexBuffer9::Lock(UINT offset, UINT size, void** data, DWORD flags) try {
    auto* dev = info.core->dev();
    if (!dev || !data) return trace_result(__func__, D3DERR_INVALIDCALL);
    auto r = dev->lock_buffer(id_, offset, size, flags);
    if (!r) return trace_result(__func__, D3DERR_INVALIDCALL);
    *data = r.value.data;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }
HRESULT IndexBuffer9::Unlock() try { auto* dev = info.core->dev(); return dev ? to_hresult(dev->unlock_buffer(id_)) : D3DERR_INVALIDCALL; } catch (...) { return guard_failed(__func__); }
HRESULT IndexBuffer9::GetDesc(D3DINDEXBUFFER_DESC* d) try {
    if (!d) return trace_result(__func__, D3DERR_INVALIDCALL);
    d->Format = format_; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = usage_; d->Pool = pool_; d->Size = length_;
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

// ---- declarations, shaders, state blocks -------------------------------------------------------------------------------------------------

VertexDeclaration9::~VertexDeclaration9() { if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); } }
HRESULT VertexDeclaration9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT VertexDeclaration9::GetDeclaration(D3DVERTEXELEMENT9* elements, UINT* count) try {
    if (!count) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (elements) std::memcpy(elements, elements_.data(), elements_.size() * sizeof(D3DVERTEXELEMENT9));
    *count = UINT(elements_.size());
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

VertexShader9::~VertexShader9() { if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); } }
HRESULT VertexShader9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT VertexShader9::GetFunction(void* data, UINT* size) try {
    if (!size) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (data) std::memcpy(data, code_.data(), code_.size() * 4);
    *size = UINT(code_.size() * 4);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

PixelShader9::~PixelShader9() { if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); } }
HRESULT PixelShader9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT PixelShader9::GetFunction(void* data, UINT* size) try {
    if (!size) return trace_result(__func__, D3DERR_INVALIDCALL);
    if (data) std::memcpy(data, code_.data(), code_.size() * 4);
    *size = UINT(code_.size() * 4);
    return D3D_OK;
} catch (...) { return guard_failed(__func__); }

StateBlock9::~StateBlock9() { if (auto* dev = info.core ? info.core->dev() : nullptr) { ++g_counter_destroy; dev->destroy(id_); } }
HRESULT StateBlock9::GetDevice(IDirect3DDevice9** out) try { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = reinterpret_cast<IDirect3DDevice9*>(info.device); (*out)->AddRef(); return D3D_OK; } catch (...) { return guard_failed(__func__); }
HRESULT StateBlock9::Apply() try { NUE_CALL(); auto* dev = info.core->dev(); if (!dev) return trace_result(__func__, D3DERR_INVALIDCALL); dev->apply(id_); return D3D_OK; } catch (...) { return guard_failed(__func__); }

// ---- swap chain --------------------------------------------------------------------------------------------------------------------------

HRESULT SwapChain9::Present(const RECT* a, const RECT* b, HWND c, const RGNDATA* d, DWORD) { return device_->Present(a, b, c, d); }
HRESULT SwapChain9::GetBackBuffer(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out) { return device_->GetBackBuffer(0, index, type, out); }
HRESULT SwapChain9::GetRasterStatus(D3DRASTER_STATUS* status) { return device_->GetRasterStatus(0, status); }
HRESULT SwapChain9::GetDisplayMode(D3DDISPLAYMODE* mode) { return device_->GetDisplayMode(0, mode); }
HRESULT SwapChain9::GetDevice(IDirect3DDevice9** out) { if (!out) return trace_result(__func__, D3DERR_INVALIDCALL); *out = device_; device_->AddRef(); return D3D_OK; }
HRESULT SwapChain9::GetPresentParameters(D3DPRESENT_PARAMETERS* pp) { if (!pp) return trace_result(__func__, D3DERR_INVALIDCALL); *pp = device_->pp_; return D3D_OK; }

}  // namespace nue::facade
