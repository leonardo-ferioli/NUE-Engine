// Sample format / channel / rate conversion primitives shared by audio streams and the mixer (float domain, interleaved).
#pragma once

#include "nue/audio.h"

#include <vector>

namespace nuei {

uint32_t bytes_per_sample(nue_audio_format f);
void to_float(const nue_audio_spec& spec, const void* src, uint32_t frames, float* dst);          // dst: frames * channels floats
void from_float(const nue_audio_spec& spec, const float* src, uint32_t frames, void* dst);
void remap_channels(const float* src, uint32_t src_ch, uint32_t frames, float* dst, uint32_t dst_ch);

// Streaming linear resampler with an input FIFO, so block boundaries are seamless.
class Resampler {
public:
    void reset(uint32_t channels);
    // Input frames that must be pushed before produce(out_frames, step) can deliver all of them.
    uint32_t input_needed(uint32_t out_frames, double step) const;
    void push(const float* frames, uint32_t count);
    uint32_t produce(uint32_t out_frames, double step, float* out);   // step = input_rate / output_rate; returns frames made
private:
    uint32_t channels_ = 2;
    double pos_ = 0;
    std::vector<float> fifo_;
};

}  // namespace nuei
