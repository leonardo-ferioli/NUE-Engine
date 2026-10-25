// Stand-ins for the few core functions the platform layer calls, so platform_linux.cpp can be unit-tested
// on its own (host build, no core library). In the integrated build the real core provides them.
#include <chrono>
#include <cstdarg>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "internal/platform_ops.h"
#include "ops_stubs.hpp"

namespace stubs {
std::mutex mtx;
std::vector<nue_event> events;
std::map<std::string, const void*> extensions;
std::vector<std::string> logs;
std::vector<int> app_states;
std::vector<int> glyph_overrides;
}  // namespace stubs

extern "C" {

void nue_internal_push_event(const nue_event* event) {
    std::lock_guard<std::mutex> l(stubs::mtx);
    stubs::events.push_back(*event);
}
void nue_internal_register_extension(const char* name, const void* table) {
    std::lock_guard<std::mutex> l(stubs::mtx);
    stubs::extensions[name] = table;
}
int64_t nue_time_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void nue_log_write(nue_log_level, const char* tag, const char*, int, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(stubs::mtx);
    stubs::logs.push_back(std::string(tag) + ": " + buf);
}
void nue_set_last_error(const char*, ...) {}
nue_result nue_input_set_glyph_override(int32_t family) {
    std::lock_guard<std::mutex> l(stubs::mtx);
    stubs::glyph_overrides.push_back(family);
    return NUE_OK;
}
void nue_internal_set_app_state(nue_app_state state) {
    std::lock_guard<std::mutex> l(stubs::mtx);
    stubs::app_states.push_back(static_cast<int>(state));
}

}  // extern "C"
