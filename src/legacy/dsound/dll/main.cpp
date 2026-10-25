// dsound.dll: the drop-in DirectSound for Windows games, on the NUE audio stack. DirectSoundCreate hands out the COM objects of com_facade.cpp over a
// nue::dsound::Engine whose output is a nue_audio_stream. Copy it next to the game's executable (it must be built for the game's bitness).
//
// Settings (environment variables, read when the first DirectSound object is created):
//   NUE_DSOUND_LATENCY_MS   output latency target in milliseconds (default 40)
//   NUE_DSOUND_SPATIAL      auto | balance | pan | binaural | ambisonic (default auto: DirectSound's own balance law on stereo, pairwise panning above)
//   NUE_DSOUND_HEADLESS     1: no audio device (NUE null backend); used by the tests
//   NUE_DSOUND_LOG          path of a text file that receives the call counts and the first failing call when the dll is unloaded
#include <windows.h>
#include <mmsystem.h>   // WAVEFORMATEX needs these two when WIN32_LEAN_AND_MEAN is set
#include <mmreg.h>
#include <dsound.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "com_facade.hpp"
#include "nue/core.h"
#include "nue_legacy/dsound_engine.hpp"

namespace {

using namespace nue::dsound;

std::string env(const char* name) {
    char buffer[260];
    const DWORD n = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
    return n && n < sizeof(buffer) ? std::string(buffer, n) : std::string();
}

class Runtime final : public com::DeviceProvider {
public:
    Device* acquire() override {
        if (!engine_ && !failed_) start();
        return engine_ ? &engine_->device() : nullptr;
    }
    // The engine stays up for the life of the process: DirectSound games create and release the object repeatedly (capability probes) and a fresh
    // output device each time would cost more than a stream playing silence.
    void release() override {}

    void shutdown() {
        engine_.reset();
        if (nue_initialized_) nue_shutdown();
        nue_initialized_ = false;
    }

private:
    void start() {
        const bool headless = env("NUE_DSOUND_HEADLESS") == "1";
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.app_name = "legacy_dsound";
        d.app_id = "legacy-dsound";
        d.org_name = "NUE";
        d.backend = headless ? NUE_BACKEND_NULL : NUE_BACKEND_AUTO;
        d.loop_model = NUE_LOOP_HOST;
        d.subsystems = NUE_SUBSYSTEM_EVENTS | NUE_SUBSYSTEM_AUDIO;
        d.flags = NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD | (headless ? NUE_INIT_HEADLESS : 0u);
        d.log_level = NUE_LOG_WARN;
        if (NUE_FAILED(nue_init(&d))) {
            failed_ = true;
            return;
        }
        nue_initialized_ = true;
        EngineConfig config;
        if (const std::string ms = env("NUE_DSOUND_LATENCY_MS"); !ms.empty()) config.latency_ms = uint32_t(std::atoi(ms.c_str()));
        const std::string spatial = env("NUE_DSOUND_SPATIAL");
        if (spatial == "balance") config.spatial.mode = SpatialMode::balance;
        else if (spatial == "pan") config.spatial.mode = SpatialMode::pan;
        else if (spatial == "binaural") config.spatial.mode = SpatialMode::binaural;
        else if (spatial == "ambisonic") config.spatial.mode = SpatialMode::ambisonic;
        engine_ = Engine::create(config, make_nue_sink());
        if (!engine_) {
            failed_ = true;
            nue_shutdown();
            nue_initialized_ = false;
            return;
        }
        engine_->start();
    }

    std::unique_ptr<Engine> engine_;
    bool nue_initialized_ = false, failed_ = false;
};

Runtime g_runtime;
com::Facade g_facade(g_runtime);

void write_log() {
    const std::string path = env("NUE_DSOUND_LOG");
    if (path.empty()) return;
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        const std::string report = g_facade.report();
        std::fwrite(report.data(), 1, report.size(), f);
        std::fputc('\n', f);
        std::fclose(f);
    }
}

}  // namespace

extern "C" {

HRESULT WINAPI DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer) { return g_facade.create(guid, out, outer); }

// DirectSound8 and capture are not offered: the layer implements IDirectSound only.
HRESULT WINAPI DirectSoundCreate8(LPCGUID, LPDIRECTSOUND8* out, LPUNKNOWN) {
    if (out) *out = nullptr;
    return DSERR_NODRIVER;
}
HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID, LPDIRECTSOUNDCAPTURE* out, LPUNKNOWN) {
    if (out) *out = nullptr;
    return DSERR_NODRIVER;
}
HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID, LPDIRECTSOUNDCAPTURE8* out, LPUNKNOWN) {
    if (out) *out = nullptr;
    return DSERR_NODRIVER;
}
HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID, LPCGUID, LPCDSCBUFFERDESC, LPCDSBUFFERDESC, HWND, DWORD, LPDIRECTSOUNDFULLDUPLEX* out, LPDIRECTSOUNDCAPTUREBUFFER8* capture,
                                          LPDIRECTSOUNDBUFFER8* render, LPUNKNOWN) {
    if (out) *out = nullptr;
    if (capture) *capture = nullptr;
    if (render) *render = nullptr;
    return DSERR_NODRIVER;
}

// The one playback device: "Primary Sound Driver" (a null GUID), as the system DirectSound lists it first.
HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA callback, LPVOID context) {
    if (!callback) return DSERR_INVALIDPARAM;
    callback(nullptr, "Primary Sound Driver", "", context);
    return DS_OK;
}
HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW callback, LPVOID context) {
    if (!callback) return DSERR_INVALIDPARAM;
    callback(nullptr, L"Primary Sound Driver", L"", context);
    return DS_OK;
}
HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA, LPVOID) { return DS_OK; }   // no capture devices
HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW, LPVOID) { return DS_OK; }

HRESULT WINAPI GetDeviceID(LPCGUID guid_src, LPGUID guid_dst) {
    if (!guid_dst) return DSERR_INVALIDPARAM;
    if (!guid_src || IsEqualGUID(*guid_src, DSDEVID_DefaultPlayback)) {
        *guid_dst = DSDEVID_DefaultPlayback;
    } else if (IsEqualGUID(*guid_src, DSDEVID_DefaultVoicePlayback)) {
        *guid_dst = DSDEVID_DefaultVoicePlayback;
    } else {
        *guid_dst = *guid_src;
    }
    return DS_OK;
}

HRESULT WINAPI DllCanUnloadNow() { return g_facade.live_objects() == 0 ? S_OK : S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* out) {   // CoCreateInstance(CLSID_DirectSound) is not supported: games use DirectSoundCreate
    if (out) *out = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH) {
        write_log();
        if (!reserved) g_runtime.shutdown();   // FreeLibrary; at process exit the audio thread is gone already and nothing is worth waiting for
    }
    return TRUE;
}

}  // extern "C"
