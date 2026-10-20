// nue_format_test: every texture format and mip level, drawn through the drop-in d3d9.dll and through the system Direct3D 9 runtime with the same calls, and compared.
//
//   nue_format_test <path to our d3d9.dll>
//
// For each format a texture of 8x8 (two levels) is filled with deterministic pseudo-random data through Lock/Unlock, sampled with point filtering by a pre-transformed quad
// (fixed-function pipeline, no shaders) onto a 64x64 window back buffer and read back. Colour and alpha are read in two passes (alpha is replicated into the colour). The windows are
// never shown. The system runtime is the ground truth; a format the system runtime cannot create is skipped.
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using CreateFn = IDirect3D9*(WINAPI*)(UINT);

struct Format { const char* name; D3DFORMAT format; UINT bits_per_pixel; bool block; bool signed_data; };
const Format formats[] = {
    {"A8R8G8B8", D3DFMT_A8R8G8B8, 32, false, false}, {"X8R8G8B8", D3DFMT_X8R8G8B8, 32, false, false}, {"R5G6B5", D3DFMT_R5G6B5, 16, false, false},
    {"X1R5G5B5", D3DFMT_X1R5G5B5, 16, false, false}, {"A1R5G5B5", D3DFMT_A1R5G5B5, 16, false, false}, {"A4R4G4B4", D3DFMT_A4R4G4B4, 16, false, false},
    {"A8", D3DFMT_A8, 8, false, false}, {"L8", D3DFMT_L8, 8, false, false}, {"A8L8", D3DFMT_A8L8, 16, false, false}, {"V8U8", D3DFMT_V8U8, 16, false, true},
    {"Q8W8V8U8", D3DFMT_Q8W8V8U8, 32, false, true}, {"A2R10G10B10", D3DFMT_A2R10G10B10, 32, false, false}, {"G16R16", D3DFMT_G16R16, 32, false, false},
    {"DXT1", D3DFMT_DXT1, 4, true, false}, {"DXT3", D3DFMT_DXT3, 8, true, false}, {"DXT5", D3DFMT_DXT5, 8, true, false},
};

uint32_t g_seed = 12345;
uint32_t next_random() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }

struct Run { bool ok = false; std::vector<uint8_t> pixels; };   // BGRA 8x8 or 4x4 colour, then alpha-as-colour

struct Device {
    IDirect3D9* d3d = nullptr;
    IDirect3DDevice9* device = nullptr;
    HWND window = nullptr;
};

bool make_device(CreateFn create, Device& d) {
    d.window = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "STATIC", "nue format test", WS_OVERLAPPED, 100, 100, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);   // never shown
    d.d3d = create(D3D_SDK_VERSION);
    if (!d.d3d || !d.window) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE; pp.hDeviceWindow = d.window; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return SUCCEEDED(d.d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, d.window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &d.device)) && d.device;
}

UINT level_bytes(const Format& f, UINT w, UINT h) {
    if (f.block) { const UINT blocks = ((w + 3) / 4) * ((h + 3) / 4); return blocks * (f.bits_per_pixel * 2); }
    return w * h * f.bits_per_pixel / 8;
}

// Deterministic data per format: valid for the format (DXT blocks keep their endpoints ordered irrelevant: any bits decode).
void fill(const Format& f, std::vector<uint8_t>& data, UINT bytes) {
    data.resize(bytes);
    for (auto& b : data) b = uint8_t(next_random());
    if (f.format == D3DFMT_A2R10G10B10 || f.format == D3DFMT_G16R16) return;
}

struct Vertex { float x, y, z, rhw, u, v; };

bool draw_quad(IDirect3DDevice9* device, IDirect3DTexture9* texture, UINT size, DWORD max_mip, bool alpha_pass, std::vector<uint8_t>& out) {
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    device->SetTexture(0, texture);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, max_mip);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, alpha_pass ? (D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE) : D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 255), 1.0f, 0);   // a texel that is never written stays magenta
    const float s = float(size);
    const Vertex quad[4] = {{-0.5f, -0.5f, 0.5f, 1.0f, 0, 0}, {s - 0.5f, -0.5f, 0.5f, 1.0f, 1, 0}, {-0.5f, s - 0.5f, 0.5f, 1.0f, 0, 1}, {s - 0.5f, s - 0.5f, 0.5f, 1.0f, 1, 1}};
    device->BeginScene();
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    device->EndScene();
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return false;
    if (FAILED(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr)) || !copy) { back->Release(); return false; }
    bool ok = SUCCEEDED(device->GetRenderTargetData(back, copy));
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(copy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        out.resize(size_t(size) * size * 4);
        for (UINT y = 0; y < size; ++y) std::memcpy(out.data() + size_t(y) * size * 4, static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, size_t(size) * 4);
        copy->UnlockRect();
    } else ok = false;
    copy->Release(); back->Release();
    return ok;
}

// The same sequence on one implementation. Returns false when the format cannot be created there.
bool run_format(Device& d, const Format& f, std::vector<uint8_t>& level0, std::vector<uint8_t>& level1) {
    IDirect3DTexture9* texture = nullptr;
    g_seed = 777 + f.format;
    if (FAILED(d.device->CreateTexture(8, 8, 2, 0, f.format, D3DPOOL_MANAGED, &texture, nullptr)) || !texture) return false;
    for (UINT level = 0; level < 2; ++level) {
        const UINT size = 8u >> level;
        D3DLOCKED_RECT lr;
        if (FAILED(texture->LockRect(level, &lr, nullptr, 0))) { texture->Release(); return false; }
        std::vector<uint8_t> data;
        fill(f, data, level_bytes(f, size, size));
        const UINT row = f.block ? ((size + 3) / 4) * (f.bits_per_pixel * 2) : size * f.bits_per_pixel / 8;
        const UINT rows = f.block ? (size + 3) / 4 : size;
        for (UINT r = 0; r < rows; ++r) std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(r) * lr.Pitch, data.data() + size_t(r) * row, row);
        texture->UnlockRect(level);
    }
    std::vector<uint8_t> color, alpha;
    bool ok = draw_quad(d.device, texture, 8, 0, false, color) && draw_quad(d.device, texture, 8, 0, true, alpha);
    std::vector<uint8_t> c1, a1;
    ok = ok && draw_quad(d.device, texture, 4, 1, false, c1) && draw_quad(d.device, texture, 4, 1, true, a1);
    d.device->SetTexture(0, nullptr);
    texture->Release();
    if (!ok) return false;
    level0 = color; level0.insert(level0.end(), alpha.begin(), alpha.end());
    level1 = c1; level1.insert(level1.end(), a1.begin(), a1.end());
    return true;
}

double max_difference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, double& mean) {
    double worst = 0, sum = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) { if ((i & 3) == 3) continue;   // the X8 byte of the back buffer is not data
        const double v = std::abs(int(a[i]) - int(b[i])); worst = std::max(worst, v); sum += v; }
    mean = n ? sum / double(n) : 0;
    return worst;
}
}  // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();
    if (argc < 2) { std::printf("usage: nue_format_test <our d3d9.dll>\n"); return 2; }
    HMODULE ours = LoadLibraryA(argv[1]);
    char system[MAX_PATH];
    GetSystemDirectoryA(system, MAX_PATH);
    HMODULE real = LoadLibraryA((std::string(system) + "\\d3d9.dll").c_str());
    if (!ours || !real) { std::printf("cannot load the dlls\n"); return 2; }
    auto create_ours = reinterpret_cast<CreateFn>(GetProcAddress(ours, "Direct3DCreate9"));
    auto create_real = reinterpret_cast<CreateFn>(GetProcAddress(real, "Direct3DCreate9"));
    Device a, b;
    if (!make_device(create_real, a)) { std::printf("the system runtime cannot make a device\n"); return 2; }
    if (!make_device(create_ours, b)) { std::printf("our dll cannot make a device\n"); return 2; }
    int failed = 0;
    for (const Format& f : formats) {
        std::vector<uint8_t> r0, r1, o0, o1;
        const bool real_ok = run_format(a, f, r0, r1);
        if (!real_ok) { std::printf("SKIP %-12s (the system runtime cannot do it)\n", f.name); continue; }
        const bool ours_ok = run_format(b, f, o0, o1);
        if (!ours_ok) { std::printf("FAIL %-12s our dll cannot create / fill / draw it\n", f.name); ++failed; continue; }
        double m0, m1;
        const double w0 = max_difference(r0, o0, m0), w1 = max_difference(r1, o1, m1);
        const bool good = w0 <= 3 && w1 <= 3;   // the real runtime and bgfx round block decoders and unorm conversions slightly differently
        std::printf("%s %-12s level 0: max diff %3.0f mean %.2f | level 1: max diff %3.0f mean %.2f\n", good ? "PASS" : "FAIL", f.name, w0, m0, w1, m1);
        if (!good) {
            ++failed;
            for (int i = 0; i < 3; ++i) std::printf("      sample %d real %02x%02x%02x%02x ours %02x%02x%02x%02x\n", i, r0[i * 4], r0[i * 4 + 1], r0[i * 4 + 2], r0[i * 4 + 3], o0[i * 4], o0[i * 4 + 1], o0[i * 4 + 2], o0[i * 4 + 3]);
        }
    }
    std::printf(failed ? "FAILED: %d formats differ\n" : "PASS nue_format_test\n", failed);
    return failed ? 1 : 0;
}
