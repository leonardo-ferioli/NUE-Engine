// systrace / Perfetto markers (android.os.Trace from native code).  Cheap when tracing is off.
#include <android/trace.h>
#include <dlfcn.h>

#include "nue_android.h"

namespace {

// ATrace_setCounter exists from API 29; the library may be built for a lower minSdk (the engine preset uses 26), so it is
// resolved at run time instead of being called through the header.
using SetCounterFn = void (*)(const char*, int64_t);

SetCounterFn set_counter_fn() {
    static SetCounterFn fn = [] {
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        return lib ? reinterpret_cast<SetCounterFn>(dlsym(lib, "ATrace_setCounter")) : nullptr;
    }();
    return fn;
}

} // namespace

extern "C" {

void nue_android_trace_begin(const char* name) {
    if (ATrace_isEnabled()) ATrace_beginSection(name ? name : "nue");
}

void nue_android_trace_end(void) {
    if (ATrace_isEnabled()) ATrace_endSection();
}

void nue_android_trace_counter(const char* name, int64_t value) {
    if (!ATrace_isEnabled()) return;
    if (SetCounterFn fn = set_counter_fn()) fn(name ? name : "nue", value);
}

} // extern "C"
