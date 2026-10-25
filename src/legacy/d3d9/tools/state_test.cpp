// nue_state_test: render states and fixed-function texture stages, drawn through the drop-in d3d9.dll and through the system Direct3D 9 runtime with the same calls, and compared.
//
//   nue_state_test <path to our d3d9.dll> [filter]
//
// Pre-transformed quads (XYZRHW | DIFFUSE | TEX1, the vertex format the game uses without shaders) onto a 32x32 window back buffer with an automatic depth buffer; colour is read
// back before Present. Windows are never shown. The system runtime is the ground truth. Cases cover alpha test, blending, depth bias, culling, colour write masks, texture-stage operations,
// texture coordinate generation and the texture transform (what a car's reflection layers use).
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {
using CreateFn = IDirect3D9*(WINAPI*)(UINT);

struct Vertex { float x, y, z, rhw; DWORD diffuse; float u, v; };
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
struct Vertex3 { float x, y, z; float nx, ny, nz; float u, v; };
constexpr DWORD kFvf3 = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

struct Context {
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    HWND window = nullptr;
    IDirect3DTexture9* texture = nullptr;      // 4x4 pseudo-random A8R8G8B8
    IDirect3DTexture9* texture2 = nullptr;
    IDirect3DCubeTexture9* cube = nullptr;     // six flat colours
    IDirect3DTexture9* mips = nullptr;         // 16x16, five levels, each a flat colour of its own
    IDirect3DTexture9* target = nullptr;       // 16x16 render target (smaller than the 32x32 back buffer and its depth buffer)
};

bool make_context(CreateFn create, Context& c) {
    c.window = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "STATIC", "nue state test", WS_OVERLAPPED, 100, 100, 32, 32, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);   // never shown
    c.d3d = create(D3D_SDK_VERSION);
    if (!c.d3d || !c.window) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 32; pp.BackBufferHeight = 32; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE; pp.hDeviceWindow = c.window; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    if (FAILED(c.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, c.window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &c.device)) || !c.device) return false;
    uint32_t seed = 4242;
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return uint8_t(seed >> 16); };
    for (IDirect3DTexture9** t : {&c.texture, &c.texture2}) {
        if (FAILED(c.device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, t, nullptr))) return false;
        D3DLOCKED_RECT lr;
        (*t)->LockRect(0, &lr, nullptr, 0);
        for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) { uint8_t* p = static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4; p[0] = rnd(); p[1] = rnd(); p[2] = rnd(); p[3] = rnd(); }
        (*t)->UnlockRect(0);
    }
    if (SUCCEEDED(c.device->CreateTexture(16, 16, 5, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c.mips, nullptr))) {
        const DWORD level_colors[5] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF};
        for (UINT level = 0; level < 5; ++level) {
            D3DLOCKED_RECT lr;
            c.mips->LockRect(level, &lr, nullptr, 0);
            const UINT size = 16u >> level;
            for (UINT y = 0; y < size; ++y) for (UINT x = 0; x < size; ++x) std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4, &level_colors[level], 4);
            c.mips->UnlockRect(level);
        }
    }
    c.device->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c.target, nullptr);
    if (SUCCEEDED(c.device->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c.cube, nullptr))) {
        const DWORD colors[6] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF};   // +X -X +Y -Y +Z -Z
        for (int face = 0; face < 6; ++face) {
            D3DLOCKED_RECT lr;
            c.cube->LockRect(D3DCUBEMAP_FACES(face), 0, &lr, nullptr, 0);
            for (UINT y = 0; y < 4; ++y) for (UINT x = 0; x < 4; ++x) std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4, &colors[face], 4);
            c.cube->UnlockRect(D3DCUBEMAP_FACES(face), 0);
        }
    }
    return true;
}

void defaults(IDirect3DDevice9* d) {
    d->SetRenderState(D3DRS_ZENABLE, TRUE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
    d->SetRenderState(D3DRS_ALPHAREF, 0);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetRenderState(D3DRS_DEPTHBIAS, 0);
    d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    d->SetRenderState(D3DRS_TEXTUREFACTOR, 0x80C0A060);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    for (DWORD s = 0; s < 4; ++s) {
        d->SetTexture(s, nullptr);
        d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT); d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetTextureStageState(s, D3DTSS_COLOROP, s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
        d->SetTextureStageState(s, D3DTSS_COLORARG1, D3DTA_DIFFUSE); d->SetTextureStageState(s, D3DTSS_COLORARG2, D3DTA_CURRENT); d->SetTextureStageState(s, D3DTSS_COLORARG0, D3DTA_CURRENT);
        d->SetTextureStageState(s, D3DTSS_ALPHAOP, s == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);
        d->SetTextureStageState(s, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE); d->SetTextureStageState(s, D3DTSS_ALPHAARG2, D3DTA_CURRENT); d->SetTextureStageState(s, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
        d->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
        d->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        d->SetTextureStageState(s, D3DTSS_RESULTARG, D3DTA_CURRENT);
    }
    for (DWORD i = 0; i < 8; ++i) d->LightEnable(i, FALSE);
    d->SetRenderState(D3DRS_COLORVERTEX, TRUE); d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1); d->SetRenderState(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
    d->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL); d->SetRenderState(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    d->SetRenderState(D3DRS_LOCALVIEWER, TRUE); d->SetRenderState(D3DRS_NORMALIZENORMALS, FALSE); d->SetRenderState(D3DRS_AMBIENT, 0);
    d->SetFVF(kFvf);
}

void quad(IDirect3DDevice9* d, float x0, float y0, float x1, float y1, float z0, float z1, DWORD c00, DWORD c10, DWORD c01, DWORD c11) {
    const Vertex v[4] = {{x0 - 0.5f, y0 - 0.5f, z0, 1.0f, c00, 0, 0}, {x1 - 0.5f, y0 - 0.5f, z0, 1.0f, c10, 1, 0}, {x0 - 0.5f, y1 - 0.5f, z1, 1.0f, c01, 0, 1}, {x1 - 0.5f, y1 - 0.5f, z1, 1.0f, c11, 1, 1}};
    d->SetFVF(kFvf);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vertex));
}
void quad(IDirect3DDevice9* d, float z, DWORD c) { quad(d, 0, 0, 32, 32, z, z, c, c, c, c); }

std::vector<uint8_t> frame(Context& c, const std::function<void(Context&)>& draw) {
    IDirect3DDevice9* d = c.device;
    defaults(d);
    d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, D3DCOLOR_XRGB(40, 80, 120), 1.0f, 0);
    d->BeginScene();
    draw(c);
    d->EndScene();
    std::vector<uint8_t> out;
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    if (FAILED(d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return out;
    if (SUCCEEDED(d->CreateOffscreenPlainSurface(32, 32, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr)) && SUCCEEDED(d->GetRenderTargetData(back, copy))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(copy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            out.resize(32 * 32 * 3);
            for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x) std::memcpy(&out[(y * 32 + x) * 3], static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4, 3);   // B, G, R
            copy->UnlockRect();
        }
    }
    if (copy) copy->Release();
    back->Release();
    return out;
}

struct Case { std::string name; std::function<void(Context&)> draw; };

std::vector<Case> build_cases() {
    std::vector<Case> cases;
    auto add = [&](std::string name, std::function<void(Context&)> f) { cases.push_back({std::move(name), std::move(f)}); };
    // ---- alpha test: vertex alpha runs 0..255 from left to right
    const char* funcs[] = {"NEVER", "LESS", "EQUAL", "LESSEQUAL", "GREATER", "NOTEQUAL", "GREATEREQUAL", "ALWAYS"};
    for (int f = 1; f <= 8; ++f) add(std::string("alpha test ") + funcs[f - 1] + " ref 0x80", [f](Context& c) {
        c.device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); c.device->SetRenderState(D3DRS_ALPHAFUNC, f); c.device->SetRenderState(D3DRS_ALPHAREF, 0x80);
        quad(c.device, 0, 0, 32, 32, 0.5f, 0.5f, 0x00FF8000, 0xFFFF8000, 0x00FF8000, 0xFFFF8000);
    });
    add("alpha test GREATER ref 0x10 with a texture", [](Context& c) {
        c.device->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE); c.device->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER); c.device->SetRenderState(D3DRS_ALPHAREF, 0x10);
        c.device->SetTexture(0, c.texture); c.device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); c.device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); c.device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        c.device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); c.device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); c.device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        quad(c.device, 0.5f, 0xFFFFFFFF);
    });
    // ---- blending over an opaque base
    struct Blend { const char* name; DWORD src, dst, op; };
    const Blend blends[] = {
        {"SRCALPHA INVSRCALPHA", D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD}, {"ONE ONE", D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD}, {"DESTCOLOR ZERO", D3DBLEND_DESTCOLOR, D3DBLEND_ZERO, D3DBLENDOP_ADD},
        {"ZERO SRCCOLOR", D3DBLEND_ZERO, D3DBLEND_SRCCOLOR, D3DBLENDOP_ADD}, {"SRCALPHA ONE", D3DBLEND_SRCALPHA, D3DBLEND_ONE, D3DBLENDOP_ADD}, {"INVDESTCOLOR ONE", D3DBLEND_INVDESTCOLOR, D3DBLEND_ONE, D3DBLENDOP_ADD},
        {"DESTALPHA INVDESTALPHA", D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA, D3DBLENDOP_ADD}, {"ONE ONE SUBTRACT", D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_SUBTRACT}, {"ONE ONE REVSUBTRACT", D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_REVSUBTRACT},
        {"ONE ONE MIN", D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_MIN}, {"ONE ONE MAX", D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_MAX}, {"BLENDFACTOR INVBLENDFACTOR", D3DBLEND_BLENDFACTOR, D3DBLEND_INVBLENDFACTOR, D3DBLENDOP_ADD},
        {"SRCALPHASAT ONE", D3DBLEND_SRCALPHASAT, D3DBLEND_ONE, D3DBLENDOP_ADD},
    };
    for (const Blend& b : blends) add(std::string("blend ") + b.name, [b](Context& c) {
        quad(c.device, 0.6f, 0xFF30A0D0);
        c.device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); c.device->SetRenderState(D3DRS_SRCBLEND, b.src); c.device->SetRenderState(D3DRS_DESTBLEND, b.dst); c.device->SetRenderState(D3DRS_BLENDOP, b.op);
        c.device->SetRenderState(D3DRS_BLENDFACTOR, 0x40A0C0E0);
        c.device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        quad(c.device, 0, 0, 32, 32, 0.5f, 0.5f, 0x80F06020, 0xC0102030, 0x40FFFFFF, 0xFF80FF00);
    });
    // ---- depth bias
    for (float bias : {0.0f, -0.00002f, -0.0001f, -0.001f, 0.001f}) add("depth bias " + std::to_string(bias) + " (equal depth, ZFUNC LESS)", [bias](Context& c) {
        quad(c.device, 0.5f, 0xFFFF0000);
        c.device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        DWORD bits; std::memcpy(&bits, &bias, 4);
        c.device->SetRenderState(D3DRS_DEPTHBIAS, bits);
        quad(c.device, 0, 0, 32, 16, 0.5f, 0.5f, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);
    });
    for (float slope : {0.0f, -1.0f, -4.0f}) add("slope scale depth bias " + std::to_string(slope) + " (sloped, ZFUNC LESS)", [slope](Context& c) {
        quad(c.device, 0, 0, 32, 32, 0.3f, 0.7f, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000);
        c.device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        DWORD bits; std::memcpy(&bits, &slope, 4);
        c.device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, bits);
        quad(c.device, 0, 0, 32, 32, 0.3f, 0.7f, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);
    });
    // ---- culling: a triangle with a winding, three cull modes
    for (DWORD mode : {D3DCULL_NONE, D3DCULL_CW, D3DCULL_CCW}) for (int winding = 0; winding < 2; ++winding) add("cull mode " + std::to_string(mode) + " winding " + std::to_string(winding), [mode, winding](Context& c) {
        c.device->SetRenderState(D3DRS_CULLMODE, mode);
        Vertex t[3] = {{4, 4, 0.5f, 1, 0xFFFFFF00, 0, 0}, {28, 4, 0.5f, 1, 0xFFFFFF00, 1, 0}, {16, 28, 0.5f, 1, 0xFFFFFF00, 0, 1}};
        if (winding) std::swap(t[1], t[2]);
        c.device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t, sizeof(Vertex));
    });
    // ---- colour write mask
    for (DWORD mask : {0x1u, 0x6u, 0xAu, 0x0u}) add("colour write mask " + std::to_string(mask), [mask](Context& c) {
        c.device->SetRenderState(D3DRS_COLORWRITEENABLE, mask);
        quad(c.device, 0.5f, 0xFFC0A060);
    });
    // ---- texture stage operations: stage 0 = texture (op) diffuse, then stage 1 = another texture combined with the result
    const struct { const char* name; DWORD op; } ops[] = {
        {"SELECTARG1", D3DTOP_SELECTARG1}, {"SELECTARG2", D3DTOP_SELECTARG2}, {"MODULATE", D3DTOP_MODULATE}, {"MODULATE2X", D3DTOP_MODULATE2X}, {"MODULATE4X", D3DTOP_MODULATE4X}, {"ADD", D3DTOP_ADD},
        {"ADDSIGNED", D3DTOP_ADDSIGNED}, {"ADDSIGNED2X", D3DTOP_ADDSIGNED2X}, {"SUBTRACT", D3DTOP_SUBTRACT}, {"ADDSMOOTH", D3DTOP_ADDSMOOTH}, {"BLENDDIFFUSEALPHA", D3DTOP_BLENDDIFFUSEALPHA},
        {"BLENDTEXTUREALPHA", D3DTOP_BLENDTEXTUREALPHA}, {"BLENDFACTORALPHA", D3DTOP_BLENDFACTORALPHA}, {"BLENDTEXTUREALPHAPM", D3DTOP_BLENDTEXTUREALPHAPM}, {"BLENDCURRENTALPHA", D3DTOP_BLENDCURRENTALPHA},
        {"MODULATEALPHA_ADDCOLOR", D3DTOP_MODULATEALPHA_ADDCOLOR}, {"MODULATECOLOR_ADDALPHA", D3DTOP_MODULATECOLOR_ADDALPHA}, {"MODULATEINVALPHA_ADDCOLOR", D3DTOP_MODULATEINVALPHA_ADDCOLOR},
        {"MODULATEINVCOLOR_ADDALPHA", D3DTOP_MODULATEINVCOLOR_ADDALPHA}, {"DOTPRODUCT3", D3DTOP_DOTPRODUCT3}, {"MULTIPLYADD", D3DTOP_MULTIPLYADD}, {"LERP", D3DTOP_LERP},
    };
    for (const auto& o : ops) {
        const DWORD op = o.op;
        add(std::string("stage 0 colour op ") + o.name + " (texture, diffuse, factor)", [op](Context& c) {
            auto* d = c.device;
            d->SetTexture(0, c.texture);
            d->SetTextureStageState(0, D3DTSS_COLOROP, op); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); d->SetTextureStageState(0, D3DTSS_COLORARG0, D3DTA_TFACTOR);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            quad(d, 0, 0, 32, 32, 0.5f, 0.5f, 0xFF9060C0, 0x80C0A030, 0xC0304080, 0x40F0F0F0);
        });
        add(std::string("stage 1 colour op ") + o.name + " after a modulate stage", [op](Context& c) {
            auto* d = c.device;
            d->SetTexture(0, c.texture); d->SetTexture(1, c.texture2);
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            d->SetTextureStageState(1, D3DTSS_COLOROP, op); d->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT); d->SetTextureStageState(1, D3DTSS_COLORARG0, D3DTA_TFACTOR);
            d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); d->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            quad(d, 0, 0, 32, 32, 0.5f, 0.5f, 0xFF9060C0, 0xA0C0A030, 0xC0304080, 0x80F0F0F0);
        });
    }
    add("alpha op ADD / SUBTRACT / MODULATE2X on two stages", [](Context& c) {
        auto* d = c.device;
        d->SetTexture(0, c.texture); d->SetTexture(1, c.texture2);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_ADD); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SUBTRACT); d->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE); d->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE); d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        quad(d, 0, 0, 32, 32, 0.5f, 0.5f, 0x80FFFFFF, 0x80FFFFFF, 0x80FFFFFF, 0x80FFFFFF);
    });
    add("texture factor and complement/alpha-replicate argument modifiers", [](Context& c) {
        auto* d = c.device;
        d->SetTexture(0, c.texture);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_COMPLEMENT); d->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
        quad(d, 0.5f, 0xFFFFFFFF);
    });
    add("TEMP register result (RESULTARG)", [](Context& c) {
        auto* d = c.device;
        d->SetTexture(0, c.texture); d->SetTexture(1, c.texture2);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_TEMP);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD); d->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE); d->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEMP);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); d->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        quad(d, 0.5f, 0xFF808080);
    });
    // ---- the texture transform on a pre-transformed quad (2D coordinates scaled and offset)
    add("texture transform COUNT2 (scale and offset)", [](Context& c) {
        auto* d = c.device;
        d->SetTexture(0, c.texture);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
        const float m[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0.25f, 0.25f, 1, 0, 0, 0, 0, 1};
        d->SetTransform(D3DTS_TEXTURE0, reinterpret_cast<const D3DMATRIX*>(m));
        quad(d, 0.5f, 0xFFFFFFFF);
    });
    const struct { const char* name; float m[16]; DWORD flags; } transforms[] = {
        {"identity COUNT2", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, D3DTTFF_COUNT2},
        {"scale 0.5 COUNT2", {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, D3DTTFF_COUNT2},
        {"offset in row 3 COUNT2", {1, 0, 0, 0, 0, 1, 0, 0, 0.25f, 0.25f, 1, 0, 0, 0, 0, 1}, D3DTTFF_COUNT2},
        {"offset in row 4 COUNT2", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.25f, 0.25f, 0, 1}, D3DTTFF_COUNT2},
        {"offset in row 4 COUNT3", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.25f, 0.25f, 0, 1}, D3DTTFF_COUNT3},
        {"swap u v COUNT2", {0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, D3DTTFF_COUNT2},
    };
    for (const auto& t : transforms) {
        std::array<float, 16> mm{}; std::memcpy(mm.data(), t.m, 64); const DWORD fl = t.flags;
        add(std::string("texture transform ") + t.name, [mm, fl](Context& c) {
            auto* d = c.device;
            d->SetTexture(0, c.texture);
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, fl);
            d->SetTransform(D3DTS_TEXTURE0, reinterpret_cast<const D3DMATRIX*>(mm.data()));
            quad(d, 0.5f, 0xFFFFFFFF);
        });
    }
    // ---- texture coordinate generation: a sphere-ish patch with normals, a cube map, camera space reflection / normal / position
    const struct { const char* name; DWORD tci; DWORD flags; } gens[] = {
        {"TCI CAMERASPACENORMAL cube COUNT3", D3DTSS_TCI_CAMERASPACENORMAL, D3DTTFF_COUNT3}, {"TCI CAMERASPACEREFLECTIONVECTOR cube COUNT3", D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR, D3DTTFF_COUNT3},
        {"TCI CAMERASPACEPOSITION cube COUNT3", D3DTSS_TCI_CAMERASPACEPOSITION, D3DTTFF_COUNT3},
        {"TCI SPHEREMAP 2D texture COUNT2", D3DTSS_TCI_SPHEREMAP, D3DTTFF_COUNT2},
    };
    for (const auto& g : gens) {
        const DWORD tci = g.tci, flags = g.flags;
        add(g.name, [tci, flags](Context& c) {
            auto* d = c.device;
            const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            d->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(I));
            const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 3, 1};   // camera looks down +Z, the object 3 units in front
            d->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));
            const float proj[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1.0f, 1, 0, 0, -0.1f, 0};
            d->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(proj));
            d->SetTexture(0, tci == D3DTSS_TCI_SPHEREMAP ? static_cast<IDirect3DBaseTexture9*>(c.texture) : static_cast<IDirect3DBaseTexture9*>(c.cube));
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, tci);
            d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, flags);
            d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetFVF(kFvf3);
            // a grid of 8x8 quads bulging like a sphere cap: normals follow the surface
            std::vector<Vertex3> verts;
            for (int j = 0; j <= 8; ++j) for (int i = 0; i <= 8; ++i) {
                const float x = (i - 4) / 4.0f, y = (j - 4) / 4.0f, z = std::sqrt(std::max(0.0f, 1.0f - 0.7f * (x * x + y * y)));
                const float len = std::sqrt(0.49f * (x * x + y * y) + z * z);
                verts.push_back({x * 0.9f, y * 0.9f, -z * 0.5f, 0.7f * x / len, 0.7f * y / len, -z / len, 0, 0});
            }
            std::vector<uint16_t> idx;
            for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) { const uint16_t a = uint16_t(j * 9 + i); idx.insert(idx.end(), {a, uint16_t(a + 1), uint16_t(a + 9), uint16_t(a + 1), uint16_t(a + 10), uint16_t(a + 9)}); }
            std::vector<Vertex3> list;   // DrawPrimitiveUP only: the layer does not offer the indexed user-pointer draw
            for (uint16_t i : idx) list.push_back(verts[i]);
            d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(list.size() / 3), list.data(), sizeof(Vertex3));
        });
    }
    // ---- fixed-function lighting on a curved patch (normals vary), camera 3 units back looking along +Z
    struct LightCase { const char* name; std::function<void(IDirect3DDevice9*)> setup; bool vertex_colors; };
    auto light = [](D3DLIGHTTYPE type, float dx, float dy, float dz, float px, float py, float pz) {
        D3DLIGHT9 l{}; l.Type = type; l.Diffuse = {1.0f, 0.8f, 0.6f, 1}; l.Specular = {0.9f, 0.9f, 1.0f, 1}; l.Ambient = {0.1f, 0.05f, 0.0f, 1};
        l.Direction = {dx, dy, dz}; l.Position = {px, py, pz}; l.Range = 20.0f; l.Attenuation0 = 1.0f; return l;
    };
    auto material = [](float power, bool emissive) {
        D3DMATERIAL9 m{}; m.Diffuse = {0.9f, 0.8f, 0.7f, 1}; m.Ambient = {0.6f, 0.5f, 0.4f, 1}; m.Specular = {1, 1, 1, 1}; m.Power = power;
        if (emissive) m.Emissive = {0.1f, 0.0f, 0.2f, 0};
        return m;
    };
    const LightCase light_cases[] = {
        {"lighting: directional, ambient", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_DIRECTIONAL, 0.3f, -0.4f, 1.0f, 0, 0, 0); d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m); d->SetRenderState(D3DRS_AMBIENT, 0xFF303030); }, false},
        {"lighting: no light enabled, ambient and emissive only", [=](IDirect3DDevice9* d) { D3DMATERIAL9 m = material(0, true); d->SetMaterial(&m); d->SetRenderState(D3DRS_AMBIENT, 0xFF506070); }, false},
        {"lighting: point light with attenuation and range", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_POINT, 0, 0, 1, 0.3f, 0.2f, -1.5f); l.Attenuation0 = 0.2f; l.Attenuation1 = 0.3f; l.Attenuation2 = 0.1f; l.Range = 2.4f; d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m); d->SetRenderState(D3DRS_AMBIENT, 0xFF101010); }, false},
        {"lighting: spot light cone", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_SPOT, 0.1f, 0.0f, 1.0f, -0.1f, 0.0f, -2.0f); l.Theta = 0.5f; l.Phi = 1.1f; l.Falloff = 1.0f; l.Attenuation0 = 0.5f; l.Attenuation1 = 0.1f; d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m); }, false},
        {"lighting: spot light, wide cone (acts as a point light)", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_SPOT, 0.1f, 0.0f, 1.0f, -0.1f, 0.0f, -2.0f); l.Theta = 2.8f; l.Phi = 3.0f; l.Falloff = 1.0f; l.Attenuation0 = 0.5f; l.Attenuation1 = 0.1f; d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m); }, false},
        {"lighting: spot light cone, falloff 2", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_SPOT, 0.2f, 0.1f, 1.0f, -0.3f, -0.1f, -2.0f); l.Theta = 0.3f; l.Phi = 1.3f; l.Falloff = 2.0f; d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m); }, false},
        {"lighting: directional with specular, local viewer", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_DIRECTIONAL, 0.4f, -0.3f, 1.0f, 0, 0, 0); d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(12.0f, false); d->SetMaterial(&m); d->SetRenderState(D3DRS_SPECULARENABLE, TRUE); d->SetRenderState(D3DRS_LOCALVIEWER, TRUE); }, false},
        {"lighting: directional with specular, infinite viewer", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_DIRECTIONAL, 0.4f, -0.3f, 1.0f, 0, 0, 0); d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(40.0f, false); d->SetMaterial(&m); d->SetRenderState(D3DRS_SPECULARENABLE, TRUE); d->SetRenderState(D3DRS_LOCALVIEWER, FALSE); }, false},
        {"lighting: point light with specular", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_POINT, 0, 0, 1, 0.5f, 0.3f, -1.8f); d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(8.0f, false); d->SetMaterial(&m); d->SetRenderState(D3DRS_SPECULARENABLE, TRUE); }, false},
        {"lighting: two lights", [=](IDirect3DDevice9* d) { D3DLIGHT9 a = light(D3DLIGHT_DIRECTIONAL, 0.5f, 0.2f, 1.0f, 0, 0, 0), b = light(D3DLIGHT_POINT, 0, 0, 1, -0.6f, -0.4f, -1.2f); b.Diffuse = {0.2f, 0.9f, 0.4f, 1}; d->SetLight(0, &a); d->SetLight(3, &b); d->LightEnable(0, TRUE); d->LightEnable(3, TRUE); D3DMATERIAL9 m = material(0, true); d->SetMaterial(&m); d->SetRenderState(D3DRS_AMBIENT, 0xFF181818); }, false},
        {"lighting: vertex colour as diffuse source", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_DIRECTIONAL, 0.3f, -0.4f, 1.0f, 0, 0, 0); d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m);
            d->SetRenderState(D3DRS_COLORVERTEX, TRUE); d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1); d->SetRenderState(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1); d->SetRenderState(D3DRS_AMBIENT, 0xFF404040); }, true},
        {"lighting: COLORVERTEX off ignores the vertex colour", [=](IDirect3DDevice9* d) { D3DLIGHT9 l = light(D3DLIGHT_DIRECTIONAL, 0.3f, -0.4f, 1.0f, 0, 0, 0); d->SetLight(0, &l); d->LightEnable(0, TRUE); D3DMATERIAL9 m = material(0, false); d->SetMaterial(&m);
            d->SetRenderState(D3DRS_COLORVERTEX, FALSE); d->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1); }, true},
    };
    for (const LightCase& lc : light_cases) {
        const LightCase test = lc;
        add(test.name, [test](Context& c) {
            auto* d = c.device;
            const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            d->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(I));
            const float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 3, 1};
            d->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(view));
            const float proj[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1.0f, 1, 0, 0, -0.1f, 0};
            d->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(proj));
            d->SetRenderState(D3DRS_LIGHTING, TRUE);
            d->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
            test.setup(d);
            struct VertexC { float x, y, z, nx, ny, nz; DWORD diffuse; };
            const DWORD fvf = D3DFVF_XYZ | D3DFVF_NORMAL | (test.vertex_colors ? D3DFVF_DIFFUSE : 0);
            d->SetFVF(fvf);
            std::vector<VertexC> verts;
            for (int j = 0; j <= 8; ++j) for (int i = 0; i <= 8; ++i) {
                const float x = (i - 4) / 4.0f, y = (j - 4) / 4.0f, z = std::sqrt(std::max(0.0f, 1.0f - 0.7f * (x * x + y * y)));
                const float len = std::sqrt(0.49f * (x * x + y * y) + z * z);
                verts.push_back({x * 0.9f, y * 0.9f, -z * 0.5f, 0.7f * x / len, 0.7f * y / len, -z / len, D3DCOLOR_ARGB(255, 60 + 24 * i, 255 - 20 * j, 90 + 12 * (i + j))});
            }
            std::vector<VertexC> list;
            for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) { const int a = j * 9 + i; for (int q : {a, a + 1, a + 9, a + 1, a + 10, a + 9}) list.push_back(verts[size_t(q)]); }
            d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, UINT(list.size() / 3), list.data(), sizeof(VertexC));
        });
    }
    // ---- mip levels: MIPFILTER NONE samples one level only (MAXMIPLEVEL), MAXMIPLEVEL keeps the more detailed levels out
    struct MipCase { const char* name; DWORD mipfilter, maxmip; float size; };
    const MipCase mip_cases[] = {
        {"mip filter NONE on a mipmapped texture, minified 4x", D3DTEXF_NONE, 0, 4}, {"mip filter POINT, minified 4x (level 2)", D3DTEXF_POINT, 0, 4},
        {"MAXMIPLEVEL 1 with mip filter POINT, not minified", D3DTEXF_POINT, 1, 16}, {"MAXMIPLEVEL 2 with mip filter NONE, minified 2x", D3DTEXF_NONE, 2, 8},
        {"MAXMIPLEVEL 1 with mip filter LINEAR, minified 4x", D3DTEXF_LINEAR, 1, 4},
    };
    for (const MipCase& mc : mip_cases) {
        const MipCase m = mc;
        add(m.name, [m](Context& c) {
            auto* d = c.device;
            d->SetTexture(0, c.mips);
            d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            d->SetSamplerState(0, D3DSAMP_MIPFILTER, m.mipfilter);
            d->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, m.maxmip);
            quad(d, 0, 0, m.size, m.size, 0.5f, 0.5f, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
            d->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
        });
    }
    // ---- a 16x16 render target used with the 32x32 depth buffer (legal in Direct3D 9: the depth buffer may be larger), then shown on the back buffer
    for (int depth_test = 0; depth_test < 2; ++depth_test) add(std::string("render target smaller than the depth buffer, depth test ") + (depth_test ? "on" : "off"), [depth_test](Context& c) {
        auto* d = c.device;
        IDirect3DSurface9 *back = nullptr, *rt = nullptr;
        d->GetRenderTarget(0, &back);
        c.target->GetSurfaceLevel(0, &rt);
        d->SetRenderTarget(0, rt);
        d->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(10, 20, 30), 1.0f, 0);
        d->SetRenderState(D3DRS_ZENABLE, depth_test ? TRUE : FALSE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);
        quad(d, 0, 0, 16, 16, 0.5f, 0.5f, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000, 0xFFFF0000);
        quad(d, 4, 4, 12, 12, 0.6f, 0.6f, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00, 0xFF00FF00);   // behind the red quad: hidden when the depth test is on
        d->SetRenderTarget(0, back);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetTexture(0, c.target);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        quad(d, 0, 0, 32, 32, 0.5f, 0.5f, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF);
        d->SetTexture(0, nullptr);
        rt->Release(); back->Release();
    });
    return cases;
}
}  // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();
    if (argc < 2) { std::printf("usage: nue_state_test <our d3d9.dll> [name filter]\n"); return 2; }
    const std::string filter = argc > 2 ? argv[2] : "";
    HMODULE ours = LoadLibraryA(argv[1]);
    char system[MAX_PATH];
    GetSystemDirectoryA(system, MAX_PATH);
    HMODULE real = LoadLibraryA((std::string(system) + "\\d3d9.dll").c_str());
    if (!ours || !real) { std::printf("cannot load the dlls\n"); return 2; }
    Context a, b;
    if (!make_context(reinterpret_cast<CreateFn>(GetProcAddress(real, "Direct3DCreate9")), a)) { std::printf("the system runtime cannot make a device\n"); return 2; }
    if (!make_context(reinterpret_cast<CreateFn>(GetProcAddress(ours, "Direct3DCreate9")), b)) { std::printf("our dll cannot make a device\n"); return 2; }
    int failed = 0, total = 0;
    for (const Case& c : build_cases()) {
        if (!filter.empty() && c.name.find(filter) == std::string::npos) continue;
        const std::vector<uint8_t> r = frame(a, c.draw), o = frame(b, c.draw);
        ++total;
        if (r.empty() || o.empty()) { std::printf("FAIL %-62s no image (real %zu, ours %zu bytes)\n", c.name.c_str(), r.size(), o.size()); ++failed; continue; }
        double worst = 0, sum = 0;
        size_t bad = 0;
        for (size_t i = 0; i < r.size(); ++i) { const double v = std::abs(int(r[i]) - int(o[i])); worst = std::max(worst, v); sum += v; if (v > 3) ++bad; }
        const size_t allowed = c.name.find("TCI") != std::string::npos ? 3 : 1;   // cube face borders and sphere-map seams flip on rounding
        const bool good = bad * 100 <= r.size() * allowed;   // at most 1% of the values differ by more than 3 (edges of covered areas can round differently)
        std::printf("%s %-62s max diff %3.0f mean %.2f, %zu of %zu values differ by more than 3\n", good ? "PASS" : "FAIL", c.name.c_str(), worst, sum / double(r.size()), bad, r.size());
        if (!good && std::getenv("NUE_STATE_TEST_GRID")) {
            for (int pass = 0; pass < 2; ++pass) {
                std::printf("      %s: 8x8 grid of the blue channel (every 4th pixel)\n", pass ? "ours" : "real");
                const auto& im = pass ? o : r;
                for (int y = 0; y < 32; y += 4) { std::printf("        "); for (int x = 0; x < 32; x += 4) std::printf("%02x ", im[(y * 32 + x) * 3]); std::printf("\n"); }
            }
        }
        if (!good) {
            ++failed;
            for (int p : {0, 7 * 32 + 8, 16 * 32 + 16, 24 * 32 + 28}) std::printf("      pixel %3d,%-3d real %02x%02x%02x ours %02x%02x%02x (BGR)\n", p % 32, p / 32, r[p * 3], r[p * 3 + 1], r[p * 3 + 2], o[p * 3], o[p * 3 + 1], o[p * 3 + 2]);
        }
    }
    std::printf(failed ? "FAILED: %d of %d cases differ\n" : "PASS nue_state_test (%d cases)\n", failed ? failed : total, total);
    return failed ? 1 : 0;
}
