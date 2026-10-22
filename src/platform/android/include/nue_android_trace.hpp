// C++ convenience for the trace markers of nue_android.h:  NUE_TRACE_SCOPE("frame");
#pragma once
#include "nue_android.h"

namespace nue::android {
struct TraceScope {
    explicit TraceScope(const char* name) { nue_android_trace_begin(name); }
    ~TraceScope() { nue_android_trace_end(); }
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
};
} // namespace nue::android

#define NUE_TRACE_CONCAT2(a, b) a##b
#define NUE_TRACE_CONCAT(a, b) NUE_TRACE_CONCAT2(a, b)
#define NUE_TRACE_SCOPE(name) ::nue::android::TraceScope NUE_TRACE_CONCAT(nue_trace_scope_, __LINE__)(name)
