// The built dsound.dll as a game sees it: loaded by path, called by ordinal and by name, headless (NUE_DSOUND_HEADLESS=1: the NUE null backend, no sound
// device). Usage: nue_dsound_dll_test <path to dsound.dll>. This is the only test that goes through the real exports and the real DllMain.
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>

#include <cstdio>
#include <string>

#include "test_support.hpp"

namespace {

using CreateFn = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

void run(const wchar_t* path) {
    SetEnvironmentVariableA("NUE_DSOUND_HEADLESS", "1");
    HMODULE dll = LoadLibraryW(path);
    CHECK(dll != nullptr);
    if (!dll) return;
    // The ordinals of the system dsound.dll: a game that imports by ordinal 1 (DirectSoundCreate) binds to the same function as by name.
    auto by_name = [&](const char* n) { return GetProcAddress(dll, n); };
    auto by_ordinal = [&](int o) { return GetProcAddress(dll, MAKEINTRESOURCEA(o)); };
    CHECK(by_name("DirectSoundCreate") && by_ordinal(1) == by_name("DirectSoundCreate"));
    CHECK(by_ordinal(2) == by_name("DirectSoundEnumerateA") && by_ordinal(3) == by_name("DirectSoundEnumerateW"));
    CHECK(by_ordinal(4) == by_name("DllCanUnloadNow") && by_ordinal(5) == by_name("DllGetClassObject"));
    CHECK(by_ordinal(6) == by_name("DirectSoundCaptureCreate") && by_ordinal(9) == by_name("GetDeviceID"));
    CHECK(by_ordinal(11) == by_name("DirectSoundCreate8") && by_ordinal(12) == by_name("DirectSoundCaptureCreate8"));
    auto create = reinterpret_cast<CreateFn>(by_ordinal(1));
    if (!create) return;

    IDirectSound* ds = nullptr;
    CHECK(create(nullptr, &ds, nullptr) == DS_OK && ds != nullptr);
    if (!ds) return;
    CHECK(ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY) == DS_OK);
    DSBUFFERDESC pd = {};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DSBCAPS_PRIMARYBUFFER;
    IDirectSoundBuffer *primary = nullptr, *ring = nullptr;
    CHECK(ds->CreateSoundBuffer(&pd, &primary, nullptr) == DS_OK);
    WAVEFORMATEX wf = {};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100 * 4;
    CHECK(primary && primary->SetFormat(&wf) == DS_OK);
    DSBUFFERDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    sd.dwBufferBytes = 22048 * 4;
    sd.lpwfxFormat = &wf;
    CHECK(ds->CreateSoundBuffer(&sd, &ring, nullptr) == DS_OK);
    if (ring) {
        void* p = nullptr;
        DWORD n = 0;
        CHECK(ring->Lock(0, sd.dwBufferBytes, &p, &n, nullptr, nullptr, 0) == DS_OK && n == sd.dwBufferBytes);
        std::memset(p, 0, n);
        CHECK(ring->Unlock(p, n, nullptr, 0) == DS_OK);
        CHECK(ring->Play(0, 0, DSBPLAY_LOOPING) == DS_OK);
        // The null backend's clock runs in real time here: the play cursor must move (the client rebuilds its audio after 100 identical polls).
        DWORD first = 0, last = 0;
        ring->GetCurrentPosition(&first, nullptr);
        bool moved = false;
        for (int i = 0; i < 60 && !moved; ++i) {
            Sleep(10);
            ring->GetCurrentPosition(&last, nullptr);
            moved = last != first;
        }
        CHECK(moved);
        DWORD status = 0;
        CHECK(ring->GetStatus(&status) == DS_OK && (status & DSBSTATUS_PLAYING));
        CHECK(ring->Release() == 0);
    }
    if (primary) CHECK(primary->Release() == 0);
    CHECK(ds->Release() == 0);

    // The other exports.
    int enumerated = 0;
    auto enumerate = reinterpret_cast<HRESULT(WINAPI*)(LPDSENUMCALLBACKA, LPVOID)>(by_name("DirectSoundEnumerateA"));
    CHECK(enumerate && enumerate([](LPGUID guid, LPCSTR, LPCSTR, LPVOID ctx) -> BOOL { ++*static_cast<int*>(ctx); return guid == nullptr; }, &enumerated) == DS_OK && enumerated == 1);
    auto create8 = reinterpret_cast<HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN)>(by_name("DirectSoundCreate8"));
    LPDIRECTSOUND8 ds8 = reinterpret_cast<LPDIRECTSOUND8>(1);
    CHECK(create8 && create8(nullptr, &ds8, nullptr) == DSERR_NODRIVER && ds8 == nullptr);
    auto get_id = reinterpret_cast<HRESULT(WINAPI*)(LPCGUID, LPGUID)>(by_name("GetDeviceID"));
    GUID id = {};
    CHECK(get_id && get_id(nullptr, &id) == DS_OK && IsEqualGUID(id, DSDEVID_DefaultPlayback));
    auto can_unload = reinterpret_cast<HRESULT(WINAPI*)()>(by_name("DllCanUnloadNow"));
    CHECK(can_unload && can_unload() == S_OK);   // every object was released

    // Diagnostics and an explicit stop (the dll may not be unloadable: the native audio backend can pin it).
    auto report_fn = reinterpret_cast<size_t(WINAPI*)(char*, size_t)>(by_name("NueDsoundReport"));
    auto shutdown_fn = reinterpret_cast<void(WINAPI*)()>(by_name("NueDsoundShutdown"));
    CHECK(report_fn && shutdown_fn);
    char buffer[4096] = {};
    const std::string text = report_fn ? (report_fn(buffer, sizeof(buffer)), std::string(buffer)) : std::string();
    if (shutdown_fn) shutdown_fn();
    CHECK(FreeLibrary(dll));
    CHECK(text.find("DirectSoundCreate calls: 1") != std::string::npos);
    CHECK(text.find("IDirectSoundBuffer::Lock: 1 calls") != std::string::npos);
    CHECK(text.find("failed") == std::string::npos);   // not one DirectSound call failed
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: %s <path to dsound.dll>\n", argv[0]);
        return 2;
    }
    wchar_t wide[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wide, MAX_PATH);
    run(wide);
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
