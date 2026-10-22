// ADPF (Android Dynamic Performance Framework) hint session through the NDK PerformanceHint API (API 33+).
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
void* g_session = nullptr;
AdpfTargetTracker g_target;

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

int adpf_available() {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    return g_api.manager ? 1 : 0;
}

void adpf_shutdown() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_session && g_api.close) g_api.close(g_session);
    g_session = nullptr;
}

} // namespace nue::android::native

extern "C" {

int nue_android_hint_session_start(const int32_t* tids, size_t count, int64_t target_ns) {
    std::lock_guard<std::mutex> lock(g_mu);
    load_locked();
    if (!g_api.manager || !tids || count == 0 || target_ns <= 0) return 0;
    if (g_session) {
        g_api.close(g_session);
        g_session = nullptr;
    }
    g_session = g_api.create(g_api.manager, tids, count, target_ns);
    g_target = AdpfTargetTracker();
    g_target.needs_update(target_ns); // remember the initial target
    if (!g_session) {
        NLOGW("ADPF: createSession failed (no permission for these threads or the device does not support hints)");
        return 0;
    }
    return 1;
}

void nue_android_hint_update_target(int64_t target_ns) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_session || !g_target.needs_update(target_ns)) return;
    g_api.update(g_session, target_ns);
}

void nue_android_hint_report(int64_t actual_ns) {
    if (actual_ns <= 0) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_session) g_api.report(g_session, actual_ns);
}

void nue_android_hint_session_stop(void) { nue::android::native::adpf_shutdown(); }

} // extern "C"
