// legacy_dsound: common types of the DirectSound layer. No Windows type, no guest pointer, no COM object: DirectSound's published constants
// (HRESULTs, flags, WAVEFORMATEX fields) are plain integers so that Level A reads like the API it serves. See docs/LEGACY_DSOUND.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace nue::dsound {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

using Bytes = std::span<const std::byte>;

// Speaker positions as in WAVEFORMATEXTENSIBLE (dwChannelMask); the channels of interleaved data are in the order of the set bits. These are
// the same bits as NUE_SPK_* in nue/audio_dsp.h.
namespace speaker {
constexpr uint32_t front_left = 0x1, front_right = 0x2, front_center = 0x4, lfe = 0x8, back_left = 0x10, back_right = 0x20,
                   front_left_of_center = 0x40, front_right_of_center = 0x80, back_center = 0x100, side_left = 0x200, side_right = 0x400;
constexpr uint32_t mono = front_center, stereo = front_left | front_right, quad = front_left | front_right | back_left | back_right,
                   surround_5_1 = front_left | front_right | front_center | lfe | back_left | back_right,
                   surround_7_1 = surround_5_1 | side_left | side_right;
// Mask for a channel count when the format does not carry one (what Windows assumes).
constexpr uint32_t default_mask(uint32_t channels) {
    switch (channels) {
        case 1: return mono;
        case 2: return stereo;
        case 3: return front_left | front_right | front_center;
        case 4: return quad;
        case 5: return front_left | front_right | front_center | back_left | back_right;
        case 6: return surround_5_1;
        case 8: return surround_7_1;
        default: return 0;
    }
}
constexpr uint32_t count(uint32_t mask) {
    uint32_t n = 0;
    for (; mask; mask &= mask - 1) ++n;
    return n;
}
}  // namespace speaker

enum class SampleType : uint8_t { pcm, ieee_float };

// What a buffer or the primary buffer holds. `bits` is the container size (8 unsigned, 16/24/32 signed, 32 float).
struct WaveFormat {
    SampleType type = SampleType::pcm;
    uint16_t channels = 2;
    uint32_t sample_rate = 44100;
    uint16_t bits = 16;
    uint32_t channel_mask = 0;   // 0 = default for the channel count
    uint32_t frame_bytes() const { return uint32_t(channels) * (bits / 8u); }
    uint32_t effective_mask() const { return channel_mask ? channel_mask : speaker::default_mask(channels); }
    friend bool operator==(const WaveFormat&, const WaveFormat&) = default;
};

// The format the layer renders in: interleaved float32.
struct OutputLayout {
    uint32_t sample_rate = 48000;
    uint32_t channels = 2;
    uint32_t channel_mask = speaker::stereo;
    friend bool operator==(const OutputLayout&, const OutputLayout&) = default;
};

}  // namespace nue::dsound
