// Differential test: the same DirectSound call sequences on the REAL system DirectSound (the dsound.dll of this Windows, loaded from the system
// directory) and on our COM facade + layer, comparing HRESULTs, structures and (loosely, in real time) cursor behaviour. It plays only silence
// (zero-filled buffers), never touches the system volume (no SetVolume on a primary buffer) and creates no window apart from a hidden one that
// DirectSound needs for its cooperative level. Skips itself (exit 0) when the machine has no output device.
//
// This is the ground truth for the semantics in src/device.cpp and dll/com_facade.cpp. A mismatch is a bug in our layer unless it is listed in
// `known_differences` with the reason (and in docs/LEGACY_DSOUND.md, "Deviations").
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#include <cmath>
#include <memory>

#include "com_facade.hpp"
#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;

namespace {

int g_total = 0, g_mismatch = 0, g_known = 0;
bool g_verbose = false;

struct Known {
    const char* name;
    const char* reason;
};
// Differences from the real DirectSound that are decided and documented (none so far unless listed here).
const std::vector<Known> known_differences = {};

const char* known_reason(const std::string& name) {
    for (const Known& k : known_differences)
        if (name == k.name) return k.reason;
    return nullptr;
}

void report(const std::string& name, bool equal, const std::string& detail = "") {
    ++g_total;
    if (equal) {
        if (g_verbose) std::printf("  ok    %s  %s\n", name.c_str(), detail.c_str());
        return;
    }
    if (const char* reason = known_reason(name)) {
        ++g_known;
        std::printf("  KNOWN %s  %s  (%s)\n", name.c_str(), detail.c_str(), reason);
        return;
    }
    ++g_mismatch;
    std::printf("  DIFF  %s  %s\n", name.c_str(), detail.c_str());
}

void same_hr(const std::string& name, HRESULT real, HRESULT mine) {
    char d[96];
    std::snprintf(d, sizeof(d), "real 0x%08x  mine 0x%08x", unsigned(real), unsigned(mine));
    report(name, real == mine, d);
}
void same_u32(const std::string& name, uint32_t real, uint32_t mine) {
    char d[96];
    std::snprintf(d, sizeof(d), "real %u  mine %u", real, mine);
    report(name, real == mine, d);
}

// A WAVEFORMATEX blob with every field settable, for both worlds.
struct Wfx {
    WORD tag = 1, channels = 2;
    DWORD rate = 44100;
    WORD bits = 16;
    int block = -1;      // -1: channels * bits / 8
    DWORD mask = 0;      // != 0: WAVEFORMATEXTENSIBLE (PCM sub-format) with this channel mask
    std::vector<uint8_t> blob() const {
        std::vector<uint8_t> b(mask ? 40 : 18, 0);
        auto p16 = [&](int o, uint16_t v) { std::memcpy(b.data() + o, &v, 2); };
        auto p32 = [&](int o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); };
        const uint16_t ba = uint16_t(block < 0 ? channels * bits / 8 : block);
        p16(0, mask ? 0xFFFE : tag);
        p16(2, channels);
        p32(4, rate);
        p32(8, rate * ba);
        p16(12, ba);
        p16(14, bits);
        if (mask) {
            p16(16, 22);
            p16(18, bits);
            p32(20, mask);
            const uint8_t pcm[16] = {1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
            std::memcpy(b.data() + 24, pcm, 16);
        }
        return b;
    }
};

using DirectSoundCreateFn = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

struct Pair {
    IDirectSoundBuffer* real = nullptr;
    IDirectSoundBuffer* mine = nullptr;
};

struct Rig final : com::DeviceProvider {
    HWND hwnd = nullptr;
    HMODULE system_dll = nullptr;
    IDirectSound *rds = nullptr, *mds = nullptr;
    std::unique_ptr<Engine> engine;
    ManualSink* sink = nullptr;
    std::unique_ptr<com::Facade> facade;
    nue::dsound::Device* acquire() override { return &engine->device(); }
    ~Rig() {
        if (rds) rds->Release();
        if (mds) mds->Release();
        facade.reset();
        if (hwnd) DestroyWindow(hwnd);
        if (system_dll) FreeLibrary(system_dll);
    }
    bool open() {
        hwnd = CreateWindowExW(0, L"STATIC", L"dsound differential test", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) return false;
        wchar_t path[MAX_PATH];
        GetSystemDirectoryW(path, MAX_PATH);
        std::wstring full = std::wstring(path) + L"\\dsound.dll";   // the SYSTEM dsound.dll, never a copy next to the test
        system_dll = LoadLibraryW(full.c_str());
        if (!system_dll) return false;
        auto create = reinterpret_cast<DirectSoundCreateFn>(GetProcAddress(system_dll, "DirectSoundCreate"));
        if (!create || FAILED(create(nullptr, &rds, nullptr))) return false;
        EngineConfig c;
        c.sample_rate = 48000;
        auto s = std::make_unique<ManualSink>(256, 480);
        sink = s.get();
        engine = Engine::create(c, std::move(s));
        engine->start();
        facade = std::make_unique<com::Facade>(*this);
        return facade->create(nullptr, &mds, nullptr) == DS_OK;
    }
    HRESULT coop(DWORD level, const std::string& label) {
        const HRESULT r = rds->SetCooperativeLevel(hwnd, level);
        const HRESULT m = mds->SetCooperativeLevel(hwnd, level);
        same_hr("SetCooperativeLevel " + label, r, m);
        return m;
    }
    Pair create(const std::string& label, DWORD flags, DWORD bytes, const Wfx* w) {
        DSBUFFERDESC d{};
        d.dwSize = sizeof(d);
        d.dwFlags = flags;
        d.dwBufferBytes = bytes;
        std::vector<uint8_t> storage;
        if (w) {
            storage = w->blob();
            d.lpwfxFormat = reinterpret_cast<WAVEFORMATEX*>(storage.data());
        }
        Pair p;
        const HRESULT r = rds->CreateSoundBuffer(&d, &p.real, nullptr);
        const HRESULT m = mds->CreateSoundBuffer(&d, &p.mine, nullptr);
        same_hr("CreateSoundBuffer " + label, r, m);
        if (SUCCEEDED(r) != SUCCEEDED(m)) {   // diverged: keep neither
            release(p);
        }
        return p;
    }
    static void release(Pair& p) {
        if (p.real) p.real->Release();
        if (p.mine) p.mine->Release();
        p = {};
    }
};

// Runs `call` on the real and on our object and compares the results.
#define BOTH(rig_name, pair, expr_with_b)                                                                         \
    do {                                                                                                          \
        auto fr = [&](auto* b) { return (expr_with_b); };                                                         \
        same_hr(rig_name, fr((pair).real), fr((pair).mine));                                                      \
    } while (0)

DWORD caps_flags(IDirectSoundBuffer* b, DWORD* bytes) {
    DSBCAPS c{};
    c.dwSize = sizeof(c);
    if (FAILED(b->GetCaps(&c))) return 0xFFFFFFFF;
    if (bytes) *bytes = c.dwBufferBytes;
    return c.dwFlags;
}

void run_device(Rig& r) {
    std::printf("[device and primary buffer]\n");
    for (DWORD level : {0u, 5u, 99u}) same_hr("SetCooperativeLevel invalid " + std::to_string(level), r.rds->SetCooperativeLevel(r.hwnd, level), r.mds->SetCooperativeLevel(r.hwnd, level));
    same_hr("SetCooperativeLevel null window", r.rds->SetCooperativeLevel(nullptr, DSSCL_NORMAL), r.mds->SetCooperativeLevel(nullptr, DSSCL_NORMAL));
    r.coop(DSSCL_NORMAL, "normal");
    Pair prim = r.create("primary", DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME, 0, nullptr);
    if (!prim.real || !prim.mine) return;
    Wfx stereo44;
    auto set_fmt = [&](const Wfx& w) {
        auto blob = w.blob();
        auto* wf = reinterpret_cast<WAVEFORMATEX*>(blob.data());
        return std::pair<HRESULT, HRESULT>(prim.real->SetFormat(wf), prim.mine->SetFormat(wf));
    };
    {
        auto [a, b] = set_fmt(stereo44);
        same_hr("primary SetFormat at normal level", a, b);
    }
    r.coop(DSSCL_PRIORITY, "priority");
    {
        auto [a, b] = set_fmt(stereo44);
        same_hr("primary SetFormat 44100/16/2", a, b);
    }
    struct Case { const char* name; Wfx w; };
    std::vector<Case> formats;
    { Wfx w; w.channels = 1; w.bits = 8; w.rate = 22050; formats.push_back({"22050/8/1", w}); }
    { Wfx w; w.rate = 48000; formats.push_back({"48000/16/2", w}); }
    { Wfx w; w.rate = 11025; w.bits = 8; formats.push_back({"11025/8/2", w}); }
    { Wfx w; w.channels = 0; w.block = 0; formats.push_back({"0 channels", w}); }
    { Wfx w; w.tag = 0x55; formats.push_back({"unknown tag", w}); }
    { Wfx w; w.block = 7; formats.push_back({"wrong block align", w}); }
    { Wfx w; w.channels = 9; formats.push_back({"9 channels", w}); }
    { Wfx w; w.bits = 12; w.block = 3; formats.push_back({"12 bits", w}); }
    { Wfx w; w.rate = 50; formats.push_back({"50 Hz", w}); }
    { Wfx w; w.rate = 99; formats.push_back({"99 Hz", w}); }
    { Wfx w; w.rate = 100; formats.push_back({"100 Hz", w}); }
    { Wfx w; w.rate = 192000; formats.push_back({"192000 Hz", w}); }
    { Wfx w; w.rate = 200000; formats.push_back({"200000 Hz", w}); }
    { Wfx w; w.rate = 200001; formats.push_back({"200001 Hz", w}); }
    { Wfx w; w.tag = 3; w.bits = 32; formats.push_back({"float 32", w}); }
    { Wfx w; w.bits = 24; formats.push_back({"24 bit", w}); }
    { Wfx w; w.bits = 32; formats.push_back({"32 bit", w}); }
    { Wfx w; w.channels = 6; formats.push_back({"6 channels, plain WAVEFORMATEX", w}); }
    { Wfx w; w.channels = 4; w.mask = 0x33; formats.push_back({"4 channels extensible (quad)", w}); }
    { Wfx w; w.channels = 6; w.mask = 0x3F; formats.push_back({"6 channels extensible (5.1)", w}); }
    { Wfx w; w.channels = 8; w.mask = 0x63F; formats.push_back({"8 channels extensible (7.1)", w}); }
    { Wfx w; w.channels = 3; w.mask = 0x7; formats.push_back({"3 channels extensible", w}); }
    { Wfx w; w.channels = 9; w.mask = 0x7FF; formats.push_back({"9 channels extensible", w}); }
    for (auto& c : formats) {
        auto [a, b] = set_fmt(c.w);
        same_hr(std::string("primary SetFormat ") + c.name, a, b);
    }
    set_fmt(stereo44);   // back to a known format, then GetFormat
    WAVEFORMATEX gr{}, gm{};
    DWORD wr = 0, wm = 0;
    same_hr("primary GetFormat", prim.real->GetFormat(&gr, sizeof(gr), &wr), prim.mine->GetFormat(&gm, sizeof(gm), &wm));
    same_u32("primary GetFormat size written", wr, wm);
    same_u32("primary GetFormat rate", gr.nSamplesPerSec, gm.nSamplesPerSec);
    // Second request for the primary buffer yields the same object.
    IDirectSoundBuffer *again_r = nullptr, *again_m = nullptr;
    DSBUFFERDESC pd{};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME;
    const HRESULT rr = r.rds->CreateSoundBuffer(&pd, &again_r, nullptr);
    const HRESULT mm = r.mds->CreateSoundBuffer(&pd, &again_m, nullptr);
    same_hr("primary requested twice", rr, mm);
    if (SUCCEEDED(rr)) {
        report("primary twice is the same object (real)", again_r == prim.real);
        report("primary twice is the same object (mine)", again_m == prim.mine);
        again_r->Release();
    }
    if (SUCCEEDED(mm)) again_m->Release();
    BOTH("primary Play without looping", prim, b->Play(0, 0, 0));
    BOTH("primary Play looping", prim, b->Play(0, 0, DSBPLAY_LOOPING));
    BOTH("primary SetFrequency", prim, b->SetFrequency(22050));
    // Device caps.
    DSCAPS rc{}, mc{};
    rc.dwSize = mc.dwSize = sizeof(DSCAPS);
    same_hr("GetCaps", r.rds->GetCaps(&rc), r.mds->GetCaps(&mc));
    std::printf("        (info) real DSCAPS flags 0x%lx min rate %lu max rate %lu primary buffers %lu; mine 0x%lx %lu %lu %lu\n", rc.dwFlags, rc.dwMinSecondarySampleRate,
                rc.dwMaxSecondarySampleRate, rc.dwPrimaryBuffers, mc.dwFlags, mc.dwMinSecondarySampleRate, mc.dwMaxSecondarySampleRate, mc.dwPrimaryBuffers);
    same_u32("GetCaps format flags (low 12 bits, formats the device accepts)", rc.dwFlags & 0xF1F, mc.dwFlags & 0xF1F);
    DSCAPS bad{};
    bad.dwSize = 40;
    same_hr("GetCaps wrong dwSize", r.rds->GetCaps(&bad), r.mds->GetCaps(&bad));
    same_hr("GetSpeakerConfig null", r.rds->GetSpeakerConfig(nullptr), r.mds->GetSpeakerConfig(nullptr));
    same_hr("Initialize after DirectSoundCreate", r.rds->Initialize(nullptr), r.mds->Initialize(nullptr));
    Rig::release(prim);
}

void run_secondary_creation(Rig& r) {
    std::printf("[secondary buffer creation]\n");
    r.coop(DSSCL_PRIORITY, "priority");
    struct C { const char* name; DWORD flags, bytes; Wfx w; bool format = true; };
    std::vector<C> cases;
    auto add = [&](const char* n, DWORD f, DWORD b, Wfx w, bool fmt = true) { cases.push_back({n, f, b, w, fmt}); };
    Wfx s22; s22.rate = 22050;
    Wfx m22; m22.channels = 1; m22.rate = 22050;
    add("plain 4096 stereo", 0, 4096, s22);
    add("no format", 0, 4096, s22, false);
    add("3 bytes", 0, 3, s22);
    add("0 bytes", 0, 0, s22);
    add("4 bytes (minimum)", 0, 4, s22);
    add("4001 bytes (not a whole frame multiple)", 0, 4001, s22);
    add("size 7 (less than a frame twice)", 0, 7, s22);
    add("CTRL3D stereo", DSBCAPS_CTRL3D, 4096, s22);
    add("CTRL3D mono", DSBCAPS_CTRL3D, 4096, m22);
    add("CTRL3D + CTRLPAN mono", DSBCAPS_CTRL3D | DSBCAPS_CTRLPAN, 4096, m22);
    add("LOCHARDWARE", DSBCAPS_LOCHARDWARE, 4096, s22);
    add("LOCSOFTWARE", DSBCAPS_LOCSOFTWARE, 4096, s22);
    add("LOCHARDWARE + LOCSOFTWARE", DSBCAPS_LOCHARDWARE | DSBCAPS_LOCSOFTWARE, 4096, s22);
    add("STATIC", DSBCAPS_STATIC, 4096, s22);
    add("all controls", DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPOSITIONNOTIFY, 4096, s22);
    add("GETCURRENTPOSITION2 + GLOBALFOCUS (a streaming ring)", DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS, 88192, Wfx{});
    { Wfx w; w.tag = 3; w.bits = 32; add("float 32-bit", 0, 4096, w); }
    { Wfx w; w.bits = 24; add("24-bit", 0, 4096, w); }
    { Wfx w; w.bits = 32; add("32-bit integer", 0, 4096, w); }
    { Wfx w; w.bits = 8; w.channels = 1; w.rate = 8000; add("8000/8/1", 0, 800, w); }
    { Wfx w; w.rate = 96000; add("96000/16/2", 0, 4096, w); }
    { Wfx w; w.rate = 50; add("50 Hz", 0, 4096, w); }
    { Wfx w; w.rate = 300000; add("300000 Hz", 0, 4096, w); }
    { Wfx w; w.channels = 6; add("6 channels plain WAVEFORMATEX", 0, 4096 * 3, w); }
    { Wfx w; w.channels = 6; w.mask = 0x3F; add("6 channels extensible", 0, 4096 * 3, w); }
    { Wfx w; w.channels = 4; w.mask = 0x33; add("4 channels extensible", 0, 4096 * 2, w); }
    { Wfx w; w.channels = 8; w.mask = 0x63F; add("8 channels extensible", 0, 4096 * 4, w); }
    { Wfx w; w.channels = 6; w.mask = 0x3F; add("6 channels extensible, 0x18000 (a 5.1 ring)", DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS, 22048 * 12, w); }
    { Wfx w; w.channels = 0; w.block = 0; add("0 channels", 0, 4096, w); }
    { Wfx w; w.bits = 12; w.block = 3; add("12 bits", 0, 4096, w); }
    { Wfx w; w.block = 7; add("wrong block align", 0, 4096, w); }
    { Wfx w; w.tag = 0x55; add("unknown tag", 0, 4096, w); }
    for (C& c : cases) {
        Pair p = r.create(c.name, c.flags, c.bytes, c.format ? &c.w : nullptr);
        if (p.real && p.mine) {
            DWORD rb = 0, mb = 0;
            const DWORD rf = caps_flags(p.real, &rb), mf = caps_flags(p.mine, &mb);
            same_u32(std::string("GetCaps bytes: ") + c.name, rb, mb);
            same_u32(std::string("GetCaps flags: ") + c.name, rf, mf);
        }
        Rig::release(p);
    }
}

void run_controls(Rig& r) {
    std::printf("[controls]\n");
    r.coop(DSSCL_PRIORITY, "priority");
    Wfx w;
    w.rate = 22050;
    Pair none = r.create("no controls", 0, 4096, &w);
    Pair all = r.create("all controls", DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY, 4096, &w);
    if (!none.real || !none.mine || !all.real || !all.mine) return;
    for (Pair* p : {&none, &all}) {
        const std::string tag = p == &none ? "(no flag) " : "(flag) ";
        LONG lr = 0, lm = 0;
        DWORD hr_ = 0, hm = 0;
        for (int v : {0, -1, -600, -10000, -10001, 1, 100}) BOTH(tag + "SetVolume " + std::to_string(v), *p, b->SetVolume(v));
        same_hr(tag + "GetVolume", p->real->GetVolume(&lr), p->mine->GetVolume(&lm));
        if (p == &all) same_u32("GetVolume value", uint32_t(lr), uint32_t(lm));
        for (int v : {0, 5000, -10000, 10000, -10001, 10001}) BOTH(tag + "SetPan " + std::to_string(v), *p, b->SetPan(v));
        same_hr(tag + "GetPan", p->real->GetPan(&lr), p->mine->GetPan(&lm));
        if (p == &all) same_u32("GetPan value", uint32_t(lr), uint32_t(lm));
        for (unsigned f : {0u, 50u, 99u, 100u, 101u, 11025u, 44100u, 100000u, 200000u, 200001u, 1000000u}) BOTH(tag + "SetFrequency " + std::to_string(f), *p, b->SetFrequency(f));
        same_hr(tag + "GetFrequency", p->real->GetFrequency(&hr_), p->mine->GetFrequency(&hm));
        if (p == &all) same_u32("GetFrequency value (after SetFrequency 1000000, last valid 200000)", hr_, hm);
        auto blob = w.blob();
        BOTH(tag + "SetFormat on a secondary", *p, b->SetFormat(reinterpret_cast<WAVEFORMATEX*>(blob.data())));
    }
    all.real->SetFrequency(0);
    all.mine->SetFrequency(0);
    DWORD a = 0, b = 0;
    all.real->GetFrequency(&a);
    all.mine->GetFrequency(&b);
    same_u32("SetFrequency(0) = original rate", a, b);
    Rig::release(none);
    Rig::release(all);
}

void run_lock(Rig& r) {
    std::printf("[lock and unlock]\n");
    r.coop(DSSCL_PRIORITY, "priority");
    Wfx w;
    w.rate = 22050;
    Pair b = r.create("lock buffer", 0, 4000, &w);
    if (!b.real || !b.mine) return;
    struct L { const char* name; DWORD off, bytes, flags; };
    const L cases[] = {
        {"0,0", 0, 0, 0},
        {"0,4000 (whole)", 0, 4000, 0},
        {"0,4001 (too long)", 0, 4001, 0},
        {"4000,4 (offset at the end)", 4000, 4, 0},
        {"4001,4 (offset past the end)", 4001, 4, 0},
        {"3996,8 (wraps)", 3996, 8, 0},
        {"3999,2 (unaligned, wraps)", 3999, 2, 0},
        {"1,3 (unaligned)", 1, 3, 0},
        {"ENTIREBUFFER", 123, 5, DSBLOCK_ENTIREBUFFER},
        {"FROMWRITECURSOR stopped", 100, 8, DSBLOCK_FROMWRITECURSOR},
        {"unknown flag", 0, 8, 0x80},
    };
    for (const L& c : cases) {
        void *rp1 = nullptr, *rp2 = nullptr, *mp1 = nullptr, *mp2 = nullptr;
        DWORD rl1 = 0, rl2 = 0, ml1 = 0, ml2 = 0;
        const HRESULT rr = b.real->Lock(c.off, c.bytes, &rp1, &rl1, &rp2, &rl2, c.flags);
        const HRESULT mm = b.mine->Lock(c.off, c.bytes, &mp1, &ml1, &mp2, &ml2, c.flags);
        same_hr(std::string("Lock ") + c.name, rr, mm);
        if (SUCCEEDED(rr) && SUCCEEDED(mm)) {
            same_u32(std::string("Lock bytes1 ") + c.name, rl1, ml1);
            same_u32(std::string("Lock bytes2 ") + c.name, rl2, ml2);
            same_hr(std::string("Unlock after ") + c.name, b.real->Unlock(rp1, rl1, rp2, rl2), b.mine->Unlock(mp1, ml1, mp2, ml2));
        } else {
            if (SUCCEEDED(rr)) b.real->Unlock(rp1, rl1, rp2, rl2);
            if (SUCCEEDED(mm)) b.mine->Unlock(mp1, ml1, mp2, ml2);
        }
    }
    // Locking twice, mismatched Unlock, Unlock without Lock.
    void *ra1 = nullptr, *ra2 = nullptr, *rb1 = nullptr, *rb2 = nullptr, *ma1 = nullptr, *ma2 = nullptr, *mb1 = nullptr, *mb2 = nullptr;
    DWORD ral1 = 0, ral2 = 0, rbl1 = 0, rbl2 = 0, mal1 = 0, mal2 = 0, mbl1 = 0, mbl2 = 0;
    same_hr("first Lock", b.real->Lock(0, 400, &ra1, &ral1, &ra2, &ral2, 0), b.mine->Lock(0, 400, &ma1, &mal1, &ma2, &mal2, 0));
    same_hr("second Lock while locked", b.real->Lock(1000, 400, &rb1, &rbl1, &rb2, &rbl2, 0), b.mine->Lock(1000, 400, &mb1, &mbl1, &mb2, &mbl2, 0));
    same_hr("Unlock with wrong pointer", b.real->Unlock(static_cast<char*>(ra1) + 4, 396, nullptr, 0), b.mine->Unlock(static_cast<char*>(ma1) + 4, 396, nullptr, 0));
    same_hr("Unlock correct", b.real->Unlock(ra1, ral1, nullptr, 0), b.mine->Unlock(ma1, mal1, nullptr, 0));
    same_hr("Unlock without Lock", b.real->Unlock(ra1, ral1, nullptr, 0), b.mine->Unlock(ma1, mal1, nullptr, 0));
    void *rp = nullptr, *mp = nullptr;
    DWORD rl = 0, ml = 0;
    same_hr("Lock without second region where it wraps", b.real->Lock(3900, 400, &rp, &rl, nullptr, nullptr, 0), b.mine->Lock(3900, 400, &mp, &ml, nullptr, nullptr, 0));
    same_u32("  bytes1 of that Lock", rl, ml);
    Rig::release(b);
}

void run_transport_static(Rig& r) {
    std::printf("[status, position, restore, duplicate, notifications]\n");
    r.coop(DSSCL_PRIORITY, "priority");
    Wfx w;
    w.rate = 22050;
    Pair b = r.create("transport buffer", DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2, 22050 * 4, &w);
    if (!b.real || !b.mine) return;
    b.real->SetVolume(-10000);   // silence for sure (the data is zero anyway)
    b.mine->SetVolume(-10000);
    DWORD sr = 0, sm = 0, pr = 0, pm = 0, wr = 0, wm = 0;
    same_hr("GetStatus stopped", b.real->GetStatus(&sr), b.mine->GetStatus(&sm));
    same_u32("status of a stopped buffer", sr, sm);
    same_hr("GetCurrentPosition stopped", b.real->GetCurrentPosition(&pr, &wr), b.mine->GetCurrentPosition(&pm, &wm));
    same_u32("play cursor of a new buffer", pr, pm);
    same_u32("write cursor of a stopped buffer", wr, wm);
    same_hr("GetCurrentPosition both null", b.real->GetCurrentPosition(nullptr, nullptr), b.mine->GetCurrentPosition(nullptr, nullptr));
    BOTH("SetCurrentPosition 4000", b, b->SetCurrentPosition(4000));
    BOTH("SetCurrentPosition 4002 (unaligned)", b, b->SetCurrentPosition(4002));
    BOTH("SetCurrentPosition = size", b, b->SetCurrentPosition(22050 * 4));
    b.real->SetCurrentPosition(4000);
    b.mine->SetCurrentPosition(4000);
    b.real->GetCurrentPosition(&pr, &wr);
    b.mine->GetCurrentPosition(&pm, &wm);
    same_u32("play cursor after SetCurrentPosition(4000)", pr, pm);
    BOTH("Play reserved1 != 0", b, b->Play(1, 0, 0));
    BOTH("Play priority != 0 without LOCDEFER", b, b->Play(0, 5, 0));
    BOTH("Play unknown flags", b, b->Play(0, 0, 0x4000));
    BOTH("Restore of a healthy buffer", b, b->Restore());
    same_u32("buffer caps flags", caps_flags(b.real, nullptr), caps_flags(b.mine, nullptr));
    IDirectSoundBuffer *dr = nullptr, *dm = nullptr;
    same_hr("DuplicateSoundBuffer", r.rds->DuplicateSoundBuffer(b.real, &dr), r.mds->DuplicateSoundBuffer(b.mine, &dm));
    if (dr) dr->Release();
    if (dm) dm->Release();
    // Notification positions: needs CTRLPOSITIONNOTIFY, refused while playing.
    IDirectSoundNotify *nr = nullptr, *nm = nullptr;
    same_hr("QueryInterface IDirectSoundNotify", b.real->QueryInterface(IID_IDirectSoundNotify, reinterpret_cast<void**>(&nr)),
            b.mine->QueryInterface(IID_IDirectSoundNotify, reinterpret_cast<void**>(&nm)));
    if (nr && nm) {
        HANDLE ev_r = CreateEventW(nullptr, FALSE, FALSE, nullptr), ev_m = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DSBPOSITIONNOTIFY pos_r[2] = {{22050 * 4 / 2, ev_r}, {DSBPN_OFFSETSTOP, ev_r}}, pos_m[2] = {{22050 * 4 / 2, ev_m}, {DSBPN_OFFSETSTOP, ev_m}};
        same_hr("SetNotificationPositions", nr->SetNotificationPositions(2, pos_r), nm->SetNotificationPositions(2, pos_m));
        same_hr("SetNotificationPositions count 0", nr->SetNotificationPositions(0, nullptr), nm->SetNotificationPositions(0, nullptr));
        nr->SetNotificationPositions(2, pos_r);
        nm->SetNotificationPositions(2, pos_m);
        b.real->SetCurrentPosition(0);
        b.mine->SetCurrentPosition(0);
        b.real->Play(0, 0, DSBPLAY_LOOPING);
        b.mine->Play(0, 0, DSBPLAY_LOOPING);
        same_hr("SetNotificationPositions while playing", nr->SetNotificationPositions(2, pos_r), nm->SetNotificationPositions(2, pos_m));
        // The position passes the middle after 0.5 s.
        const DWORD waited = WaitForSingleObject(ev_r, 1500);
        r.sink->advance(48000 / 2 + 2000);   // half a second of our time (+ latency)
        report("notification at the middle fires (real)", waited == WAIT_OBJECT_0);
        report("notification at the middle fires (mine)", WaitForSingleObject(ev_m, 0) == WAIT_OBJECT_0);
        b.real->Stop();
        b.mine->Stop();
        report("stop notification (real)", WaitForSingleObject(ev_r, 500) == WAIT_OBJECT_0);
        r.sink->advance(1);
        report("stop notification (mine)", WaitForSingleObject(ev_m, 0) == WAIT_OBJECT_0);
        CloseHandle(ev_r);
        CloseHandle(ev_m);
    }
    if (nr) nr->Release();
    if (nm) nm->Release();
    Rig::release(b);
}

// Real time: cursors of a playing looping buffer. The real device's behaviour is printed; the checks are loose (a driver-dependent margin).
void run_realtime(Rig& r) {
    std::printf("[cursors in real time]\n");
    r.coop(DSSCL_PRIORITY, "priority");
    Wfx w;
    w.rate = 44100;
    const DWORD bytes = 44100 * 4;   // 1 s ring
    Pair b = r.create("1 s ring", DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME, bytes, &w);
    if (!b.real || !b.mine) return;
    b.real->SetVolume(-10000);
    b.mine->SetVolume(-10000);
    BOTH("Play looping", b, b->Play(0, 0, DSBPLAY_LOOPING));
    std::printf("        t(ms)   real play/write (frames)      mine play/write (frames)\n");
    double real_margin_sum = 0, mine_margin_sum = 0;
    DWORD last_real_play = 0, last_mine_play = 0;
    bool real_monotone = true, mine_monotone = true;
    const ULONGLONG t0 = GetTickCount64();
    for (int step = 1; step <= 8; ++step) {
        Sleep(100);
        const ULONGLONG elapsed = GetTickCount64() - t0;
        DWORD play = 0, write = 0, mp = 0, mw = 0;
        b.real->GetCurrentPosition(&play, &write);
        r.sink->advance(480 * 10);   // 100 ms of our time per step
        b.mine->GetCurrentPosition(&mp, &mw);
        std::printf("        %5llu   %8lu / %-8lu            %8lu / %-8lu\n", static_cast<unsigned long long>(elapsed), play / 4, write / 4, mp / 4, mw / 4);
        real_margin_sum += double((write + bytes - play) % bytes) / 4.0 / 44.1;
        mine_margin_sum += double((mw + bytes - mp) % bytes) / 4.0 / 44.1;
        real_monotone = real_monotone && play >= last_real_play;
        mine_monotone = mine_monotone && mp >= last_mine_play;
        last_real_play = play;
        last_mine_play = mp;
    }
    std::printf("        mean write-play margin: real %.1f ms, mine %.1f ms\n", real_margin_sum / 8, mine_margin_sum / 8);
    report("play cursor advances monotonically (real)", real_monotone);
    report("play cursor advances monotonically (mine)", mine_monotone);
    DWORD play = 0, mp = 0;
    b.real->GetCurrentPosition(&play, nullptr);
    b.mine->GetCurrentPosition(&mp, nullptr);
    report("real play cursor is near 0.8 s after 8 x 100 ms (within 150 ms)", std::fabs(double(play) / 4.0 / 44.1 - 800.0) < 150.0);
    report("mine play cursor is near 0.8 s (within 60 ms)", std::fabs(double(mp) / 4.0 / 44.1 - 800.0) < 60.0);
    // Stop keeps the position; a stopped buffer does not move.
    BOTH("Stop", b, b->Stop());
    DWORD p1 = 0, p2 = 0;
    b.real->GetCurrentPosition(&p1, nullptr);
    Sleep(150);
    b.real->GetCurrentPosition(&p2, nullptr);
    report("stopped buffer does not move (real)", p1 == p2);
    r.sink->advance(480 * 15);
    b.mine->GetCurrentPosition(&p1, nullptr);
    r.sink->advance(480 * 15);
    b.mine->GetCurrentPosition(&p2, nullptr);
    report("stopped buffer does not move (mine)", p1 == p2);
    DWORD st_r = 0, st_m = 0;
    b.real->GetStatus(&st_r);
    b.mine->GetStatus(&st_m);
    same_u32("status after Stop", st_r, st_m);
    Rig::release(b);
    // A one-shot buffer stops by itself and goes back to the start.
    Pair shot = r.create("100 ms one-shot", DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME, 4410 * 4, &w);
    if (!shot.real || !shot.mine) return;
    shot.real->SetVolume(-10000);
    shot.mine->SetVolume(-10000);
    shot.real->Play(0, 0, 0);
    shot.mine->Play(0, 0, 0);
    Sleep(400);
    r.sink->advance(480 * 40);
    DWORD rs = 0, rp = 0, ms = 0, mpp = 0;
    shot.real->GetStatus(&rs);
    shot.real->GetCurrentPosition(&rp, nullptr);
    shot.mine->GetStatus(&ms);
    shot.mine->GetCurrentPosition(&mpp, nullptr);
    same_u32("one-shot status after the end", rs, ms);
    same_u32("one-shot play cursor after the end", rp, mpp);
    Rig::release(shot);
}

// A streaming client's startup and polling, call for call: priority level, primary buffer with flags 1, SetFormat probes with 1, 2, 4 and 6
// channels, a 0.5 s secondary ring with flags 0x18000, one Lock of the whole ring with NO second region, memset, Unlock(p, n, 0, 0),
// Play(0, 0, DSBPLAY_LOOPING), then GetStatus and GetCurrentPosition(&play, NULL) polls.
void run_streaming_sequence(Rig& r) {
    std::printf("[a streaming client's startup sequence]\n");
    r.coop(DSSCL_PRIORITY, "priority");
    Pair prim = r.create("primary (flags 1)", DSBCAPS_PRIMARYBUFFER, 0, nullptr);
    if (!prim.real || !prim.mine) return;
    for (uint16_t channels : {1, 2, 4, 6}) {
        Wfx f;
        f.channels = channels;
        f.mask = channels == 6 ? 0x3F : channels == 4 ? 0x33 : 0;
        auto blob = f.blob();
        BOTH("probe SetFormat " + std::to_string(channels) + " channels", prim, b->SetFormat(reinterpret_cast<WAVEFORMATEX*>(blob.data())));
    }
    Wfx f;   // the default the client ends with: 2 channels, 44.1 kHz, 16 bits
    {
        auto blob = f.blob();
        BOTH("final SetFormat", prim, b->SetFormat(reinterpret_cast<WAVEFORMATEX*>(blob.data())));
    }
    const DWORD frames = (44100 * 500 / 1000) & ~15u, bytes = frames * 4;
    Pair ring = r.create("ring (0x18000)", DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS, bytes, &f);
    if (!ring.real || !ring.mine) {
        Rig::release(prim);
        return;
    }
    void *rp1 = nullptr, *mp1 = nullptr;
    DWORD rl1 = 0, ml1 = 0;
    same_hr("Lock whole ring, no second region", ring.real->Lock(0, bytes, &rp1, &rl1, nullptr, nullptr, 0), ring.mine->Lock(0, bytes, &mp1, &ml1, nullptr, nullptr, 0));
    same_u32("Lock length", rl1, ml1);
    std::memset(rp1, 0, rl1);
    std::memset(mp1, 0, ml1);
    same_hr("Unlock(p, n, 0, 0)", ring.real->Unlock(rp1, rl1, nullptr, 0), ring.mine->Unlock(mp1, ml1, nullptr, 0));
    BOTH("Play looping", ring, b->Play(0, 0, DSBPLAY_LOOPING));
    DWORD sr = 0, sm = 0;
    ring.real->GetStatus(&sr);
    ring.mine->GetStatus(&sm);
    same_u32("GetStatus while playing", sr, sm);
    DWORD last_real = 0, last_mine = 0;
    bool real_ok = true, mine_ok = true;
    for (int poll = 0; poll < 20; ++poll) {   // 20 polls at 10 ms, as the audio thread does
        Sleep(10);
        DWORD rplay = 0, mplay = 0;
        same_hr("GetCurrentPosition(&play, NULL)", ring.real->GetCurrentPosition(&rplay, nullptr), ring.mine->GetCurrentPosition(&mplay, nullptr));
        r.sink->advance(480);
        ring.mine->GetCurrentPosition(&mplay, nullptr);
        if (poll > 0) {
            real_ok = real_ok && (rplay >= last_real || last_real - rplay > bytes / 2);
            mine_ok = mine_ok && (mplay >= last_mine || last_mine - mplay > bytes / 2);
        }
        last_real = rplay;
        last_mine = mplay;
    }
    report("polls: the real play cursor never goes backwards", real_ok);
    report("polls: our play cursor never goes backwards", mine_ok);
    BOTH("Restore on a healthy buffer", ring, b->Restore());
    Rig::release(ring);
    Rig::release(prim);
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "-v") g_verbose = true;
    Rig rig;
    if (!rig.open()) {
        std::printf("SKIP: no usable DirectSound output device on this machine (the system DirectSoundCreate failed); nothing compared\n");
        return 0;
    }
    run_device(rig);
    run_secondary_creation(rig);
    run_controls(rig);
    run_lock(rig);
    run_transport_static(rig);
    run_realtime(rig);
    run_streaming_sequence(rig);
    std::printf("%s: %d comparisons, %d differences, %d known differences\n", g_mismatch ? "FAIL" : "PASS", g_total, g_mismatch, g_known);
    return g_mismatch ? 1 : 0;
}
