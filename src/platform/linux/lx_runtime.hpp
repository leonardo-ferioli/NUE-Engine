// Run-time capability survey of the Linux services (what the engine can actually use here),
// used by linux_probe and by the engine's startup log.
#pragma once
#include <string>
#include <vector>

namespace nue::lx {

struct OptionalLibrary {
    const char* name;      // human name
    const char* soname;    // what dlopen tries
    const char* used_for;
    bool present = false;
};

struct RuntimeCaps {
    std::vector<OptionalLibrary> libraries;
    bool dbus_session = false, dbus_system = false;
    bool gamemode_lib = false, gamemode_daemon = false;
    bool power_profiles = false, rtkit = false, screensaver_service = false;
    bool io_uring = false;
    int max_rt_priority = 0, min_nice = 0;
    bool timerfd = false;
    long monotonic_resolution_ns = 0;
    unsigned long long core_limit = 0;
    std::vector<std::string> notes;
};

RuntimeCaps query_runtime_caps();
void print_runtime_capabilities();  // formatted dump to stdout

}  // namespace nue::lx
