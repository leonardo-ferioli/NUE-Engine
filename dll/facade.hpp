// nue_d3d9 as a drop-in d3d9.dll: the COM objects a Direct3D 9 game talks to (IDirect3D9, IDirect3DDevice9, textures, buffers, surfaces, shaders, state
// blocks, swap chain) implemented on top of nue::d3d9::Layer + the bgfx adapter. Put d3d9.dll next to the game's executable (it is found before the
// system's one) and the game renders through bgfx. Written for a 32-bit game, but nothing here assumes the bitness: the 64-bit build is used by the tests.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <intrin.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "nue/bgfx_gpu.hpp"
#include "nue/d3d9_layer.hpp"

namespace nue::facade {

void log(const char* format, ...);                 // nue_d3d9.log next to the dll (config: log=0 turns it off)
HRESULT to_hresult(Status status);
// Direct3D 9 forces the x87 control word to single precision with every exception masked (0x007F) when a device is created or reset, unless
// D3DCREATE_FPU_PRESERVE is given, and leaves it that way: games that unmask exceptions before CreateDevice rely on it (measured with the real runtime).
void apply_d3d9_fpu_mode(DWORD behaviour_flags);
extern const char* volatile g_last_call;           // the COM method the game thread is in (for crash reports and the heartbeat)
extern std::atomic<unsigned> g_counter_present, g_counter_draw, g_counter_create, g_counter_calls;
// frame pacing as the game sees it (written by Present, read by the heartbeat)
extern std::atomic<unsigned> g_frame_sum_us, g_frame_max_us, g_frame_slow, g_frame_count, g_gpu_us, g_render_cpu_us, g_present_us;
HRESULT trace_result(const char* method, HRESULT hr);
HRESULT guard_failed(const char* method);              // a C++ exception escaped a COM method: logged, the call fails instead of killing the game   // logs a failing result (a few times per method) and returns it
// Every COM method of the device starts with NUE_CALL(): it records the method (crash reports, heartbeat), times it (profile in the log), notices calls from
// a thread other than the one that created the device and from then on serialises all calls (Direct3D 9 is not thread safe unless asked, but games do it).
struct MethodStats { std::atomic<unsigned long long> ticks{0}; std::atomic<unsigned> calls{0}; const char* name = ""; };
constexpr unsigned kMaxMethods = 256;
extern MethodStats g_methods[kMaxMethods];
extern DWORD g_main_thread;
extern std::atomic<bool> g_multithreaded;
extern CRITICAL_SECTION g_call_lock;
unsigned register_method(const char* name);
void note_other_thread(const char* method);
class ScopedCall {
public:
    ScopedCall(unsigned slot, const char* name) : slot_(slot) {
        g_last_call = name;
        ++g_counter_calls;
        if (g_multithreaded.load(std::memory_order_relaxed)) { EnterCriticalSection(&g_call_lock); locked_ = true; }
        else if (g_main_thread && GetCurrentThreadId() != g_main_thread) { note_other_thread(name); EnterCriticalSection(&g_call_lock); locked_ = true; }
        start_ = __rdtsc();
    }
    ~ScopedCall() {
        g_methods[slot_].ticks.fetch_add(__rdtsc() - start_, std::memory_order_relaxed);
        g_methods[slot_].calls.fetch_add(1, std::memory_order_relaxed);
        if (locked_) LeaveCriticalSection(&g_call_lock);
    }
private:
    unsigned slot_;
    unsigned long long start_ = 0;
    bool locked_ = false;
};
#define NUE_CALL() static const unsigned nue_slot_ = ::nue::facade::register_method(__func__); ::nue::facade::ScopedCall nue_scope_(nue_slot_, __func__)
void device_destroyed();                           // lets the factory create another device (bgfx is a singleton)

void set_window_status(const std::string& text);
std::string window_status_copy();   // the last window / swap chain description (NueD3D9_DescribeWindow)

struct Config {
    std::string renderer = "d3d11";                // d3d11 | vulkan
    bool vsync = true;                             // honour the game's present interval (false: never wait for the display)
    bool render_thread = true;                     // bgfx runs its backend on its own thread
    bool log = true;
    bool log_unsupported = true;
    uint32_t texture_copy_mb = 160;                // texture contents kept in system memory for later Locks (MB; 0 = none for textures on the GPU)
    uint32_t texture_report_at = 0;                // present number at which a texture report is written without a key press (0 = never; tests and unattended runs)
    bool mark_missing = false;                     // textures without data are drawn as a magenta checker (F10 toggles it while the game runs)
    uint32_t msaa = 0;                             // 0 = as the game asks (its multisample type and quality); 1 = off; 2, 4, 8 = force that many samples                   // one line per unsupported feature the game asks for (also summarised at exit)
    std::string shader_cache;                      // folder for compiled shaders (default: <dll folder>\nue_shader_cache)
    std::string dll_directory;
};
const Config& config();

// State shared by the device and every resource it created: resources can outlive the device object in badly behaved games.
struct Core {
    std::unique_ptr<gpu::BgfxGpu> gpu;
    std::unique_ptr<d3d9::Layer> layer;
    bool alive = true;
    d3d9::Device* dev() { return alive && layer ? layer.get() : nullptr; }
};

class Device9;
class Direct3D9;

// IUnknown for single-inheritance COM classes: every interface of the chain answers QueryInterface with the same pointer.
#define NUE_COM_UNKNOWN(...)                                                                                                                    \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override { return query_interface(riid, out, {__VA_ARGS__}); }           \
    ULONG STDMETHODCALLTYPE AddRef() override { return add_ref(); }                                                                            \
    ULONG STDMETHODCALLTYPE Release() override { return release(); }

class RefCounted {
protected:
    virtual ~RefCounted() = default;
    virtual ULONG add_ref() { return ++refs_; }
    virtual ULONG release() { const ULONG r = --refs_; if (!r) delete this; return r; }
    HRESULT query_interface_impl(IUnknown* self, REFIID riid, void** out, std::initializer_list<const IID*> ids);
    std::atomic<ULONG> refs_{1};
};

#define NUE_QUERY(self_type) HRESULT query_interface(REFIID riid, void** out, std::initializer_list<const IID*> ids) { return query_interface_impl(static_cast<self_type*>(this), riid, out, ids); }

// ---- resources ---------------------------------------------------------------------------------------------------------------------------

struct ChildInfo {   // what every resource keeps
    std::shared_ptr<Core> core;
    Device9* device = nullptr;
};

class Texture9 final : public IDirect3DTexture9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DTexture9)
    Texture9(ChildInfo info, d3d9::TextureId id, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~Texture9() override;
    // IDirect3DResource9
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { const DWORD o = priority_; priority_ = p; return o; }
    STDMETHOD_(DWORD, GetPriority)() override { return priority_; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_TEXTURE; }
    // IDirect3DBaseTexture9
    STDMETHOD_(DWORD, SetLOD)(DWORD l) override { const DWORD o = lod_; lod_ = l; return o; }
    STDMETHOD_(DWORD, GetLOD)() override { return lod_; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return levels_; }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE) override { return D3D_OK; }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return D3DTEXF_LINEAR; }
    STDMETHOD_(void, GenerateMipSubLevels)() override {}
    // IDirect3DTexture9
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC* desc) override;
    STDMETHOD(GetSurfaceLevel)(UINT level, IDirect3DSurface9** out) override;
    STDMETHOD(LockRect)(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(UINT level) override;
    STDMETHOD(AddDirtyRect)(const RECT*) override { return D3D_OK; }

    d3d9::TextureId id() const { return id_; }
    UINT width() const { return width_; } UINT height() const { return height_; }
    DWORD usage() const { return usage_; } D3DFORMAT format() const { return format_; } D3DPOOL pool() const { return pool_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    friend class Surface9;
    d3d9::TextureId id_;
    UINT width_, height_, levels_;
    DWORD usage_, priority_ = 0, lod_ = 0;
    D3DFORMAT format_;
    D3DPOOL pool_;
    std::vector<IDirect3DSurface9*> surfaces_;   // one per level, created on demand, owned by the texture
};

class CubeTexture9 final : public IDirect3DCubeTexture9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DResource9, &IID_IDirect3DBaseTexture9, &IID_IDirect3DCubeTexture9)
    CubeTexture9(ChildInfo info, d3d9::TextureId id, UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    ~CubeTexture9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override { const DWORD o = priority_; priority_ = p; return o; }
    STDMETHOD_(DWORD, GetPriority)() override { return priority_; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_CUBETEXTURE; }
    STDMETHOD_(DWORD, SetLOD)(DWORD l) override { const DWORD o = lod_; lod_ = l; return o; }
    STDMETHOD_(DWORD, GetLOD)() override { return lod_; }
    STDMETHOD_(DWORD, GetLevelCount)() override { return levels_; }
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE) override { return D3D_OK; }
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return D3DTEXF_LINEAR; }
    STDMETHOD_(void, GenerateMipSubLevels)() override {}
    STDMETHOD(GetLevelDesc)(UINT level, D3DSURFACE_DESC* desc) override;
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9** out) override;
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES face, UINT level) override;
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, const RECT*) override { return D3D_OK; }
    d3d9::TextureId id() const { return id_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    friend class Surface9;
    d3d9::TextureId id_;
    UINT edge_, levels_;
    DWORD usage_, priority_ = 0, lod_ = 0;
    D3DFORMAT format_;
    D3DPOOL pool_;
    std::vector<IDirect3DSurface9*> surfaces_;   // face * levels + level
};

// A surface: a level of a texture (its references go to the texture), the back buffer, a render target, a depth surface or a system-memory plain surface.
class Surface9 final : public IDirect3DSurface9, RefCounted {
public:
    enum class Kind { texture_level, back_buffer, render_target, depth_stencil, system_memory };
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DResource9, &IID_IDirect3DSurface9)
    Surface9(ChildInfo info, Kind kind, d3d9::SurfaceId id, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample);
    ~Surface9() override;
    ULONG add_ref() override { return container_ ? container_->AddRef() : ++refs_; }
    ULONG release() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_SURFACE; }
    STDMETHOD(GetContainer)(REFIID riid, void** out) override;
    STDMETHOD(GetDesc)(D3DSURFACE_DESC* desc) override;
    STDMETHOD(LockRect)(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override;
    STDMETHOD(UnlockRect)() override;
    STDMETHOD(GetDC)(HDC*) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(ReleaseDC)(HDC) override { return D3DERR_INVALIDCALL; }

    void set_container(IUnknown* container, d3d9::TextureId texture, UINT level, UINT face, DWORD usage, D3DPOOL pool) { container_ = container; texture_ = texture; level_ = level; face_ = face; texture_usage_ = usage; texture_pool_ = pool; }
    Kind kind() const { return kind_; }
    d3d9::SurfaceId id() const { return id_; }
    UINT width() const { return width_; } UINT height() const { return height_; }
    D3DFORMAT format() const { return format_; }
    std::vector<uint8_t>& memory() { return memory_; }
    void detach() { container_ = nullptr; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    friend class Texture9;
    friend class CubeTexture9;
    Kind kind_;
    d3d9::SurfaceId id_;
    UINT width_, height_;
    D3DFORMAT format_;
    D3DMULTISAMPLE_TYPE multisample_;
    IUnknown* container_ = nullptr;
    d3d9::TextureId texture_;
    UINT level_ = 0, face_ = 0;
    DWORD texture_usage_ = 0;
    D3DPOOL texture_pool_ = D3DPOOL_MANAGED;
    std::vector<uint8_t> memory_;   // system_memory surfaces
    bool locked_ = false;
};

class VertexBuffer9 final : public IDirect3DVertexBuffer9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DResource9, &IID_IDirect3DVertexBuffer9)
    VertexBuffer9(ChildInfo info, d3d9::BufferId id, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool) : info(std::move(info)), id_(id), length_(length), usage_(usage), fvf_(fvf), pool_(pool) {}
    ~VertexBuffer9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_VERTEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void** data, DWORD flags) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC* desc) override;
    d3d9::BufferId id() const { return id_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    d3d9::BufferId id_;
    UINT length_;
    DWORD usage_, fvf_;
    D3DPOOL pool_;
};

class IndexBuffer9 final : public IDirect3DIndexBuffer9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DResource9, &IID_IDirect3DIndexBuffer9)
    IndexBuffer9(ChildInfo info, d3d9::BufferId id, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool) : info(std::move(info)), id_(id), length_(length), usage_(usage), format_(format), pool_(pool) {}
    ~IndexBuffer9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(SetPrivateData)(REFGUID, const void*, DWORD, DWORD) override { return D3D_OK; }
    STDMETHOD(GetPrivateData)(REFGUID, void*, DWORD*) override { return D3DERR_NOTFOUND; }
    STDMETHOD(FreePrivateData)(REFGUID) override { return D3D_OK; }
    STDMETHOD_(DWORD, SetPriority)(DWORD) override { return 0; }
    STDMETHOD_(DWORD, GetPriority)() override { return 0; }
    STDMETHOD_(void, PreLoad)() override {}
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return D3DRTYPE_INDEXBUFFER; }
    STDMETHOD(Lock)(UINT offset, UINT size, void** data, DWORD flags) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC* desc) override;
    d3d9::BufferId id() const { return id_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    d3d9::BufferId id_;
    UINT length_;
    DWORD usage_;
    D3DFORMAT format_;
    D3DPOOL pool_;
};

class VertexDeclaration9 final : public IDirect3DVertexDeclaration9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DVertexDeclaration9)
    VertexDeclaration9(ChildInfo info, d3d9::DeclarationId id, std::vector<D3DVERTEXELEMENT9> elements) : info(std::move(info)), id_(id), elements_(std::move(elements)) {}
    ~VertexDeclaration9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9* elements, UINT* count) override;
    d3d9::DeclarationId id() const { return id_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    d3d9::DeclarationId id_;
    std::vector<D3DVERTEXELEMENT9> elements_;
};

class VertexShader9 final : public IDirect3DVertexShader9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DVertexShader9)
    VertexShader9(ChildInfo info, d3d9::ShaderId id, std::vector<DWORD> code) : info(std::move(info)), id_(id), code_(std::move(code)) {}
    ~VertexShader9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetFunction)(void* data, UINT* size) override;
    d3d9::ShaderId id() const { return id_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    d3d9::ShaderId id_;
    std::vector<DWORD> code_;
};

class PixelShader9 final : public IDirect3DPixelShader9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DPixelShader9)
    PixelShader9(ChildInfo info, d3d9::ShaderId id, std::vector<DWORD> code) : info(std::move(info)), id_(id), code_(std::move(code)) {}
    ~PixelShader9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetFunction)(void* data, UINT* size) override;
    d3d9::ShaderId id() const { return id_; }
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    d3d9::ShaderId id_;
    std::vector<DWORD> code_;
};

class StateBlock9 final : public IDirect3DStateBlock9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DStateBlock9)
    StateBlock9(ChildInfo info, d3d9::StateBlockId id) : info(std::move(info)), id_(id) {}
    ~StateBlock9() override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(Capture)() override { return D3D_OK; }   // the block keeps what was recorded when it was created
    STDMETHOD(Apply)() override;
    ChildInfo info;
    NUE_QUERY(IUnknown)
private:
    d3d9::StateBlockId id_;
};

class SwapChain9 final : public IDirect3DSwapChain9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DSwapChain9)
    explicit SwapChain9(Device9* device) : device_(device) {}
    STDMETHOD(Present)(const RECT*, const RECT*, HWND, const RGNDATA*, DWORD flags) override;
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9*) override { return D3DERR_NOTAVAILABLE; }
    STDMETHOD(GetBackBuffer)(UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out) override;
    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS* status) override;
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE* mode) override;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS* pp) override;
    NUE_QUERY(IUnknown)
private:
    Device9* device_;
};

// ---- the device --------------------------------------------------------------------------------------------------------------------------

class Device9 final : public IDirect3DDevice9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3DDevice9)
    Device9(Direct3D9* d3d, std::shared_ptr<Core> core, const D3DDEVICE_CREATION_PARAMETERS& creation, const D3DPRESENT_PARAMETERS& pp, const D3DCAPS9& caps, HWND window);
    ~Device9() override;
    NUE_QUERY(IUnknown)

    STDMETHOD(TestCooperativeLevel)() override;
    STDMETHOD_(UINT, GetAvailableTextureMem)() override { return 1024u * 1024u * 1024u; }
    STDMETHOD(EvictManagedResources)() override { return D3D_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D9** out) override;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9* caps) override;
    STDMETHOD(GetDisplayMode)(UINT swap_chain, D3DDISPLAYMODE* mode) override;
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS* params) override;
    STDMETHOD(SetCursorProperties)(UINT, UINT, IDirect3DSurface9*) override { return D3D_OK; }
    STDMETHOD_(void, SetCursorPosition)(int, int, DWORD) override {}
    STDMETHOD_(BOOL, ShowCursor)(BOOL show) override { const BOOL old = cursor_; cursor_ = show; return old; }
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**) override { return unsupported("CreateAdditionalSwapChain"); }
    STDMETHOD(GetSwapChain)(UINT index, IDirect3DSwapChain9** out) override;
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override { return 1; }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS* pp) override;
    STDMETHOD(Present)(const RECT*, const RECT*, HWND, const RGNDATA*) override;
    STDMETHOD(GetBackBuffer)(UINT swap_chain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9** out) override;
    STDMETHOD(GetRasterStatus)(UINT, D3DRASTER_STATUS* status) override;
    STDMETHOD(SetDialogBoxMode)(BOOL) override { return D3D_OK; }
    STDMETHOD_(void, SetGammaRamp)(UINT, DWORD, const D3DGAMMARAMP* ramp) override { if (ramp) gamma_ = *ramp; }
    STDMETHOD_(void, GetGammaRamp)(UINT, D3DGAMMARAMP* ramp) override { if (ramp) *ramp = gamma_; }
    STDMETHOD(CreateTexture)(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) override;
    STDMETHOD(CreateVolumeTexture)(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*) override { return unsupported("CreateVolumeTexture"); }
    STDMETHOD(CreateCubeTexture)(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) override;
    STDMETHOD(CreateVertexBuffer)(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared) override;
    STDMETHOD(CreateIndexBuffer)(UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared) override;
    STDMETHOD(CreateRenderTarget)(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL lockable, IDirect3DSurface9** out, HANDLE* shared) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multisample, DWORD quality, BOOL discard, IDirect3DSurface9** out, HANDLE* shared) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const POINT*) override { return unsupported("UpdateSurface"); }
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9* source, IDirect3DBaseTexture9* destination) override;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9* render_target, IDirect3DSurface9* destination) override;
    STDMETHOD(GetFrontBufferData)(UINT, IDirect3DSurface9*) override { return unsupported("GetFrontBufferData"); }
    STDMETHOD(StretchRect)(IDirect3DSurface9* source, const RECT* source_rect, IDirect3DSurface9* destination, const RECT* destination_rect, D3DTEXTUREFILTERTYPE filter) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9*, const RECT*, D3DCOLOR) override { return unsupported("ColorFill"); }
    STDMETHOD(CreateOffscreenPlainSurface)(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9** out, HANDLE* shared) override;
    STDMETHOD(SetRenderTarget)(DWORD index, IDirect3DSurface9* target) override;
    STDMETHOD(GetRenderTarget)(DWORD index, IDirect3DSurface9** out) override;
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9* depth) override;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9** out) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(Clear)(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE type, D3DMATRIX* matrix) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE type, const D3DMATRIX* matrix) override;
    STDMETHOD(SetViewport)(const D3DVIEWPORT9* viewport) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT9* viewport) override;
    STDMETHOD(SetMaterial)(const D3DMATERIAL9* material) override { if (material) material_ = *material; return D3D_OK; }
    STDMETHOD(GetMaterial)(D3DMATERIAL9* material) override { if (material) *material = material_; return D3D_OK; }
    STDMETHOD(SetLight)(DWORD index, const D3DLIGHT9* light) override;
    STDMETHOD(GetLight)(DWORD index, D3DLIGHT9* light) override;
    STDMETHOD(LightEnable)(DWORD index, BOOL enable) override;
    STDMETHOD(GetLightEnable)(DWORD index, BOOL* enable) override;
    STDMETHOD(SetClipPlane)(DWORD, const float*) override { return D3D_OK; }
    STDMETHOD(GetClipPlane)(DWORD, float* plane) override { if (plane) plane[0] = plane[1] = plane[2] = plane[3] = 0; return D3D_OK; }
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE state, DWORD value) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE state, DWORD* value) override;
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9** out) override;
    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9** out) override;
    STDMETHOD(SetClipStatus)(const D3DCLIPSTATUS9*) override { return D3D_OK; }
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9* status) override { if (status) *status = D3DCLIPSTATUS9{}; return D3D_OK; }
    STDMETHOD(GetTexture)(DWORD stage, IDirect3DBaseTexture9** out) override;
    STDMETHOD(SetTexture)(DWORD stage, IDirect3DBaseTexture9* texture) override;
    STDMETHOD(GetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD* value) override;
    STDMETHOD(SetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override;
    STDMETHOD(GetSamplerState)(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD* value) override;
    STDMETHOD(SetSamplerState)(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override;
    STDMETHOD(ValidateDevice)(DWORD* passes) override { if (passes) *passes = 1; return D3D_OK; }
    STDMETHOD(SetPaletteEntries)(UINT, const PALETTEENTRY*) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(GetPaletteEntries)(UINT, PALETTEENTRY*) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(SetCurrentTexturePalette)(UINT) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(GetCurrentTexturePalette)(UINT*) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(SetScissorRect)(const RECT* rect) override { if (rect) scissor_ = *rect; return D3D_OK; }
    STDMETHOD(GetScissorRect)(RECT* rect) override { if (rect) *rect = scissor_; return D3D_OK; }
    STDMETHOD(SetSoftwareVertexProcessing)(BOOL) override { return D3D_OK; }
    STDMETHOD_(BOOL, GetSoftwareVertexProcessing)() override { return FALSE; }
    STDMETHOD(SetNPatchMode)(float) override { return D3D_OK; }
    STDMETHOD_(float, GetNPatchMode)() override { return 0.0f; }
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE, UINT, UINT) override { return unsupported("DrawPrimitive"); }
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE type, INT base_vertex, UINT min_vertex, UINT vertex_count, UINT start_index, UINT primitive_count) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE type, UINT primitive_count, const void* vertices, UINT stride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT) override { return unsupported("DrawIndexedPrimitiveUP"); }
    STDMETHOD(ProcessVertices)(UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD) override { return unsupported("ProcessVertices"); }
    STDMETHOD(CreateVertexDeclaration)(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** out) override;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9* declaration) override;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9** out) override;
    STDMETHOD(SetFVF)(DWORD fvf) override;
    STDMETHOD(GetFVF)(DWORD* fvf) override { if (fvf) *fvf = fvf_; return D3D_OK; }
    STDMETHOD(CreateVertexShader)(const DWORD* function, IDirect3DVertexShader9** out) override;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9* shader) override;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9** out) override;
    STDMETHOD(SetVertexShaderConstantF)(UINT start, const float* data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantF)(UINT start, float* data, UINT count) override;
    STDMETHOD(SetVertexShaderConstantI)(UINT start, const int* data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantI)(UINT start, int* data, UINT count) override;
    STDMETHOD(SetVertexShaderConstantB)(UINT start, const BOOL* data, UINT count) override;
    STDMETHOD(GetVertexShaderConstantB)(UINT start, BOOL* data, UINT count) override;
    STDMETHOD(SetStreamSource)(UINT stream, IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) override;
    STDMETHOD(GetStreamSource)(UINT stream, IDirect3DVertexBuffer9** out, UINT* offset, UINT* stride) override;
    STDMETHOD(SetStreamSourceFreq)(UINT, UINT divider) override { return divider == 1 || divider == 0 ? D3D_OK : unsupported("SetStreamSourceFreq (instancing)"); }
    STDMETHOD(GetStreamSourceFreq)(UINT, UINT* divider) override { if (divider) *divider = 1; return D3D_OK; }
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9* buffer) override;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9** out) override;
    STDMETHOD(CreatePixelShader)(const DWORD* function, IDirect3DPixelShader9** out) override;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9* shader) override;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9** out) override;
    STDMETHOD(SetPixelShaderConstantF)(UINT start, const float* data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantF)(UINT start, float* data, UINT count) override;
    STDMETHOD(SetPixelShaderConstantI)(UINT start, const int* data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantI)(UINT start, int* data, UINT count) override;
    STDMETHOD(SetPixelShaderConstantB)(UINT start, const BOOL* data, UINT count) override;
    STDMETHOD(GetPixelShaderConstantB)(UINT start, BOOL* data, UINT count) override;
    STDMETHOD(DrawRectPatch)(UINT, const float*, const D3DRECTPATCH_INFO*) override { return unsupported("DrawRectPatch"); }
    STDMETHOD(DrawTriPatch)(UINT, const float*, const D3DTRIPATCH_INFO*) override { return unsupported("DrawTriPatch"); }
    STDMETHOD(DeletePatch)(UINT) override { return unsupported("DeletePatch"); }
    STDMETHOD(CreateQuery)(D3DQUERYTYPE, IDirect3DQuery9**) override { return unsupported("CreateQuery"); }   // the game falls back when queries are not offered

    HRESULT unsupported(const char* what);
    const std::shared_ptr<Core>& core() const { return core_; }
    ChildInfo child() { return ChildInfo{core_, this}; }
    void shutdown();

private:
    friend class SwapChain9;
    d3d9::DeviceParams params_from(const D3DPRESENT_PARAMETERS& pp, UINT& width, UINT& height) const;
    void apply_window_size();
    Surface9* back_buffer_surface();
    template <class T> static void assign(T*& slot, T* value) { if (value) value->AddRef(); if (slot) slot->Release(); slot = value; }

    Direct3D9* d3d_;
    std::shared_ptr<Core> core_;
    D3DDEVICE_CREATION_PARAMETERS creation_;
    D3DPRESENT_PARAMETERS pp_;
    D3DCAPS9 caps_;
    HWND window_;
    UINT window_w_ = 0, window_h_ = 0;
    RECT visible_client_{};
    bool visible_known_ = false; int status_dirty_ = 0;
    BOOL cursor_ = FALSE;
    D3DGAMMARAMP gamma_{};
    D3DMATERIAL9 material_{};
    RECT scissor_{};
    D3DVIEWPORT9 viewport_{};
    DWORD fvf_ = 0;
    DWORD render_state_[256] = {};
    DWORD sampler_state_[16][14] = {};
    DWORD stage_state_[8][33] = {};
    std::map<DWORD, D3DMATRIX> transforms_;
    std::map<DWORD, std::pair<D3DLIGHT9, BOOL>> lights_;
    std::vector<float> vs_f_ = std::vector<float>(256 * 4), ps_f_ = std::vector<float>(224 * 4);
    int vs_i_[16][4] = {}, ps_i_[16][4] = {};
    BOOL vs_b_[16] = {}, ps_b_[16] = {};
    // what the device keeps referenced while it is bound
    IDirect3DBaseTexture9* textures_[16] = {};
    IDirect3DVertexBuffer9* streams_[16] = {};
    UINT stream_offset_[16] = {}, stream_stride_[16] = {};
    IDirect3DIndexBuffer9* indices_ = nullptr;
    IDirect3DSurface9* render_targets_[4] = {};
    IDirect3DSurface9* depth_stencil_ = nullptr;
    IDirect3DVertexDeclaration9* declaration_ = nullptr;
    IDirect3DVertexShader9* vertex_shader_ = nullptr;
    IDirect3DPixelShader9* pixel_shader_ = nullptr;
    Surface9* back_buffer_ = nullptr;
    Surface9* auto_depth_ = nullptr;
    bool in_scene_ = false;
    bool recording_block_ = false;
    d3d9::StateBlockId recording_id_;
    std::map<std::string, int> unsupported_seen_;
    unsigned long long last_present_ticks_ = 0;
    unsigned present_count_ = 0;
    std::string last_report_;
};

// ---- the factory -------------------------------------------------------------------------------------------------------------------------

class Direct3D9 final : public IDirect3D9, RefCounted {
public:
    NUE_COM_UNKNOWN(&IID_IUnknown, &IID_IDirect3D9)
    Direct3D9();
    ~Direct3D9() override;
    NUE_QUERY(IUnknown)
    STDMETHOD(RegisterSoftwareDevice)(void*) override { return D3DERR_NOTAVAILABLE; }
    STDMETHOD_(UINT, GetAdapterCount)() override;
    STDMETHOD(GetAdapterIdentifier)(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) override;
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT adapter, D3DFORMAT format) override;
    STDMETHOD(EnumAdapterModes)(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) override;
    STDMETHOD(GetAdapterDisplayMode)(UINT adapter, D3DDISPLAYMODE* mode) override;
    STDMETHOD(CheckDeviceType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) override;
    STDMETHOD(CheckDeviceFormat)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT check) override;
    STDMETHOD(CheckDeviceMultiSampleType)(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE multisample, DWORD* quality) override;
    STDMETHOD(CheckDepthStencilMatch)(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, D3DFORMAT render_target, D3DFORMAT depth) override;
    STDMETHOD(CheckDeviceFormatConversion)(UINT adapter, D3DDEVTYPE type, D3DFORMAT source, D3DFORMAT target) override;
    STDMETHOD(GetDeviceCaps)(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) override;
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT adapter) override;
    STDMETHOD(CreateDevice)(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) override;
    IDirect3D9* real() const { return real_; }
private:
    IDirect3D9* real_ = nullptr;   // the system's Direct3D 9, used only to answer capability questions exactly as the hardware would
};

}  // namespace nue::facade
