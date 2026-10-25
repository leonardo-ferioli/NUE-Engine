// nue_defaults_test: the device state right after CreateDevice (and after Reset), read back with GetRenderState / GetSamplerState / GetTextureStageState / GetTransform / GetViewport / GetMaterial,
// from the system Direct3D 9 runtime and from the drop-in dll, compared value by value. A game that never sets a state relies on its default; a launcher that skips repeated state calls relies on it too.
//
//   nue_defaults_test <path to our d3d9.dll>
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
using CreateFn = IDirect3D9*(WINAPI*)(UINT);

struct Dump { std::vector<DWORD> rs, ss, tss; std::vector<float> misc; };

bool make_device(CreateFn create, IDirect3DDevice9*& device, HWND& window, D3DPRESENT_PARAMETERS& pp) {
    window = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "STATIC", "nue defaults test", WS_OVERLAPPED, 100, 100, 320, 240, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);   // never shown
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    if (!d3d || !window) return false;
    pp = {};
    pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.Windowed = TRUE; pp.hDeviceWindow = window; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    return SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &device)) && device;
}

Dump dump(IDirect3DDevice9* d) {
    Dump r;
    for (DWORD i = 0; i < 210; ++i) { DWORD v = 0xDEADBEEF; if (FAILED(d->GetRenderState(D3DRENDERSTATETYPE(i), &v))) v = 0xDEADBEEF; r.rs.push_back(v); }
    for (DWORD s = 0; s < 16; ++s) for (DWORD t = 1; t <= 13; ++t) { DWORD v = 0xDEADBEEF; if (FAILED(d->GetSamplerState(s, D3DSAMPLERSTATETYPE(t), &v))) v = 0xDEADBEEF; r.ss.push_back(v); }
    for (DWORD s = 0; s < 8; ++s) for (DWORD t = 1; t <= 32; ++t) { DWORD v = 0xDEADBEEF; if (FAILED(d->GetTextureStageState(s, D3DTEXTURESTAGESTATETYPE(t), &v))) v = 0xDEADBEEF; r.tss.push_back(v); }
    D3DVIEWPORT9 vp{}; d->GetViewport(&vp);
    r.misc = {float(vp.X), float(vp.Y), float(vp.Width), float(vp.Height), vp.MinZ, vp.MaxZ};
    D3DMATERIAL9 m{}; d->GetMaterial(&m);
    r.misc.insert(r.misc.end(), {m.Diffuse.r, m.Diffuse.g, m.Diffuse.b, m.Diffuse.a, m.Ambient.r, m.Ambient.g, m.Ambient.b, m.Ambient.a, m.Specular.r, m.Emissive.r, m.Power});
    for (D3DTRANSFORMSTATETYPE t : {D3DTS_VIEW, D3DTS_PROJECTION, D3DTS_TEXTURE0, D3DTS_WORLD}) { D3DMATRIX mm{}; d->GetTransform(t, &mm); const float* f = &mm._11; for (int i = 0; i < 16; ++i) r.misc.push_back(f[i]); }
    return r;
}

int compare(const char* when, const Dump& real, const Dump& ours) {
    int differences = 0;
    for (size_t i = 0; i < real.rs.size(); ++i) if (real.rs[i] != ours.rs[i] && real.rs[i] != 0xDEADBEEF && real.rs[i] != 0xBAADCAFE) { std::printf("  [%s] render state %zu: real %08x ours %08x\n", when, i, real.rs[i], ours.rs[i]); ++differences; }
    for (size_t i = 0; i < real.ss.size(); ++i) if (real.ss[i] != ours.ss[i] && real.ss[i] != 0xDEADBEEF && real.ss[i] != 0xBAADCAFE) { std::printf("  [%s] sampler %zu state %zu: real %08x ours %08x\n", when, i / 13, i % 13 + 1, real.ss[i], ours.ss[i]); ++differences; }
    for (size_t i = 0; i < real.tss.size(); ++i) if (real.tss[i] != ours.tss[i] && real.tss[i] != 0xDEADBEEF && real.tss[i] != 0xBAADCAFE) { std::printf("  [%s] stage %zu state %zu: real %08x ours %08x\n", when, i / 32, i % 32 + 1, real.tss[i], ours.tss[i]); ++differences; }
    for (size_t i = 0; i < real.misc.size(); ++i) if (std::memcmp(&real.misc[i], &ours.misc[i], 4) != 0 && real.misc[i] != ours.misc[i]) { std::printf("  [%s] misc value %zu: real %g ours %g\n", when, i, real.misc[i], ours.misc[i]); ++differences; }
    return differences;
}
}  // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();
    if (argc < 2) { std::printf("usage: nue_defaults_test <our d3d9.dll>\n"); return 2; }
    HMODULE ours = LoadLibraryA(argv[1]);
    char system[MAX_PATH];
    GetSystemDirectoryA(system, MAX_PATH);
    HMODULE real = LoadLibraryA((std::string(system) + "\\d3d9.dll").c_str());
    if (!ours || !real) { std::printf("cannot load the dlls\n"); return 2; }
    IDirect3DDevice9 *a = nullptr, *b = nullptr;
    HWND wa = nullptr, wb = nullptr;
    D3DPRESENT_PARAMETERS pa, pb;
    if (!make_device(reinterpret_cast<CreateFn>(GetProcAddress(real, "Direct3DCreate9")), a, wa, pa)) { std::printf("the system runtime cannot make a device\n"); return 2; }
    if (!make_device(reinterpret_cast<CreateFn>(GetProcAddress(ours, "Direct3DCreate9")), b, wb, pb)) { std::printf("our dll cannot make a device\n"); return 2; }
    if (argc > 2 && std::string(argv[2]) == "--table") {   // the real runtime's defaults as C++ tables (for the dll's own shadow of the state)
        const Dump d = dump(a);
        std::printf("render states {state, value}:\n");
        for (size_t i = 0; i < d.rs.size(); ++i) if (d.rs[i] != 0xDEADBEEF && d.rs[i] != 0xBAADCAFE && d.rs[i] != 0) std::printf("{%zu, 0x%08x}, ", i, d.rs[i]);
        std::printf("\nsampler states {type, value} for sampler 0:\n");
        for (size_t t = 0; t < 13; ++t) if (d.ss[t] != 0xDEADBEEF && d.ss[t] != 0) std::printf("{%zu, 0x%08x}, ", t + 1, d.ss[t]);
        std::printf("\nstage states {type, value} per stage:\n");
        for (size_t st = 0; st < 8; ++st) { std::printf("stage %zu: ", st); for (size_t t = 0; t < 32; ++t) if (d.tss[st * 32 + t] != 0xDEADBEEF && d.tss[st * 32 + t] != 0xBAADCAFE && d.tss[st * 32 + t] != 0) std::printf("{%zu, 0x%08x}, ", t + 1, d.tss[st * 32 + t]); std::printf("\n"); }
        return 0;
    }
    int differences = compare("after CreateDevice", dump(a), dump(b));
    // change some states, Reset, and compare again: Reset returns every state to its default
    for (IDirect3DDevice9* d : {a, b}) { d->SetRenderState(D3DRS_ZENABLE, FALSE); d->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW); d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR); d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_ADD); }
    a->Reset(&pa); b->Reset(&pb);
    differences += compare("after Reset", dump(a), dump(b));
    std::printf(differences ? "FAILED: %d defaults differ\n" : "PASS nue_defaults_test\n", differences);
    return differences ? 1 : 0;
}
