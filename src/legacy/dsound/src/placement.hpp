// Placement: how the voices of a Device reach the output channels. DirectSound-specific glue over the native audio module:
//   - a 2D buffer (mono or stereo) goes to the front pair with its per-channel gains (DirectSound's pan law is applied by the caller),
//   - a multichannel 2D buffer (a "bed") is mapped to the output layout by nue_chanmix,
//   - a 3D buffer is a point source: the scene maths is nue_spatial_evaluate and the placement is either DirectSound's own balance law
//     (the near channel stays at unity) or a nue_spatial renderer (pairwise panning, binaural, ambisonic, or an OS spatializer backend),
//   - the environment is a nue_reverb fed by the per-source sends.
// Nothing here knows COM or guest memory. Thread: the owner serialises every call (the Device holds one mutex around them).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "nue_legacy/dsound_device.hpp"

namespace nue::dsound::detail {

enum class SourceKind : uint8_t {
    object,      // mono (or mixed down) source placed in 3D
    stereo2d,    // mono or stereo, not placed: gain per channel (volume and pan already applied), routed to the front pair
    bed          // speaker feeds in `mask` order (multichannel, 2D): mapped to the output layout
};

// One block of one source for one render call. `data` is interleaved float32.
struct SourceBlock {
    uint32_t id = 0;
    SourceKind kind = SourceKind::stereo2d;
    const float* data = nullptr;
    uint32_t channels = 0;
    uint32_t mask = 0;               // speaker mask of the channels (0 = default for the channel count)
    float gain[2] = {1, 1};          // stereo2d: left, right gain; object and bed: gain[0] is the overall gain
    EnvironmentSend send;
};

class Placement {
public:
    Placement();
    ~Placement();
    Placement(const Placement&) = delete;
    Placement& operator=(const Placement&) = delete;

    // (Re)allocates all state for an output layout and the largest block that will be asked for. Resets the sources.
    void configure(const OutputLayout& output, uint32_t max_block_frames, uint32_t max_sources, const SpatialOptions& options);
    void set_listener(const Listener3D& listener);
    void set_environment(const Environment& environment);
    void set_object(uint32_t id, const Buffer3D& params);
    void remove_source(uint32_t id);
    void set_hrtf(std::span<const float> azimuth_deg, std::span<const float> elevation_deg, std::span<const float> irs, uint32_t ir_frames, uint32_t ir_sample_rate);
    // Playback-rate ratio the Doppler effect asks for this object right now (1 = none).
    float pitch_ratio(uint32_t id) const;
    // Adds `frames` frames of every input to `out` (interleaved, output channels). frames <= the max_block_frames given to configure.
    void render(std::span<const SourceBlock> inputs, float* out, uint32_t frames);
    const char* name() const { return name_; }

private:
    struct Impl;
    Impl* impl_;
    const char* name_ = "balance";
};

}  // namespace nue::dsound::detail
