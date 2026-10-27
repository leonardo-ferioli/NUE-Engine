// The DirectSound facade for a recompiled 32-bit guest: the only part of the layer that knows the guest ABI. It turns the guest's COM calls (32-bit
// pointers, stdcall words, WAVEFORMATEX / DSBUFFERDESC / DS3DBUFFER structures in guest memory, GUIDs, event handles) into Device calls and back.
// It owns the COM objects the guest sees (a vtable pointer and a cookie in guest memory), their reference counts, the QueryInterface graph
// (IDirectSound, IDirectSoundBuffer, IDirectSound3DBuffer, IDirectSound3DListener, IDirectSoundNotify) and the Lock staging memory (one guest-visible
// mirror per buffer: Lock hands out guest addresses inside it, Unlock reads the written bytes back through guest::Memory and gives them to the Device).
//
// Every guest address goes through guest::Memory::host(); nothing here assumes a guest address is a host address, so it works with a 4 GiB arena at any
// base (x86-64, arm64, any OS) as well as on a 32-bit identity host. It does not know how method calls reach it: the host glue (legacy_core's dispatcher,
// or the port's launcher) gives each (interface, slot) a call token, reads the stdcall words from the guest stack and calls call().
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nue_legacy/dsound_device.hpp"
#include "nue_legacy/guest_memory.hpp"

namespace nue::dsound::guest {

using Memory = nue::legacy::guest::Memory;

// Operating-system services the guest's handles refer to.
class HostServices {
public:
    virtual ~HostServices() = default;
    virtual void set_event(uint32_t guest_handle) = 0;          // SetEvent on a guest HANDLE (position notifications; runs on the mix thread)
    virtual bool is_window(uint32_t guest_hwnd) = 0;            // IsWindow (SetCooperativeLevel validates its window)
};

enum class DsInterface : uint8_t { direct_sound, buffer, buffer_3d, listener_3d, notify, count };

struct MethodDesc {
    DsInterface iface;
    uint8_t slot;
    uint8_t words;          // stdcall argument words including `this`
    const char* name;
};

class DsoundGuestFacade {
public:
    DsoundGuestFacade(Memory& memory, HostServices& host, Device& device);
    ~DsoundGuestFacade();
    DsoundGuestFacade(const DsoundGuestFacade&) = delete;
    DsoundGuestFacade& operator=(const DsoundGuestFacade&) = delete;

    // Every vtable slot of every interface, in a stable order (index = method id).
    static const std::vector<MethodDesc>& methods();
    // The call token the glue registered for method id `index` (must be set for all methods before the first DirectSoundCreate).
    void set_token(size_t index, uint32_t token);

    // DirectSoundCreate(lpcGuidDevice, ppDS, pUnkOuter): returns an HRESULT (the stdcall result in EAX).
    uint32_t direct_sound_create(uint32_t guid_address, uint32_t out_pointer, uint32_t outer);
    // One method call: `args[0]` is `this`. Returns EAX. A stale or foreign `this` fails with E_INVALIDARG, never a host fault.
    uint32_t call(size_t method_index, const uint32_t* args);

    // Diagnostics for the log: calls per method, failures per method, and the first failure's arguments.
    std::string report() const;
    uint32_t live_objects() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nue::dsound::guest
