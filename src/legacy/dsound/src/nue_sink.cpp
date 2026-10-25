// The native output sink: a nue_audio_stream with a callback (include/nue/audio.h). Everything the OS and the device negotiate (WASAPI, SDL, AAudio...)
// happens underneath; on a headless run the null backend consumes the stream with a virtual clock.
#include <string>

#include "nue/audio.h"
#include "nue_legacy/dsound_engine.hpp"

namespace nue::dsound {
namespace {

class NueSink final : public OutputSink {
public:
    ~NueSink() override { close(); }

    bool open(const SinkDesc& desc, SinkCallback callback, void* user) override {
        close();
        callback_ = callback;
        user_ = user;
        uint32_t rate = desc.spec.sample_rate;
        if (rate == 0) {
            nue_audio_device_info info{};
            info.struct_size = sizeof(info);
            rate = nue_audio_get_device_info(0, &info) == NUE_OK && info.preferred.sample_rate ? info.preferred.sample_rate : 48000;
        }
        uint32_t channels = desc.spec.channels ? desc.spec.channels : 2;
        for (int attempt = 0; attempt < 2; ++attempt) {
            nue_audio_stream_desc d{};
            d.struct_size = sizeof(d);
            d.device = 0;
            d.spec = {NUE_AUDIO_F32, channels, rate};
            d.flags = NUE_AUDIO_START_PAUSED | (desc.low_latency ? NUE_AUDIO_LOW_LATENCY : 0u);
            d.callback = &NueSink::trampoline;
            d.userdata = this;
            d.latency_ms = desc.latency_ms;
            const std::string name(desc.name);
            d.name = name.c_str();
            if (nue_audio_stream_create(&d, &stream_) != NUE_OK) {
                stream_ = nullptr;
                return false;
            }
            nue_audio_spec app{}, dev{};
            if (nue_audio_stream_get_spec(stream_, &app, &dev) == NUE_OK && attempt == 0 && channels > 2 && dev.channels >= 2 && dev.channels < channels) {
                // The device has fewer channels than the application format: the layer's own down-mix (nue_chanmix, ITU gains) is better than the
                // engine's converter, so ask for the device's layout instead.
                nue_audio_stream_destroy(stream_);
                stream_ = nullptr;
                channels = dev.channels;
                continue;
            }
            break;
        }
        actual_ = {rate, channels, speaker::default_mask(channels)};
        return true;
    }
    OutputLayout actual() const override { return actual_; }
    bool start() override { return stream_ && nue_audio_stream_start(stream_) == NUE_OK; }
    bool stop() override { return stream_ && nue_audio_stream_stop(stream_) == NUE_OK; }
    void close() override {
        if (stream_) nue_audio_stream_destroy(stream_);
        stream_ = nullptr;
    }
    bool running() const override { return stream_ && nue_audio_stream_is_running(stream_); }
    uint64_t frames_played() const override {
        uint64_t frames = 0;
        int64_t when = 0;
        if (stream_) nue_audio_stream_get_position(stream_, &frames, &when);
        return frames;
    }
    SinkLatency latency() const override {
        SinkLatency l;
        nue_audio_latency q{};
        q.struct_size = sizeof(q);
        if (stream_ && nue_audio_stream_get_latency(stream_, &q) == NUE_OK) {
            l.device_buffer_frames = q.device_buffer_frames;
            l.device_latency_ns = q.device_latency_ns;
            l.total_latency_ns = q.total_latency_ns;
        }
        return l;
    }
    void* native_handle() const override { return stream_; }
    SinkStats stats() const override {
        SinkStats s;
        nue_audio_stream_stats q{};
        q.struct_size = sizeof(q);
        if (stream_ && nue_audio_stream_get_stats(stream_, &q) == NUE_OK) {
            s.frames_played = q.frames_played;
            s.underruns = q.underruns;
            s.peak_level = q.peak_level;
            s.cpu_load = q.cpu_load;
        }
        return s;
    }

private:
    static void NUE_CALL trampoline(void* self, void* out, uint32_t frames) {
        NueSink* s = static_cast<NueSink*>(self);
        s->callback_(s->user_, static_cast<float*>(out), frames);
    }
    nue_audio_stream stream_ = nullptr;
    OutputLayout actual_;
    SinkCallback callback_ = nullptr;
    void* user_ = nullptr;
};

}  // namespace

std::unique_ptr<OutputSink> make_nue_sink() { return std::make_unique<NueSink>(); }

}  // namespace nue::dsound
