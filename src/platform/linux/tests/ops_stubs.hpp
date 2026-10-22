#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "internal/platform_ops.h"

namespace stubs {
extern std::mutex mtx;
extern std::vector<nue_event> events;
extern std::map<std::string, const void*> extensions;
extern std::vector<std::string> logs;
extern std::vector<int> app_states;  // nue_app_state values passed to nue_internal_set_app_state
}  // namespace stubs
