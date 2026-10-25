// The COM facade of legacy_dsound for native Windows hosts: IDirectSound, IDirectSoundBuffer, IDirectSound3DListener, IDirectSound3DBuffer and
// IDirectSoundNotify as real COM objects (the interfaces of the Windows SDK's dsound.h) over a Level A nue::dsound::Device. This is the only
// code that knows COM; the dll (main.cpp) and the tests both use it. The guest-pointer facade of a recompiled 32-bit game is a different
// front end over the same Device (docs/LEGACY_DSOUND.md, "Guest facade seam").
#pragma once
#include <memory>
#include <string>

#include <windows.h>
#include <mmsystem.h>   // WAVEFORMATEX needs these two when WIN32_LEAN_AND_MEAN is set
#include <mmreg.h>
#include <dsound.h>

#include "nue_legacy/dsound_device.hpp"

namespace nue::dsound::com {

// Where the Device a DirectSound object talks to comes from. The dll starts the audio engine on the first acquire; a test hands its own.
class DeviceProvider {
public:
    virtual ~DeviceProvider() = default;
    // Called when the first DirectSound object is created (and again after every time the last one went away). nullptr = DSERR_NODRIVER.
    virtual Device* acquire() = 0;
    // The last DirectSound object is gone (its buffers too).
    virtual void release() {}
};

class Facade {
public:
    explicit Facade(DeviceProvider& provider);
    ~Facade();
    Facade(const Facade&) = delete;
    Facade& operator=(const Facade&) = delete;

    // DirectSoundCreate. `guid` is the device (null, DSDEVID_DefaultPlayback and DSDEVID_DefaultVoicePlayback name the only one).
    HRESULT create(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer);

    uint32_t live_objects() const;   // COM objects not yet released (device and buffers)
    // Call counts of every method that was used, and the first failing call: what a log wants to print at exit.
    std::string report() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace nue::dsound::com
