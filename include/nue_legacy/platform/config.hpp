// Platform API: configuration store. Shaped like NUE's nue_config_* (include/nue/core.h): typed values under dotted keys, loaded from and
// saved to text. The Win32 registry layer keeps its values here under the "registry." prefix, which is how NUE persists a recompiled
// game's registry (docs/ABI.md 2.7): the registry tree, value types and handle rules are Win32 semantics, the persistence is this store.
#pragma once
#include "nue_legacy/platform/types.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::platform {

class Config {
public:
    virtual ~Config() = default;
    virtual bool has(std::string_view key) = 0;
    virtual Status get_string(std::string_view key, std::string& out) = 0;     // not_found when absent
    virtual Status set_string(std::string_view key, std::string_view value) = 0;
    virtual Status remove(std::string_view key) = 0;
    // Keys that start with `prefix`, sorted, so a registry key can list its values and subkeys.
    virtual std::vector<std::string> keys_with_prefix(std::string_view prefix) = 0;
    virtual Status load_text(std::string_view text) = 0;                       // "key=value" lines, '#' comments
    virtual std::string save_text() = 0;
    virtual Status load(std::string_view path) = 0;                            // storage path ("settings:nue.ini")
    virtual Status save(std::string_view path) = 0;
};

}  // namespace nue::legacy::platform
