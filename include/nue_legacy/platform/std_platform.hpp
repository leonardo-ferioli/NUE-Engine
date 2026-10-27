// Portable implementations of the platform interfaces over the C++ standard library only (std::chrono, std::thread, std::filesystem, stdio).
// They are the default on every host; the few OS-specific refinements (thread priority, memory totals, time zone) are guarded and optional.
// NUE replaces them with its own modules; nothing above these classes knows which is in use.
#pragma once
#include "nue_legacy/platform/clock.hpp"
#include "nue_legacy/platform/config.hpp"
#include "nue_legacy/platform/storage.hpp"
#include "nue_legacy/platform/system.hpp"
#include "nue_legacy/platform/threads.hpp"
#include <memory>

namespace nue::legacy::platform::stdlib {

std::unique_ptr<Clock> make_clock();
std::unique_ptr<Threads> make_threads();
std::unique_ptr<System> make_system();
std::unique_ptr<Storage> make_storage();           // a file system with named roots and case-insensitive lookup
std::unique_ptr<Config> make_config(Storage* storage_for_load_and_save);   // in memory; `load`/`save` go through the storage

}  // namespace nue::legacy::platform::stdlib
