// Version, result names and per-thread last-error text.
#include "nue/core.h"

#include <cstdarg>
#include <cstdio>

#ifndef NUE_GIT_COMMIT
#define NUE_GIT_COMMIT "unknown"
#endif
#ifndef NUE_VERSION_STRING_LITERAL
#define NUE_VERSION_STRING_LITERAL "0.1.1"
#endif

namespace {

thread_local char g_last_error[512];

const char* result_name(nue_result r) {
    switch (r) {
#define NUE_RESULT_CASE(x) case x: return #x;
        NUE_RESULT_CASE(NUE_OK)
        NUE_RESULT_CASE(NUE_OK_PENDING)
        NUE_RESULT_CASE(NUE_OK_PARTIAL)
        NUE_RESULT_CASE(NUE_OK_NOOP)
        NUE_RESULT_CASE(NUE_ERR_FAILED)
        NUE_RESULT_CASE(NUE_ERR_INVALID_ARGUMENT)
        NUE_RESULT_CASE(NUE_ERR_INVALID_HANDLE)
        NUE_RESULT_CASE(NUE_ERR_NOT_INITIALIZED)
        NUE_RESULT_CASE(NUE_ERR_ALREADY_INITIALIZED)
        NUE_RESULT_CASE(NUE_ERR_UNSUPPORTED)
        NUE_RESULT_CASE(NUE_ERR_OUT_OF_MEMORY)
        NUE_RESULT_CASE(NUE_ERR_NOT_FOUND)
        NUE_RESULT_CASE(NUE_ERR_ALREADY_EXISTS)
        NUE_RESULT_CASE(NUE_ERR_ACCESS_DENIED)
        NUE_RESULT_CASE(NUE_ERR_IO)
        NUE_RESULT_CASE(NUE_ERR_TIMEOUT)
        NUE_RESULT_CASE(NUE_ERR_BUFFER_TOO_SMALL)
        NUE_RESULT_CASE(NUE_ERR_WRONG_THREAD)
        NUE_RESULT_CASE(NUE_ERR_BAD_STATE)
        NUE_RESULT_CASE(NUE_ERR_VERSION_MISMATCH)
        NUE_RESULT_CASE(NUE_ERR_DEVICE_LOST)
        NUE_RESULT_CASE(NUE_ERR_CANCELLED)
        NUE_RESULT_CASE(NUE_ERR_END_OF_DATA)
        NUE_RESULT_CASE(NUE_ERR_BUSY)
        NUE_RESULT_CASE(NUE_ERR_WOULD_BLOCK)
        NUE_RESULT_CASE(NUE_ERR_CORRUPT)
        NUE_RESULT_CASE(NUE_ERR_BACKEND)
        NUE_RESULT_CASE(NUE_ERR_OVERFLOW)
#undef NUE_RESULT_CASE
    }
    return "NUE_RESULT_UNKNOWN";
}

}  // namespace

extern "C" {

NUE_API uint32_t NUE_CALL nue_get_version(void) { return NUE_API_VERSION; }

NUE_API const char* NUE_CALL nue_get_version_string(void) { return NUE_VERSION_STRING_LITERAL " (" NUE_GIT_COMMIT ")"; }

NUE_API const char* NUE_CALL nue_result_name(nue_result r) { return result_name(r); }

NUE_API const char* NUE_CALL nue_get_last_error_message(void) { return g_last_error; }

NUE_API void NUE_CALL nue_clear_last_error(void) { g_last_error[0] = 0; }

NUE_API void NUE_CALL nue_set_last_error(const char* fmt, ...) {
    if (!fmt) { g_last_error[0] = 0; return; }
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(g_last_error, sizeof(g_last_error), fmt, args);
    va_end(args);
}

}  // extern "C"
