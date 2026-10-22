// systrace / Perfetto markers (android.os.Trace from native code).  Cheap when tracing is off.
#include <android/trace.h>

#include "nue_android.h"

extern "C" {

void nue_android_trace_begin(const char* name) {
    if (ATrace_isEnabled()) ATrace_beginSection(name ? name : "nue");
}

void nue_android_trace_end(void) {
    if (ATrace_isEnabled()) ATrace_endSection();
}

void nue_android_trace_counter(const char* name, int64_t value) {
    if (ATrace_isEnabled()) ATrace_setCounter(name ? name : "nue", value); // API 29, minSdk is 29
}

} // extern "C"
