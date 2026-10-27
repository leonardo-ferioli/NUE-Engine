// dsound.dll: the drop-in DirectSound for Windows games, on the NUE audio stack. DirectSoundCreate hands out the COM objects of com_facade.cpp over a
// nue::dsound::Engine whose output is a nue_audio_stream. Copy it next to the game's executable (it must be built for the game's bitness).
//
// Settings (environment variables, read when the first DirectSound object is created):
//   NUE_DSOUND_LATENCY_MS   output latency target in milliseconds (default 40)
//   NUE_DSOUND_SPATIAL      auto | balance | pan | binaural | ambisonic (default auto: DirectSound's own balance law on stereo, pairwise panning above)
//   NUE_DSOUND_HEADLESS     1: no audio device (NUE null backend); used by the tests
//   NUE_DSOUND_TRACE        path of a Level A trace (include/nue_legacy/dsound_trace.hpp) written when the dll is unloaded or the process ends
//   NUE_DSOUND_LOG          path of a text file that receives the call counts and the first failing call when the dll is unloaded
#include <windows.h>
#include <mmsystem.h>   // WAVEFORMATEX needs these two when WIN32_LEAN_AND_MEAN is set
#include <mmreg.h>
#include <dsound.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

    void save_trace() const {
        if (!trace_path_.empty()) trace_.save(trace_path_);
    }
    void shutdown() {
        save_trace();
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
        trace_path_ = env("NUE_DSOUND_TRACE");
        if (!trace_path_.empty()) config.trace = &trace_;
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
    TraceRecorder trace_;
    std::string trace_path_;
    bool nue_initialized_ = false, failed_ = false;
};

// Never destroyed by the C++ runtime at process exit: the audio thread of the native stream is gone by then and joining it would hang or abort.
Runtime& g_runtime = *new Runtime;
com::Facade& g_facade = *new com::Facade(g_runtime);

void write_log() {
    g_runtime.save_trace();
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

// Diagnostics for a launcher or a test (not part of DirectSound): the call counts and the first failing call as text, and an explicit stop of the audio
// engine. The dll cannot count on being unloaded (the native audio backend may pin it), so nothing here waits for DllMain.
size_t WINAPI NueDsoundReport(char* buffer, size_t capacity) {
    const std::string report = g_facade.report();
    if (buffer && capacity) {
        const size_t n = report.size() < capacity - 1 ? report.size() : capacity - 1;
        std::memcpy(buffer, report.data(), n);
        buffer[n] = 0;
    }
    return report.size();
}
void WINAPI NueDsoundShutdown() { g_runtime.shutdown(); }

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
