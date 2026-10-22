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
}  // namespace stubs
