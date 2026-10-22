// NUE C++ wrapper: audio streams and the software mixer.
#pragma once

#include "nuepp/core.hpp"

namespace nuepp {

// Output stream. Push model by default; pass a callback (audio thread, real-time rules) for the callback model.
class AudioStream {
public:
    AudioStream() = default;
    explicit AudioStream(const nue_audio_stream_desc& desc) { result_ = Result(nue_audio_stream_create(&desc, &s_)); }
    AudioStream(nue_audio_spec spec, nue_audio_callback cb = nullptr, void* userdata = nullptr, uint32_t latency_ms = 0) {
        nue_audio_stream_desc d = init<nue_audio_stream_desc>();
        d.spec = spec; d.callback = cb; d.userdata = userdata; d.latency_ms = latency_ms;
        result_ = Result(nue_audio_stream_create(&d, &s_));
    }
    AudioStream(AudioStream&& o) noexcept : s_(o.s_), result_(o.result_) { o.s_ = nullptr; }
    AudioStream& operator=(AudioStream&& o) noexcept { if (this != &o) { reset(); s_ = o.s_; result_ = o.result_; o.s_ = nullptr; } return *this; }
    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;
    ~AudioStream() { reset(); }
    void reset() { if (s_) { nue_audio_stream_destroy(s_); s_ = nullptr; } }

    bool ok() const { return s_ != nullptr; }
    const Result& result() const { return result_; }
    nue_audio_stream handle() const { return s_; }
    Result start() { return nue_audio_stream_start(s_); }
    Result stop() { return nue_audio_stream_stop(s_); }
    Result flush() { return nue_audio_stream_flush(s_); }
    // Returns frames accepted.
    uint32_t push(const void* frames, uint32_t count) { uint32_t n = 0; nue_audio_stream_push(s_, frames, count, &n); return n; }
    uint32_t queued_frames() const { return nue_audio_stream_queued_frames(s_); }
    Result set_volume(float v) { return nue_audio_stream_set_volume(s_, v); }
    nue_audio_latency latency() const { nue_audio_latency l = init<nue_audio_latency>(); nue_audio_stream_get_latency(s_, &l); return l; }
    nue_audio_stream_stats stats() const { nue_audio_stream_stats s = init<nue_audio_stream_stats>(); nue_audio_stream_get_stats(s_, &s); return s; }
private:
    nue_audio_stream s_ = nullptr;
    Result result_;
};

// Software mixer (output stream attached unless NUE_MIXER_MANUAL). Voices and buffers are plain ids owned by the mixer.
class Mixer {
public:
    Mixer() = default;
    explicit Mixer(const nue_mixer_desc& desc) { result_ = Result(nue_mixer_create(&desc, &m_)); }
    static Mixer manual(uint32_t sample_rate = 48000, uint32_t channels = 2, uint32_t block_frames = 256) {
        nue_mixer_desc d = init<nue_mixer_desc>();
        d.flags = NUE_MIXER_MANUAL; d.sample_rate = sample_rate; d.channels = channels; d.block_frames = block_frames;
        return Mixer(d);
    }
    Mixer(Mixer&& o) noexcept : m_(o.m_), result_(o.result_) { o.m_ = nullptr; }
    Mixer& operator=(Mixer&& o) noexcept { if (this != &o) { reset(); m_ = o.m_; result_ = o.result_; o.m_ = nullptr; } return *this; }
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;
    ~Mixer() { reset(); }
    void reset() { if (m_) { nue_mixer_destroy(m_); m_ = nullptr; } }

    bool ok() const { return m_ != nullptr; }
    const Result& result() const { return result_; }
    nue_mixer handle() const { return m_; }

    Expected<nue_buffer_id> make_buffer(const nue_audio_spec& spec, const void* data, uint32_t frames, uint32_t loop_start = 0, uint32_t loop_end = 0) {
        nue_buffer_desc d = init<nue_buffer_desc>();
        d.spec = spec; d.data = data; d.frame_count = frames; d.copy_data = NUE_TRUE; d.loop_start = loop_start; d.loop_end = loop_end;
        nue_buffer_id id = 0;
        Result r(nue_buffer_create(m_, &d, &id));
        if (!r) return r;
        return id;
    }
    // Starts a one-shot (auto released) or looping sound from a buffer.
    Expected<nue_voice_id> play(nue_buffer_id buffer, bool loop = false, float volume = 1.0f, float pan = 0.0f, nue_bus_id bus = 0) {
        nue_voice_desc d = init<nue_voice_desc>();
        d.kind = NUE_VOICE_STATIC; d.buffer = buffer; d.bus = bus; d.volume = volume; d.pan = pan;
        d.flags = NUE_VOICE_START_PLAYING | (loop ? NUE_VOICE_LOOP : NUE_VOICE_AUTO_RELEASE);
        nue_voice_id v = 0;
        Result r(nue_voice_create(m_, &d, &v));
        if (!r) return r;
        return v;
    }
    Result stop(nue_voice_id v) { return nue_voice_stop(m_, v); }
    Result set_volume(nue_voice_id v, float x) { return nue_voice_set_volume(m_, v, x); }
    Result set_master_volume(float x) { return nue_mixer_set_master_volume(m_, x); }
    Expected<nue_bus_id> make_bus(nue_bus_id parent = 0) { nue_bus_id b = 0; Result r(nue_bus_create(m_, parent, &b)); if (!r) return r; return b; }
    // Manual mode: renders interleaved float32 frames.
    Result render(float* out, uint32_t frames) { return nue_mixer_render(m_, out, frames); }
    nue_mixer_stats stats() const { nue_mixer_stats s = init<nue_mixer_stats>(); nue_mixer_get_stats(m_, &s); return s; }
private:
    nue_mixer m_ = nullptr;
    Result result_;
};

}  // namespace nuepp
