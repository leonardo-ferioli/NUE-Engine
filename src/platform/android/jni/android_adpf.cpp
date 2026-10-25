// ADPF (Android Dynamic Performance Framework) hint sessions through the NDK PerformanceHint API (API 33+).
// The symbols are resolved with dlsym so the library keeps loading on Android 10-12 where they do not exist.
#include <dlfcn.h>

#include <mutex>

#include "android_state.hpp"
#include "thermal_policy.hpp"

using namespace nue::android;

namespace {

struct Api {
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
    using UpdateTarget = int (*)(void*, int64_t);
    using Report = int (*)(void*, int64_t);
    using Close = void (*)(void*);
    GetManager get_manager = nullptr;
    CreateSession create = nullptr;
    UpdateTarget update = nullptr;
    Report report = nullptr;
    Close close = nullptr;
    void* manager = nullptr;
    bool loaded = false;
};

std::mutex g_mu;
Api g_api;

void load_locked() {
    if (g_api.loaded) return;
    g_api.loaded = true;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    g_api.get_manager = reinterpret_cast<Api::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
    g_api.create = reinterpret_cast<Api::CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
    g_api.update = reinterpret_cast<Api::UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    g_api.report = reinterpret_cast<Api::Report>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    g_api.close = reinterpret_cast<Api::Close>(dlsym(lib, "APerformanceHint_closeSession"));
    if (g_api.get_manager && g_api.create && g_api.update && g_api.report && g_api.close) g_api.manager = g_api.get_manager();
}

} // namespace

namespace nue::android::native {

struct HintSession {
    void* handle = nullptr;
    AdpfTargetTracker target;
};

int adpf_available() {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    return g_api.manager ? 1 : 0;
}

HintSession* adpf_create(const int32_t* tids, size_t count, int64_t target_ns) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    if (!g_api.manager || !tids || count == 0 || target_ns <= 0) return nullptr;
    void* h = g_api.create(g_api.manager, tids, count, target_ns);
    if (!h) {
        NLOGW("ADPF: createSession failed (threads of another process, or the device does not support hints)");
        return nullptr;
    }
    HintSession* s = new HintSession();
    s->handle = h;
    s->target.needs_update(target_ns);
    return s;
}

void adpf_update(HintSession* s, int64_t target_ns) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (s->target.needs_update(target_ns)) g_api.update(s->handle, target_ns);
}

void adpf_report(HintSession* s, int64_t actual_ns) {
    if (!s || actual_ns <= 0) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_api.report(s->handle, actual_ns);
}

void adpf_destroy(HintSession* s) {
    if (!s) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (s->handle) g_api.close(s->handle);
    delete s;
}

namespace {
HintSession* g_default = nullptr; // the session of the single-session C API
}

void adpf_shutdown() {
    HintSession* s;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = g_default;
        g_default = nullptr;
    }
    adpf_destroy(s);
}

} // namespace nue::android::native

extern "C" {

int nue_android_hint_session_start(const int32_t* tids, size_t count, int64_t target_ns) {
    using namespace nue::android::native;
    adpf_shutdown();
    HintSession* s = adpf_create(tids, count, target_ns);
    std::lock_guard<std::mutex> lock(g_mu);
    g_default = s;
    return s ? 1 : 0;
}

void nue_android_hint_update_target(int64_t target_ns) {
    using namespace nue::android::native;
    HintSession* s;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = g_default;
    }
    adpf_update(s, target_ns);
}

void nue_android_hint_report(int64_t actual_ns) {
    using namespace nue::android::native;
    HintSession* s;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        s = g_default;
    }
    adpf_report(s, actual_ns);
}

void nue_android_hint_session_stop(void) { nue::android::native::adpf_shutdown(); }

} // extern "C"
