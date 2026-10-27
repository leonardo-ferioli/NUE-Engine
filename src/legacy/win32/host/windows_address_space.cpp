#include "nue_legacy/win32/host/windows_address_space.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace nue::legacy::win32::host {
namespace {
class IdentityAddressSpace final : public AddressSpace {
public:
    uint32_t virtual_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect, uint32_t& result) override {
        SetLastError(0);
        void* p = VirtualAlloc(reinterpret_cast<void*>(uintptr_t(address)), size, type, protect);
        result = uint32_t(reinterpret_cast<uintptr_t>(p));
        return p ? 0 : GetLastError();
    }
    uint32_t virtual_free(uint32_t address, uint32_t size, uint32_t type) override {
        SetLastError(0);
        return VirtualFree(reinterpret_cast<void*>(uintptr_t(address)), size, type) ? 0 : GetLastError();
    }
    uint32_t virtual_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t& old_protect) override {
        DWORD old = 0;
        SetLastError(0);
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(uintptr_t(address)), size, protect, &old);
        old_protect = old;
        return ok ? 0 : GetLastError();
    }
    uint32_t virtual_query(uint32_t address, RegionInfo& out) override {
        MEMORY_BASIC_INFORMATION m;
        SetLastError(0);
        if (!VirtualQuery(reinterpret_cast<void*>(uintptr_t(address)), &m, sizeof m)) return GetLastError();
        out.base_address = uint32_t(reinterpret_cast<uintptr_t>(m.BaseAddress));
        out.allocation_base = uint32_t(reinterpret_cast<uintptr_t>(m.AllocationBase));
        out.allocation_protect = m.AllocationProtect;
        out.region_size = uint32_t(m.RegionSize);
        out.state = m.State; out.protect = m.Protect; out.type = m.Type;
        return 0;
    }
    std::byte* host(uint32_t address, uint32_t) override { return reinterpret_cast<std::byte*>(uintptr_t(address)); }
};
}  // namespace

std::unique_ptr<AddressSpace> make_identity_address_space() { return std::make_unique<IdentityAddressSpace>(); }
}  // namespace nue::legacy::win32::host
