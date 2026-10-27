// Implementations of the legacy platform interfaces that call the native API (include/nue/*.h): thin adapters, no state of their own beyond handle tables.
// The engine must be initialised (nue_init) before any of them is used. The std implementations (std_platform.hpp) remain the default of the standalone library
// and the null ones its test fixtures; with these the legacy layers run on NUE's own clock, threads, timers (virtual in deterministic mode) and system report.
#pragma once
#include "nue_legacy/platform/clock.hpp"
#include "nue_legacy/platform/system.hpp"
#include "nue_legacy/platform/threads.hpp"
#include <memory>

namespace nue::legacy::platform::nue_backend {

std::unique_ptr<Clock> make_clock();        // nue_time_*, nue_sleep_ns; the local time zone offset is answered by the std implementation (the native API has none)
std::unique_ptr<Threads> make_threads();    // nue_thread_*, nue_mutex_*, nue_cond_*, nue_semaphore_*, nue_tls_*, nue_timer_*
std::unique_ptr<System> make_system();      // nue_system_get_info over the std report

}  // namespace nue::legacy::platform::nue_backend
