# legacy_dsound: the DirectSound layer

`legacy_dsound` answers the calls of a Windows DirectSound client (`IDirectSound`, `IDirectSoundBuffer`, `IDirectSound3DListener`, `IDirectSound3DBuffer`,
`IDirectSoundNotify`) on top of the native audio stack of the engine (`nue::nue` streams, `nue::audio` resampler / channel mixer / spatial / reverb). It
follows the shape described in [LEGACY_LAYERS.md](LEGACY_LAYERS.md). It was extracted from the DirectSound work of the NFS Underground 2 port (semantics, tests, facts
about the one game that used it) and rewritten on the native audio module; the game-specific parts (guest memory, import overrides, launcher glue) stay in
that project. Open items and requests to other areas: [REQUESTS_dsound.md](REQUESTS_dsound.md). Licences: [THIRD_PARTY_DSOUND.md](THIRD_PARTY_DSOUND.md).

> **State, honestly.** Implemented, compiled for Windows x64 and x86 (clang), tested headless, and compared call for call with the real Windows DirectSound
> (296 comparisons, 0 differences). Tested on a real output endpoint with **silence** only. **The audio has not been heard in a real game**: the first audible
> run of a game through this layer is still to be done, and nothing here claims that it sounds right by ear. The guest-pointer facade of a recompiled 32-bit game
> is built and tested against a fake guest arena (any base, 32- and 64-bit hosts) but **not wired to a real guest**: that needs the dispatcher of `legacy_core`
> or the port's launcher. Not run on Linux/GCC, Android or iOS (see "What is implemented / tested / only written").

## Structure

```
 DirectSound client (a Windows game, or the guest facade of a recompiled game)
   |  IDirectSound*, IDirectSoundBuffer*, ...                                       dll/com_facade.*   COM objects over dsound.h (Windows only)
   |                                                                                 [guest facade: future, over legacy_core]
 Level A: nue::dsound::Device    include/nue_legacy/dsound_device.hpp  src/device.cpp
   |   DirectSound semantics as handle-based C++; HRESULTs as integers; no COM, no guest pointers, no game knowledge
   |   per voice: PCM block (float32) + how it is placed
 Placement                       src/placement.*        <-- nue_chanmix (beds), nue_spatial_* (3D), nue_reverb (environment), DS balance pan law
 Resampling                      src/device.cpp         <-- nue_resampler
   |   interleaved float32 in the output layout
 Engine + OutputSink             include/nue_legacy/dsound_engine.hpp  src/engine.cpp  src/nue_sink.cpp
   |   a nue_audio_stream with a callback; follows the primary format's channel count; reports latency
 nue_audio_stream (include/nue/audio.h)  -->  platform backend (WASAPI/SDL3, ...) or the null backend (tests)
```

| Part | Files | What it is for |
|---|---|---|
| Level A types and device | `include/nue_legacy/dsound_types.hpp`, `dsound_device.hpp`, `src/legacy/dsound/src/device.cpp` | buffers (primary, secondary), Lock/Unlock, play/write cursors, status, volume/pan/frequency, notifications, 3D listener and buffers, environment sends, the mixer |
| Placement | `src/legacy/dsound/src/placement.{hpp,cpp}` | how a voice reaches the output channels (2D pair, multichannel bed, 3D point source) and the environment reverb |
| Engine and sinks | `include/nue_legacy/dsound_engine.hpp`, `src/engine.cpp`, `src/nue_sink.cpp` | ties a Device to an output; the shipped sink is a native stream; reopens the stream when the primary format changes the channel count |
| COM facade | `src/legacy/dsound/dll/com_facade.{hpp,cpp}` | the five interfaces as real COM objects (static library `nue_dsound_comlib`), used by the dll and the tests |
| `dsound.dll` | `src/legacy/dsound/dll/main.cpp`, `dsound.def` | the drop-in: `DirectSoundCreate` and the other exports of the system dll, with its ordinals |
| Guest facade | `include/nue_legacy/dsound_guest.hpp`, `guest_memory.hpp`, `src/legacy/dsound/guest/guest_facade.cpp` | the five interfaces for a recompiled 32-bit guest, over `guest::Memory` (no host address assumption) |
| Level A trace | `include/nue_legacy/dsound_trace.hpp`, `src/trace.cpp`, `tools/dsound_trace.cpp` | record every call and the PCM written, replay on a fresh Device, compare clients by the PCM hash |
| Inventory tool | `src/legacy/dsound/tools/dsound_inventory.py` | finds out how any 32-bit Windows executable uses DirectSound (imports, interface IDs present, call sites, vtable slots); needs `capstone` |
| Tests | `tests/legacy/dsound/` | see "Tests" |

Everything is in namespace `nue::dsound`. The library is `nue_legacy_dsound` (alias `nue::legacy_dsound`), it is built with `-DNUE_WITH_LEGACY_DSOUND=ON` and needs
`nue::audio`. There is no ABI promise for the C++ headers yet (they are the layer's own API, versioned with the repository); nothing under `include/nue/` changed.

## Use

```
cmake --preset windows-x64 -DNUE_WITH_LEGACY_DSOUND=ON      # windows-x86 for the 32-bit dsound.dll of a 32-bit game
cmake --build --preset windows-x64 --target nue_dsound_dll   # build/<preset>/src/legacy/dsound/dll/dsound.dll
ctest --preset windows-x64 -R nue_dsound
```

Copy `dsound.dll` next to the game's executable (the bitness must match the game; `dsound.dll` is not a "known DLL", so the one beside the executable wins).
On first `DirectSoundCreate` it starts the engine (`nue_init` with the audio subsystem, then an output stream on the default device, which follows the
system default). The ordinals are the system's (1 `DirectSoundCreate`, 2/3 `DirectSoundEnumerateA/W`, 4 `DllCanUnloadNow`, 5 `DllGetClassObject`, 6/7/8 capture,
9 `GetDeviceID`, 10 `DirectSoundFullDuplexCreate`, 11 `DirectSoundCreate8`, 12 `DirectSoundCaptureCreate8`), so a game that imports by ordinal binds to it. Two extra
named exports serve a launcher or a test: `NueDsoundReport(char*, size_t)` (call counts per method, the first failing call) and `NueDsoundShutdown()`.

Settings, as environment variables read when the first DirectSound object is created:

| Variable | Meaning |
|---|---|
| `NUE_DSOUND_LATENCY_MS` | output latency target (default 40) |
| `NUE_DSOUND_SPATIAL` | `auto` (default: DirectSound's balance law on stereo, pairwise panning above), `balance`, `pan`, `binaural`, `ambisonic` |
| `NUE_DSOUND_HEADLESS=1` | no audio device (the NUE null backend); used by the tests |
| `NUE_DSOUND_LOG` | file that receives the report when the dll is unloaded or the process ends |

From C++ without COM: `auto engine = nue::dsound::Engine::create(config, nue::dsound::make_nue_sink()); engine->device()` is the Level A `Device`; tests and hosts with another
output implement `OutputSink` (open, start, stop, close, frames_played, latency, stats) and hand it in instead.

## What the client may rely on (the contract of the ring)

A streaming client (one primary buffer, one 0.5 s looping secondary ring polled every ~10 ms with `GetStatus`, `GetCurrentPosition(&play, NULL)`, `Lock`/`Unlock`,
`Play`, `Restore`) is the pattern the layer was built around:

1. **The play cursor moves while the buffer plays**, at the real-time rate of the output. A client that sees the same cursor 100 times in a row rebuilds its audio. Checked:
   200 polls of a looping ring never repeat a value (`ds_com_tests`), the same on the native stream (`ds_nue_tests`), and on the real endpoint the cursor ran 0.512 s of audio
   in 0.523 s of wall time with 0 underruns (`nue_dsound_endpoint_smoke`).
2. **Data written at `play + lead` is heard**: `Unlock` publishes immediately.
3. `GetStatus` and `GetCurrentPosition` are cheap (one mutex, no allocation).
4. **Format negotiation answers like Windows**, so a client's probe of 1, 2, 4 and 6 channels settles on the same count.
5. **The play cursor is reported behind the mixer by the sink's latency**, so the client's lead stays right whatever the output buffer is. The write cursor is never closer than
   30 ms (measured on Windows 11: 30.0 ms on every poll; the layer: 30.0 ms).

## Semantics that had to be got right

All of this is checked by a test; items marked **(Windows)** were found by the differential test against the system DirectSound (its first run on the port had 39
differences; a paper reading of the documentation gets all of them wrong).

| Semantic | Right behaviour |
|---|---|
| Buffer size not a multiple of the frame | **rounded up** to whole frames: 4001 bytes of 4-byte frames -> 4004; 7 -> 8 **(Windows)** |
| `CTRL3D` | accepted for a stereo buffer and together with `CTRLPAN` **(Windows)** |
| `LOCHARDWARE` | `E_NOTIMPL`, not `DSERR_ALLOCATED` **(Windows)** |
| Malformed format | `E_INVALIDARG` for 0 channels, 0 or non-multiple-of-8 bits, wrong `nBlockAlign`, rate outside 100..200000 (buffer and primary `SetFormat`); more than 2 channels need `WAVEFORMATEXTENSIBLE` (plain: `E_INVALIDARG` for a buffer, `DSERR_ALLOCATED` for primary `SetFormat`); 9+ channels extensible: `E_INVALIDARG`; unknown tag: `E_NOTIMPL` for a buffer, **`S_OK` and ignored** for primary `SetFormat` **(Windows)** |
| Order of checks in the controls | **range first, control flag second**: `SetVolume(1)` without `CTRLVOLUME` is `E_INVALIDARG`, `SetVolume(-100)` is `DSERR_CONTROLUNAVAIL`; same for pan and frequency; `SetFrequency(0)` = the buffer's own rate **(Windows)** |
| `Lock` with `DSBLOCK_ENTIREBUFFER` | the whole buffer **starting at the given offset**, wrapping (offset 123 of 4000 -> 3877 and 123 bytes) **(Windows)** |
| Several outstanding `Lock`s | allowed; `Unlock` matches the pointers to one of them, else `E_INVALIDARG` **(Windows)** |
| `Lock` that wraps without a second pointer | succeeds, offering only the first region **(Windows)** |
| `GetStatus` of a software buffer | no location bit **(Windows)** |
| `GetCurrentPosition(NULL, NULL)` | `E_INVALIDARG` **(Windows)** |
| Stopped buffer | both cursors equal the position; `Stop` keeps the position where it was **heard**, not where the mixer got to **(Windows)** |
| One-shot buffer at its end | stops by itself, status clears, play cursor back to 0 **(Windows)** |
| `SetNotificationPositions` | while playing `DSERR_INVALIDCALL`; without `CTRLPOSITIONNOTIFY` `DSERR_CONTROLUNAVAIL`; byte offsets; `DSBPN_OFFSETSTOP` signals at stop **(Windows)** |
| Primary buffer | `SetFormat` at `DSSCL_NORMAL` is `DSERR_PRIORITYLEVELNEEDED`; a second request returns the **same object** with one more reference; `Play` needs `DSBPLAY_LOOPING` **(Windows)** |
| Reference counting | one count per object shared by all its interfaces; the device outlives its buffers; buffers die with their last `Release` |
| Gain ramps start at the target | the first block of a sound is not faded in (a bug the exact-playback test found) |
| Doppler sign | `(c + vl)/(c + vs)`, velocities projected on listener -> source: an approaching source plays higher (a bug the math test found) |
| Output independent of the caller's chunking | the same samples for any request sizes (blocks of 256 behind a FIFO) |
| Determinism | the same call sequence gives the same output hash, **identical in the 32-bit and the 64-bit build** (`0xd6f87479857334e9` for the scripted session, also through the native null-backend stream) |

## Rate conversion, layouts, 3D and the environment (what is the layer's and what is `nue::audio`'s)

| Job | Done by | Notes |
|---|---|---|
| Buffer rate (and `SetFrequency`, Doppler) to the output rate | `nue_resampler` (medium: 68 taps, 80 dB), one per playing voice, created when the rate differs | bit-exact pass-through when the rate equals the output (no resampler); the rate may change while playing. The position after priming is within one output frame of the ideal; the cursor bookkeeping absorbs it |
| Multichannel buffer ("bed") to the output layout | `nue_chanmix` (BS.775 gains, centre and surrounds at -3 dB, LFE dropped) | 5.1 to stereo, 5.1 to 5.1 (each channel on its speaker), stereo into 5.1 stays on the front pair |
| 2D mono/stereo buffer | the layer: per-channel gains to the front pair | DirectSound's own pan law: `SetPan` attenuates only the far channel; a centred mono sound is at unity in both |
| 3D scene maths (distance, cone, Doppler) | `nue_spatial_evaluate` | DS3D maps field by field: min/max distance, cone angles (full angles) and outside volume, head-relative mode, listener factors |
| 3D placement | `SpatialMode::balance` (layer: DirectSound's default law on a stereo pair, near channel at unity) or `nue_spatial` PAN / BINAURAL / AMBISONIC, or an OS spatializer through `SpatialOptions::backend` | automatic: balance for a stereo output, pairwise panning above |
| Environment (reverb) | `nue_reverb` fed by the per-buffer sends; EAX 2 listener properties through `nue_reverb_params_from_eax` (`eax2_to_environment`), buffer obstruction/occlusion through `eax2_to_send` | the layer has the model; **there is no `IKsPropertySet` facade** |
| Output | `nue_audio_stream` (callback), channel count clamped to the device's native count | see `NueSink` |

Not duplicated from the port: its spatial renderers (stereo/speaker/binaural), its FDN reverb, its bed matrix, its 16-tap resampler. Their tests (ITD, ILD, pairwise
panning power, RT60) were not carried over either: they belong to `nue_audio`'s own tests.

## COM facade and `dsound.dll`

`com_facade.cpp` implements the interfaces of the Windows SDK's `dsound.h` as real COM objects over a `Device`: `DirectSoundCreate` (`DSDEVID_DefaultPlayback`,
`DSDEVID_DefaultVoicePlayback`, null; another GUID is `DSERR_NODRIVER`; aggregation is `CLASS_E_NOAGGREGATION`), `IDirectSound` (all methods; `GetSpeakerConfig`
answers from the output layout, `SetSpeakerConfig` is accepted and ignored; `Initialize` is `DSERR_ALREADYINITIALIZED`), `IDirectSoundBuffer` (all methods),
`IDirectSound3DListener` and `IDirectSound3DBuffer` (all methods, `DS3D_DEFERRED` settings applied by `CommitDeferredSettings`), `IDirectSoundNotify`. `QueryInterface`
gives the 3D buffer interface only for a `CTRL3D` secondary, the listener only from a `CTRL3D` primary, notify only with `CTRLPOSITIONNOTIFY`; `IDirectSound8`,
`IDirectSoundBuffer8` and `IKsPropertySet` (EAX) are `E_NOINTERFACE`. `Lock` hands out pointers into a per-buffer mirror; `Unlock` passes the bytes to the Device
(a client that reads back what it wrote sees its own data, not the mixer's). Notification events are real Win32 events (`SetEvent` from the mix thread).
Not offered: `DirectSoundCreate8`, capture and full duplex (they answer `DSERR_NODRIVER`), `CoCreateInstance(CLSID_DirectSound)`.

Every method counts its calls and failures; the report (`NueDsoundReport`, or `NUE_DSOUND_LOG`) lists them and the first failing call, which is what to send back after
a game session.

## Guest facade (a recompiled 32-bit game on any 64-bit or 32-bit host)

`include/nue_legacy/dsound_guest.hpp`, `src/legacy/dsound/guest/guest_facade.cpp`: `nue::dsound::guest::DsoundGuestFacade(Memory&, HostServices&, Device&)`. A recompiled
game calls `IDirectSound*` through guest memory: a COM pointer whose first word is a vtable of **call tokens**, arguments as stdcall words on a guest stack, structures at
guest addresses, 32-bit event handles. The facade turns that into `Device` calls and back. It is the part of the layer that knows the guest ABI, and nothing else:

```cpp
// include/nue_legacy/guest_memory.hpp (legacy_core will own it; the NFS Underground 2 port's guest::Memory is the same contract)
class nue::legacy::guest::Memory {
    uint32_t alloc(uint32_t bytes);                     // zeroed, 16-byte aligned, inside the guest address range; 0 on failure
    void     free(uint32_t address);
    uint8_t* host(uint32_t address, uint32_t bytes);    // host pointer for the whole range, nullptr when not mapped
    uint32_t address_of(const void* host_pointer);      // guest address of memory this object handed out (0 when foreign)
};
struct HostServices { void set_event(uint32_t guest_handle); bool is_window(uint32_t guest_hwnd); };
```

* **No host address assumption.** Every guest address goes through `Memory::host()`, checked for its whole range; there is no cast between a guest address and a pointer
  anywhere in the layer. Guest addresses are 32-bit values and the facade does its address arithmetic in 32 bits (the `Lock` mirror lives in guest memory: `Lock` returns
  guest addresses inside it and `Unlock` reads the bytes back through `host()`; a 64-bit host never exposes buffer memory to the guest, it only copies). Works with a 4 GiB
  arena at any base on any 64-bit little-endian host (x86-64, arm64; Windows, Linux, Android, macOS, iOS) and on a 32-bit identity host. Tested with the fake arena at guest
  base `0x10000` and at `0xA0000000`, host addresses unrelated to either, on x64 and x86.
* **Hostile guests do not fault the host.** A stale or foreign `this` fails with `E_INVALIDARG`; wild pointers and sizes are rejected by `host()` or by the checks; counts are
  bounded (a 32-bit `count * 8` overflow in `SetNotificationPositions` was found by the hostile-argument test and fixed here and in the COM facade). The test makes 5000
  random calls per seed, four seeds, over every method of every interface.
* **How calls reach it** (the glue, not in the library): register a call token for each of the 75 vtable slots (`DsoundGuestFacade::methods()`: interface, slot, stdcall words
  including `this`, name; `set_token(index, token)` before the first create), override the import `dsound.dll!DirectSoundCreate` (ordinal 1) to call
  `direct_sound_create(guid, ppDS, outer)`, and in the dispatcher read the words from the guest stack and call `call(index, args)` (`args[0]` is `this`, the return value is EAX).
  The port's `dsound_install.cpp` is the example (`NativePlatform::add_method_token`, `override_import("#1")`).

Vtable layout and argument words (this included), identical to the SDK order the COM facade implements:

| Interface | Slots (words popped) |
|---|---|
| IDirectSound (11) | QueryInterface 3, AddRef 1, Release 1, CreateSoundBuffer 4, GetCaps 2, DuplicateSoundBuffer 3, SetCooperativeLevel 3, Compact 1, GetSpeakerConfig 2, SetSpeakerConfig 2, Initialize 2 |
| IDirectSoundBuffer (21) | QI 3, AddRef 1, Release 1, GetCaps 2, GetCurrentPosition 3, GetFormat 4, GetVolume 2, GetPan 2, GetFrequency 2, GetStatus 2, Initialize 3, Lock 8, Play 4, SetCurrentPosition 2, SetFormat 2, SetVolume 2, SetPan 2, SetFrequency 2, Stop 1, Unlock 5, Restore 1 |
| IDirectSound3DBuffer (21) | QI 3, AddRef 1, Release 1, GetAllParameters 2, GetConeAngles 3, GetConeOrientation 2, GetConeOutsideVolume 2, GetMaxDistance 2, GetMinDistance 2, GetMode 2, GetPosition 2, GetVelocity 2, SetAllParameters 3, SetConeAngles 4, SetConeOrientation 5, SetConeOutsideVolume 3, SetMaxDistance 3, SetMinDistance 3, SetMode 3, SetPosition 5, SetVelocity 5 |
| IDirectSound3DListener (18) | QI 3, AddRef 1, Release 1, GetAllParameters 2, GetDistanceFactor 2, GetDopplerFactor 2, GetOrientation 3, GetPosition 2, GetRolloffFactor 2, GetVelocity 2, SetAllParameters 3, SetDistanceFactor 3, SetDopplerFactor 3, SetOrientation 8, SetPosition 5, SetRolloffFactor 3, SetVelocity 5, CommitDeferredSettings 1 |
| IDirectSoundNotify (4) | QI 3, AddRef 1, Release 1, SetNotificationPositions 3 |

The format, structure and validation code is duplicated between this facade and the COM one (they differ only in how memory is read). Sharing it (functions over a byte reader) is the
next cleanup and is cheap now that both exist and both are tested against the same semantics.

## Sinks: one interface, one implementation per platform

Everything the layer needs from an output is `OutputSink` (`dsound_engine.hpp`): `open(spec, callback)`, `actual()`, `start/stop/close`, `frames_played()` (a device clock),
`latency()` (the time from handing a frame to hearing it: the DirectSound play cursor is reported that far behind what was mixed, so the client's lead stays right), `stats()`.
The shipped sink, `make_nue_sink()`, is a `nue_audio_stream` with a callback on the default device (device 0, which follows the system default), so **every platform backend of
the engine is already a DirectSound output**; the layer contains no platform code.

| Platform | Output | Who provides it | State |
|---|---|---|---|
| Windows | WASAPI, through the engine's SDL3 backend (the port's own event-driven WASAPI sink also fits `OutputSink`) | engine (windows / core) | **run** here on one stereo endpoint, silence |
| Linux, SteamOS | PipeWire / ALSA (SDL3) | engine (linux) | not run |
| Android | AAudio / Oboe | engine (android); `NUE_AUDIO_LOW_LATENCY` selects the low-latency path | not run |
| macOS, iOS | CoreAudio | engine (ios) | not run |
| tests | `ManualSink` (tests) and the engine's null backend with its virtual clock | here | run |

A sink for any of them is accepted when it passes `ds_sink_tests.cpp` (the conformance test): format reported, nothing runs before `start`, latency reported, the clock is monotonic
and never ahead of what the callback produced, `stop` pauses with the position kept, nothing runs after `close`, a closed sink opens again, and through an `Engine` the DirectSound
play cursor of a looping ring moves at every 10 ms poll (a client rebuilds its audio after 100 identical ones) at the real-time rate. It runs headless on the manual sink and on
the native stream over the null backend; on hardware, `nue_dsound_endpoint_smoke` does the equivalent. The channel count is clamped to the device's native count
(`SinkDesc::match_device_channels`) so the layer's own down-mix runs, not the platform converter.

**The cursor must not freeze when the output goes away.** A client rebuilds its audio when the play cursor stands still for 100 polls, so an unplugged endpoint (the callback stops while
the device is re-acquired) must not stop the DirectSound clock. `EngineConfig::keep_clock_without_output` (on in `dsound.dll`, off in tests that drive virtual time) starts a watchdog
that, after `watchdog_ms` (150) without a callback, renders at the real-time rate into nothing and hands over the moment the sink calls back (`ds_sink_tests.cpp`: a sink that never calls
back keeps a moving cursor, and a live sink never wakes the watchdog). This is the port's WASAPI behaviour made independent of the backend. Not tried on a real unplug. Requests per platform are in [REQUESTS_dsound.md](REQUESTS_dsound.md).

## Level A trace: record, replay, compare

`include/nue_legacy/dsound_trace.hpp`. `EngineConfig::trace` (or `NUE_DSOUND_TRACE=<file>` for `dsound.dll`) records every call the client makes on the `Device` (arguments and answers,
every `Lock`/`Unlock` with the PCM written, every mixed block's hash) in a versioned binary file (`NDST`, version 1). `replay_trace()` runs it again on a fresh `Device` without the client
and stops at the first record whose answer differs, naming it. `nue_dsound_trace info|replay|pcm <file>` does it from the command line. The **PCM hash** (FNV-1a of every byte passed to
`Unlock`, in order) is independent of how the client mixes: two implementations of the same audio (for instance the game's own mixer translated ahead of time, and a native
reimplementation) are equal when they write the same bytes into the buffers; the test shows two clients with different refill patterns and equal hash. Recorded traces are
deterministic: the same session gives the same hashes at 32 and 64 bits.

## No interpreter and no JIT (iOS): what the DirectSound side needs

The DirectSound layer is ordinary host C++: no run-time code, nothing to interpret. The questions are about the client of the layer, the game's audio modules, which are x86 code loaded
from data files. What the layer needs and offers for each alternative of the port's `MEMORY_MODEL_64.md`:

* **A. The modules translated ahead of time.** Nothing changes below the buffers: the translated pages call the same facade through the same tokens, and the ring contract is the same
  (a play cursor that always moves, `Lock`/`Unlock` of what the mixer produced). The layer needs nothing from A. What A gives the layer is a **reference**: record a session of the
  translated modules with the trace; its PCM hash and its bytes are the ground truth for B.
* **B. A native reimplementation** (codecs / mixer on `nue_audio`). If B replaces only the codecs and keeps the game's mixer, the layer is unchanged. If B replaces the game's mixer,
  there is no ring to feed and the layer degenerates into an `OutputSink` consumer: B can write the PCM directly (to an `OutputSink`, or into the same buffers to keep the cursor
  contract). Either way the fidelity check is the trace: run the same inputs through A and B and compare the PCM hash (equal bytes); if they differ the byte offset says where.
* **What can be verified without the game**: the ring contract with synthetic clients (done: the cursor, the 100-poll rule, Lock/Unlock wrapping), the guest facade against a fake guest at
  any arena base and under hostile arguments (done), the sinks (conformance, done headless), the trace record/replay and its divergence report (done), and 32/64-bit determinism of
  the output (done: identical hashes). **What cannot**: that B sounds like A (needs a recorded session of A, so one run of the original game), and any real device other than the
  stereo endpoint tested here.

## Tests

All headless and deterministic unless marked. Presets: `windows-x64`, `windows-x86` (the 32-bit game's bitness). The ctest names are `nue_dsound_*`.

| Test | What it proves | Result |
|---|---|---|
| `nue_dsound_device` (`ds_device_tests.cpp`) | Level A: creation/validation matrix, Lock regions and flags, **bit-exact pass-through** at the output rate, 8/24/32-bit and float conversion, volume/pan/frequency/master volume levels, resampling error (1 kHz tone from 44.1 to 48 kHz, DC gain, 96 to 48 kHz), exact cursor values with 10 ms latency and 256-frame blocks, stop/seek/one-shot, a cursor that never stands still over 300 polls, chunking independence, notifications, `DuplicateSoundBuffer`, 5.1 to stereo/5.1 and stereo into 5.1, DS3D (distance law, cone, disable mode, validation), Doppler both ways, listener orientation, pairwise panning on a 5.1 output, binaural, the environment sends and EAX 2 mappings, the engine reopening for a 6-channel primary, the scripted session hash | 246 checks, x64 and x86 |
| `nue_dsound_com` (`ds_com_tests.cpp`) | the COM facade through the real `dsound.h` interfaces: a streaming client's probe (1/2/4/6 channels), 0.5 s ring, 200 refill polls with wrapping `Lock`s, teardown and re-creation, 5.1 reopening the sink; COM rules (shared refcount, `QueryInterface` graph, the device outliving its buffers); argument validation; Lock/Unlock discipline; formats; 3D listener and buffer with deferred settings; notification events | 984 checks, x64 and x86 |
| `nue_dsound_nue` (`ds_nue_tests.cpp`) | the layer on the native stack end to end: `nue_init` on the null backend, the native sink, a streaming client under the virtual clock, the played frames read back (523.25 Hz tone, level, both channels), two runs give the same samples **and the same hash as the manual sink**; a 5.1 primary on a stereo device | 32 checks, x64 and x86 |
| `nue_dsound_dll_test` (`ds_dll_tests.cpp`) | the built `dsound.dll` loaded by path: the exports by ordinal and by name, `DirectSoundCreate`, a ring that plays and whose cursor moves (null backend, real time), the other exports, the report, no failed call, and the trace it recorded replays identically | 30 checks, x64 and x86 |
| `nue_dsound_guest` (`ds_guest_tests.cpp`) | the guest-pointer facade through call tokens over a fake guest arena: the 969 checks of the port's facade tests, run with the arena at guest base `0x10000` and at `0xA0000000`, plus a hostile-argument pass (5000 random calls x 4 seeds over all 75 slots, stale `this`, wild pointers) | 1970 checks, x64 and x86 |
| `nue_dsound_sink` (`ds_sink_tests.cpp`) | the `OutputSink` conformance (clock, latency, stop/close/reopen) and the DirectSound cursor contract through an Engine, on the manual sink and on the native stream over the null backend | 847 checks, x64 and x86 |
| `nue_dsound_trace` (`ds_trace_tests.cpp`) | the Level A trace: record, replay on a fresh Device (every answer and mixed block equal), divergence named by record, damaged traces refused, two clients compared by the PCM hash, recording through the Engine; pinned hashes identical at 32 and 64 bits | 21 checks, x64 and x86 |
| `nue_dsound_differential` (`ds_differential_tests.cpp`, **tool, not in ctest** unless `-DNUE_DSOUND_SYSTEM_TESTS=ON`) | the same calls on the **system DirectSound** and on the layer: HRESULTs of about 120 argument combinations, `GetCaps`, Lock lengths, status, notifications, cursors in real time (the real write-play margin is 30.0 ms, the layer's 30.0 ms), a streaming client's own sequence. Plays only zero-filled buffers at volume -10000, never touches the system volume, skips itself without an output device | **296 comparisons, 0 differences**, x64 and x86 |
| `nue_dsound_endpoint_smoke` (tool, **real endpoint, silence**) | the engine on the default output: opens, the cursor runs at the real-time rate, 0 underruns, a 6-channel primary on a stereo endpoint keeps 2 channels and the layer down-mixes | x64: 8 checks (stereo Realtek endpoint, 48 kHz, 21.3 ms device latency) |

To verify by hand: `ctest --preset windows-x64 -R nue_dsound`; `build/<preset>/src/legacy/dsound/tests/nue_dsound_differential.exe -v` (prints every comparison);
`nue_dsound_endpoint_smoke.exe`; `python src/legacy/dsound/tools/dsound_inventory.py <game.exe>` for a new game.
The full suites (`ctest --preset windows-x64`, 42 tests, and `windows-x86`) also pass with this layer enabled, including the ABI guard.

## Deviations from the real DirectSound

| Deviation | Why |
|---|---|
| The primary buffer's `SetVolume` is a master gain of the layer's mixer. On Windows 11 it drives the endpoint volume. | Changing the system volume from a game is a bug, not a feature. |
| The play cursor is the mixed position minus the sink's reported latency, not a hardware clock. | There is none; the null sink and the virtual clock make it exact. The latency is read once when the output opens. |
| Resampling is `nue_resampler` (windowed sinc), not Windows' resampler. | Quality; both keep the level (tested). |
| No hardware voices (`DSCAPS` reports none); `LOCDEFER`, `STATIC`, `STICKYFOCUS`, `MUTE3DATMAXDISTANCE` are accepted and ignored; buffers are never lost by focus changes (`lose_buffers()` is a test hook). | `GLOBALFOCUS` is the effective behaviour. |
| `CTRLFX` (DirectSound8 effects) returns `E_NOTIMPL`; `IDirectSound8` is not offered. | Not used by the clients seen so far. |
| EAX (`IKsPropertySet`) is not exposed. The model exists (`Environment`, `EnvironmentSend`, `eax2_to_environment`, `eax2_to_send`). | Add the facade when a game needs it. |
| DirectSound3D uses the layer's spatial modes, not DirectSound's HRTF algorithms (`DS3DALG_HRTF_*` are accepted and ignored). | Spatialisation is the player's choice (`SpatialOptions`). |
| `GetSpeakerConfig` answers from the output layout; `SetSpeakerConfig` is accepted and ignored. | The output follows the device and the primary format. |
| A multichannel primary on a device with fewer channels keeps the device's channels; the application's format is accepted. | The layer's ITU down-mix beats the platform converter (`SinkDesc::match_device_channels`). |
| Stats: `underruns` of the mixer are not counted (the sink counts its own). | |
| `Lock` returns the layer's mirror, so a client reading back a locked region sees its own earlier writes, not the mixer's data. | Level A does not expose the mixing ring. |
| The dll is not unloadable in practice (the native audio backend pins it); the engine runs until the process ends. | Games never unload `dsound.dll`; `NueDsoundShutdown()` stops it explicitly. |

## What is implemented / tested / only written

| Item | State |
|---|---|
| Level A device: primary + secondary buffers, Lock/Unlock, cursors, status, volume/pan/frequency, notifications, duplicate, 3D, environment | implemented, tested (units + 296 comparisons with the real DirectSound) |
| Rate conversion, layouts, 3D placement, reverb through `nue::audio` | implemented, tested headless (levels, positions, hashes); **never heard** |
| Native output sink (`nue_audio_stream`), channel follow, latency | implemented; tested on the null backend and on **one** real stereo endpoint with silence |
| COM facade, `dsound.dll` (x64 and x86 build and tests) | implemented, tested through COM and through the built dll |
| Guest-pointer facade | implemented and tested against a fake guest at two arena bases and under hostile arguments; **not wired to a real guest** (needs the dispatcher of `legacy_core` or the port's launcher) |
| Level A trace (record / replay / PCM hash) | implemented, tested, identical at 32 and 64 bits; never used on a real game |
| `IKsPropertySet`/EAX facade, `DirectSound8`, capture | not written |
| Binaural / ambisonic / OS spatializer modes | implemented as plumbing to `nue_spatial` and tested for level/position; no measured HRTF, no OS backend |
| Linux, Android, iOS | the portable part (Level A, engine, sink) is plain C++20 on `nue::audio`; compiled and run only on Windows with clang (the strict-warning pass with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wold-style-cast` is clean); **GCC and the Linux job were not run here** |

## Missing or not verified

* **No audible run of a game.** The first one is the player's; send back the `NueDsoundReport` text.
* **No 5.1/7.1 endpoint test.** The 6-channel path is tested on the null device (stream opened with 6 channels) and on a stereo endpoint (stays at 2); a real 5.1/7.1
  endpoint, exclusive mode and Windows spatial formats are untested.
* **Endpoint loss was simulated** (a sink that stops calling back), not exercised with a real unplug.
* **No follow of a default-device change in the layer.** The native stream follows the system default device by itself; the layer reads the device latency once when
  it opens the output, so a new device with another latency or rate keeps the old play-cursor offset. (Request to the platform sessions in `REQUESTS_dsound.md`.)
* **No platform spatializer backend** (`nue_spatial_backend_ops` is passed through, nothing implements it), **no measured HRTF** (`Device::set_hrtf` forwards a dataset to
  `nue_spatial_set_hrtf`; there is no SOFA reader), **no height layers**.
* **No `IKsPropertySet`/EAX facade**; EAX 1, 3 and 4 mappings are not written; the environment-id tables of EAX 1/2 are missing in `nue_audio`.
* The trace records the client's calls on the Device; it does not record the guest (a recorded session of the original game is still needed as the reference for any native reimplementation).
* `DuplicateSoundBuffer` copies the data (no sharing of static data), hardware voices, focus-loss muting for non-`GLOBALFOCUS` buffers, `DirectSoundCaptureCreate`.
* Linux/GCC, Android and iOS builds of this layer were not run.
