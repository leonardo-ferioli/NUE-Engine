// Level A of the DirectSound layer (legacy_dsound): "what the application says", with DirectSound's semantics, as a handle-based C++ interface.
//   - no guest pointer, no COM object, no Windows type: buffers are `BufferId`, bytes cross as spans valid for the call,
//   - results are DirectSound's HRESULT values as plain integers (the published constants), so that a facade passes them through and the
//     differential test can compare them with the real system DirectSound,
//   - nothing about any game.
// Implementation (src/device.cpp): a software mixer with DirectSound's cursor, Lock/Unlock, status and control semantics. Resampling,
// channel-layout conversion, spatial placement and reverb are done by the native audio module (nue::audio: nue_resampler, nue_chanmix,
// nue_spatial, nue_reverb); the output goes through an OutputSink (include/nue_legacy/dsound_engine.hpp), by default a nue_audio_stream.
// It never touches the system DirectSound.
//
// Thread: every method may be called from any thread (one internal mutex); render() is the mix thread. Lock/Unlock never wait for the mixer
// for longer than one block.
#pragma once
#include <cstdint>
#include <memory>
#include <span>

#include "nue/audio_dsp.h"
#include "nue/audio_spatial.h"
#include "nue_legacy/dsound_types.hpp"

namespace nue::dsound {

using Hr = int32_t;
constexpr bool succeeded(Hr h) { return h >= 0; }

// DirectSound HRESULTs (dsound.h; the values Windows returns).
namespace hr {
constexpr Hr ok = 0;
constexpr Hr allocated = Hr(0x8878000A), control_unavailable = Hr(0x8878001E), invalid_call = Hr(0x88780032), priority_level_needed = Hr(0x88780046),
             bad_format = Hr(0x88780064), no_driver = Hr(0x88780078), already_initialized = Hr(0x88780082), buffer_lost = Hr(0x88780096),
             other_app_has_priority = Hr(0x887800A0), uninitialized = Hr(0x887800AA), buffer_too_small = Hr(0x887800B4);
constexpr Hr invalid_param = Hr(0x80070057), out_of_memory = Hr(0x8007000E), unsupported = Hr(0x80004001), no_interface = Hr(0x80004002),
             generic = Hr(0x80004005), no_aggregation = Hr(0x80040110), access_denied = Hr(0x80070005);
}  // namespace hr

// DirectSound flag values (dsound.h).
namespace caps_flag {
constexpr uint32_t primary_mono = 0x1, primary_stereo = 0x2, primary_8bit = 0x4, primary_16bit = 0x8, continuous_rate = 0x10, emul_driver = 0x20,
                   secondary_mono = 0x100, secondary_stereo = 0x200, secondary_8bit = 0x400, secondary_16bit = 0x800;
}
namespace buffer_flag {
constexpr uint32_t primary = 0x1, static_buffer = 0x2, loc_hardware = 0x4, loc_software = 0x8, ctrl_3d = 0x10, ctrl_frequency = 0x20, ctrl_pan = 0x40,
                   ctrl_volume = 0x80, ctrl_position_notify = 0x100, ctrl_fx = 0x200, sticky_focus = 0x4000, global_focus = 0x8000,
                   get_current_position2 = 0x10000, mute_3d_at_max_distance = 0x20000, loc_defer = 0x40000;
}
namespace status_flag {
constexpr uint32_t playing = 0x1, buffer_lost = 0x2, looping = 0x4, loc_hardware = 0x8, loc_software = 0x10, terminated = 0x20;
}
namespace play_flag { constexpr uint32_t looping = 0x1; }
namespace lock_flag { constexpr uint32_t from_write_cursor = 0x1, entire_buffer = 0x2; }
namespace coop { constexpr uint32_t normal = 1, priority = 2, exclusive = 3, write_primary = 4; }
constexpr int32_t volume_min = -10000, volume_max = 0, pan_left = -10000, pan_right = 10000;
constexpr uint32_t frequency_min = 100, frequency_max = 200000, size_min = 4, size_max = 0x0FFFFFFF;
constexpr uint32_t notify_offset_stop = 0xFFFFFFFFu;   // DSBPN_OFFSETSTOP

struct BufferId {
    uint32_t value = 0;
    explicit operator bool() const { return value != 0; }
    friend bool operator==(BufferId, BufferId) = default;
};

struct Caps {
    uint32_t flags = 0;
    uint32_t min_secondary_rate = frequency_min, max_secondary_rate = frequency_max;
    uint32_t primary_buffers = 1;
    uint32_t max_hw_buffers = 0;   // no hardware voices: everything is mixed in software
};

struct BufferDesc {
    uint32_t flags = 0;
    uint32_t bytes = 0;
    bool has_format = false;       // a secondary buffer needs one; the primary must not have one
    WaveFormat format;
};

struct BufferCaps {
    uint32_t flags = 0, bytes = 0;
};

struct CreateResult {
    Hr hr = hr::generic;
    BufferId id;
    bool existing_primary = false;   // CreateSoundBuffer for the primary returns the same object again; the facade AddRefs it
};

// Where a Lock landed: byte offsets into the buffer (a facade stages the bytes where the application can reach them and hands them back at Unlock).
struct Regions {
    uint32_t offset1 = 0, bytes1 = 0, offset2 = 0, bytes2 = 0;
};

struct Notify {
    uint32_t offset = 0;     // byte offset, or notify_offset_stop
    uint64_t token = 0;      // opaque to the device (a facade stores an event handle)
};
using NotifyFn = void (*)(void* user, uint64_t token);   // runs on the mix thread, outside the device lock; should only signal
using FormatChangedFn = void (*)(void* user, const WaveFormat& primary);   // after a successful primary SetFormat, outside the lock

struct Listener3D {
    Vec3 position{0, 0, 0}, velocity{0, 0, 0}, front{0, 0, 1}, top{0, 1, 0};
    float distance_factor = 1.0f, rolloff_factor = 1.0f, doppler_factor = 1.0f;
};
enum class Mode3D : uint32_t { normal = 0, head_relative = 1, disable = 2 };
struct Buffer3D {
    Vec3 position{0, 0, 0}, velocity{0, 0, 0};
    uint32_t inside_cone_angle = 360, outside_cone_angle = 360;
    Vec3 cone_orientation{0, 0, 1};
    int32_t cone_outside_volume = 0;   // hundredths of dB
    float min_distance = 1.0f, max_distance = 1e9f;
    Mode3D mode = Mode3D::normal;
};

// The environment (what EAX / I3DL2 map to): the EFX model of nue/audio_dsp.h. Only one environment is active at a time.
struct Environment {
    bool enabled = false;
    nue_reverb_params params = default_params();
    static nue_reverb_params default_params() {
        nue_reverb_params p;
        nue_reverb_params_default(&p);
        return p;
    }
};

// Per-source send into the environment and the effect of obstruction/occlusion on the direct path (EAX buffer properties map onto it).
struct EnvironmentSend {
    float direct_gain = 1.0f, direct_gain_hf = 1.0f;     // linear; the HF value is the gain at high frequencies relative to low
    float send_gain = 1.0f, send_gain_hf = 1.0f;
};

// How 3D buffers and the environment reach the speakers.
enum class SpatialMode : uint8_t {
    automatic,    // balance for a stereo output, pan for more channels
    balance,      // DirectSound's own pan law on the front pair: the near channel stays at unity, the far one falls as cos(pi/2 * lateral)
    pan,          // nue_spatial NUE_SPATIAL_PAN: sine-law stereo, pairwise panning for any speaker mask
    binaural,     // nue_spatial NUE_SPATIAL_BINAURAL (stereo headphones)
    ambisonic     // nue_spatial NUE_SPATIAL_AMBISONIC
};
struct SpatialOptions {
    SpatialMode mode = SpatialMode::automatic;
    uint32_t ambisonic_order = 1;
    const nue_spatial_backend_ops* backend = nullptr;   // an OS spatializer or object renderer (nue_spatial_backend_ops); must outlive the device
};

struct Config {
    uint32_t max_buffers = 128;
    uint32_t block_frames = 256;           // mixing granularity; also the safety margin of the write cursor
    uint32_t write_margin_ms = 30;         // the write cursor is at least this far ahead of the play cursor (Windows: about 30 ms, measured)
    uint32_t max_primary_channels = 8;     // SetFormat on the primary refuses more (a game probes the channel count this way)
    uint32_t max_primary_rate = 200000;
    uint32_t default_rate = 44100;         // primary format before the application sets one
    uint32_t default_channels = 2;
    nue_resample_quality resample_quality = NUE_RESAMPLE_MEDIUM;   // nue_resampler quality of the buffers whose rate differs from the output
};

struct DeviceStats {
    uint32_t buffers = 0, playing = 0;
    uint64_t blocks_mixed = 0, clipped_samples = 0, underruns = 0;   // underrun: not counted by the mixer (the output sink counts its own)
    float peak = 0;
};

class Device {
public:
    virtual ~Device() = default;

    // ---- IDirectSound
    virtual Hr set_cooperative_level(uint32_t level) = 0;
    // A new DirectSound object after the last one was released: destroys every buffer and restores the defaults (cooperative level, primary
    // format, listener, environment).
    virtual void reset() = 0;
    virtual Caps caps() const = 0;
    virtual CreateResult create_buffer(const BufferDesc& desc) = 0;
    virtual CreateResult duplicate_buffer(BufferId source) = 0;
    virtual void destroy_buffer(BufferId id) = 0;   // last Release()

    // ---- IDirectSoundBuffer (primary and secondary)
    virtual Hr set_format(BufferId id, const WaveFormat& format) = 0;   // primary only
    virtual Hr get_format(BufferId id, WaveFormat& out) const = 0;
    virtual Hr get_caps(BufferId id, BufferCaps& out) const = 0;
    virtual Hr lock(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags, Regions& out) = 0;
    // `part1`/`part2` are the bytes the application wrote into the regions Lock returned (sizes equal to the regions' bytes).
    virtual Hr unlock(BufferId id, const Regions& regions, Bytes part1, Bytes part2) = 0;
    virtual Hr play(BufferId id, uint32_t priority, uint32_t flags) = 0;
    virtual Hr stop(BufferId id) = 0;
    virtual Hr get_status(BufferId id, uint32_t& out) const = 0;
    virtual Hr get_current_position(BufferId id, uint32_t* play_cursor, uint32_t* write_cursor) const = 0;
    virtual Hr set_current_position(BufferId id, uint32_t byte_position) = 0;
    virtual Hr restore(BufferId id) = 0;
    virtual Hr set_volume(BufferId id, int32_t millibels) = 0;
    virtual Hr get_volume(BufferId id, int32_t& out) const = 0;
    virtual Hr set_pan(BufferId id, int32_t millibels) = 0;
    virtual Hr get_pan(BufferId id, int32_t& out) const = 0;
    virtual Hr set_frequency(BufferId id, uint32_t hz) = 0;   // 0 = the buffer's own rate
    virtual Hr get_frequency(BufferId id, uint32_t& out) const = 0;
    virtual Hr set_notifications(BufferId id, std::span<const Notify> positions) = 0;   // empty clears

    // ---- 3D (IDirectSound3DListener / IDirectSound3DBuffer) and environment (what EAX maps to)
    virtual Hr set_listener(const Listener3D& listener) = 0;
    virtual Listener3D listener() const = 0;
    virtual Hr set_buffer_3d(BufferId id, const Buffer3D& params) = 0;
    virtual Hr get_buffer_3d(BufferId id, Buffer3D& out) const = 0;
    virtual void set_environment(const Environment& environment) = 0;
    virtual Hr set_buffer_send(BufferId id, const EnvironmentSend& send) = 0;

    // ---- Spatial rendering (not part of DirectSound: how the output is spatialised is the player's choice)
    virtual void set_spatial(const SpatialOptions& options) = 0;
    // A measured HRTF set for the binaural mode, as nue_spatial_set_hrtf takes it (copied; kept across output changes).
    virtual void set_hrtf(std::span<const float> azimuth_deg, std::span<const float> elevation_deg, std::span<const float> irs, uint32_t ir_frames,
                          uint32_t ir_sample_rate) = 0;
    virtual const char* spatial_name() const = 0;   // what renders objects now: "balance", "pan", "binaural", "ambisonic" or a backend's name

    // ---- Output side
    // Declares the format `render` produces; `latency_frames` is what the sink adds between render and the speaker (the play cursor is
    // reported that far behind what was mixed).
    virtual void set_output(const OutputLayout& layout, uint32_t latency_frames) = 0;
    virtual OutputLayout output() const = 0;
    virtual void set_notify_callback(NotifyFn fn, void* user) = 0;
    virtual void set_format_callback(FormatChangedFn fn, void* user) = 0;
    // [mix thread] Mixes `frames` frames of interleaved float32 in the output layout (overwrites `out`).
    virtual void render(float* out, uint32_t frames) = 0;
    virtual DeviceStats stats() const = 0;
    // Test hook: marks every buffer lost, as when the application loses focus (Lock/Play then return buffer_lost until restore()).
    virtual void lose_buffers() = 0;
};

std::unique_ptr<Device> make_device(const Config& config = {});

// Unit conversions shared with the tests and the facades.
float millibels_to_amplitude(int32_t millibels);          // 10^(mB/2000); <= -10000 is silence
void pan_gains(int32_t pan_millibels, float& left, float& right);

// EAX 2.0 (the version DirectSound games used most) -> the layer's model. Fields and units are EAX 2.0's (EAXLISTENERPROPERTIES /
// EAXBUFFERPROPERTIES): levels in hundredths of dB. Listener: through nue_reverb_params_from_eax; buffer: obstruction and occlusion.
struct Eax2Listener {
    int32_t room = -1000, room_hf = -100;
    float room_rolloff = 0.0f, decay_time = 1.49f, decay_hf_ratio = 0.83f;
    int32_t reflections = -2602;
    float reflections_delay = 0.007f;
    int32_t reverb = 200;
    float reverb_delay = 0.011f;
    uint32_t environment = 0;
    float environment_size = 7.5f, environment_diffusion = 1.0f, air_absorption_hf = -5.0f;   // diffusion 0..1, air absorption in dB
    uint32_t flags = 0;
};
struct Eax2Buffer {
    int32_t direct = 0, direct_hf = 0, room = 0, room_hf = 0;
    float room_rolloff = 0.0f;
    int32_t obstruction = 0;
    float obstruction_lf_ratio = 0.0f;
    int32_t occlusion = 0;
    float occlusion_lf_ratio = 0.25f, occlusion_room_ratio = 1.5f;
    float outside_volume_hf = 0.0f, air_absorption_factor = 1.0f;
    uint32_t flags = 0;
};
Environment eax2_to_environment(const Eax2Listener& listener);
EnvironmentSend eax2_to_send(const Eax2Buffer& buffer);

}  // namespace nue::dsound
