// Registry: RegOpenKey(Ex), RegCreateKey, RegCloseKey, RegQueryValueEx, RegSetValueEx (plus enumeration and deletion for completeness).
//
// `Registry` is the Level A interface: keys are handles (the predefined hives have the Win32 values, 0x80000000..0x80000003, sign-extended like on a
// 32-bit guest), values are typed byte strings, every call returns the Win32 error code (ERROR_SUCCESS = 0). Two implementations:
//   * ConfigRegistry: a registry tree kept in a platform::Config store under the "registry." prefix (how NUE persists a recompiled game's registry);
//   * the Windows registry (legacy_win32/host, Windows only) for a host that wants the real one.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/platform/config.hpp"
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nue::legacy::win32 {

namespace hkey { enum : Handle { classes_root = 0x80000000u, current_user = 0x80000001u, local_machine = 0x80000002u, users = 0x80000003u }; }
namespace reg { enum : uint32_t { none = 0, sz = 1, expand_sz = 2, binary = 3, dword = 4, dword_big_endian = 5, link = 6, multi_sz = 7, qword = 11 }; }
namespace reg_disposition { enum : uint32_t { created_new_key = 1, opened_existing_key = 2 }; }

class Registry {
public:
    virtual ~Registry() = default;
    // `sub_key` may be empty (the key itself). Each returns a Win32 error code; `out` receives the new key handle on success.
    virtual uint32_t open_key(Handle parent, std::string_view sub_key, Handle& out) = 0;
    virtual uint32_t create_key(Handle parent, std::string_view sub_key, Handle& out, uint32_t* disposition) = 0;
    virtual uint32_t close_key(Handle key) = 0;
    // `data` null asks for the size only. ERROR_MORE_DATA when `size` is too small (and `size` receives the needed size).
    virtual uint32_t query_value(Handle key, std::string_view name, uint32_t* type, uint8_t* data, uint32_t* size) = 0;
    virtual uint32_t set_value(Handle key, std::string_view name, uint32_t type, const uint8_t* data, uint32_t size) = 0;
    virtual uint32_t delete_value(Handle key, std::string_view name) = 0;
    virtual uint32_t delete_key(Handle parent, std::string_view sub_key) = 0;
    virtual uint32_t enum_key(Handle key, uint32_t index, std::string& name) = 0;                       // ERROR_NO_MORE_ITEMS at the end
    virtual uint32_t enum_value(Handle key, uint32_t index, std::string& name, uint32_t* type) = 0;
};

// A registry tree in a Config store. Every change is written through (`registry.` keys), so a Config that is saved and reloaded restores it.
std::unique_ptr<Registry> make_config_registry(platform::Config& config);

}  // namespace nue::legacy::win32
