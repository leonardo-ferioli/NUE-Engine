// Audio: pure conversion code and the Null stream (no OS audio API beyond timers). The WASAPI stream is in audio_wasapi.cpp.
#include "nue_win/audio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>

#include "nue_win/power.hpp"

namespace nue::win {

const char* sample_format_name(SampleFormat f) {
    switch (f) {
        case SampleFormat::S16: return "s16";
        case SampleFormat::S24: return "s24";
        case SampleFormat::S32: return "s32";
        default: return "f32";
    }
}

namespace {

inline float read_sample(const uint8_t* p, SampleFormat f) {
    switch (f) {
        case SampleFormat::S16: { int16_t v; std::memcpy(&v, p, 2); return float(v) * (1.0f / 32768.0f); }
        case SampleFormat::S24: { int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8; return float(v) * (1.0f / 8388608.0f); }
        case SampleFormat::S32: { int32_t v; std::memcpy(&v, p, 4); return float(double(v) * (1.0 / 2147483648.0)); }
        default: { float v; std::memcpy(&v, p, 4); return v; }
    }
}

inline void write_sample(uint8_t* p, SampleFormat f, float x) {
    if (!(x == x)) x = 0.0f;   // NaN
    switch (f) {
        case SampleFormat::S16: {
            float v = std::clamp(x * 32768.0f, -32768.0f, 32767.0f);
            int16_t s = int16_t(std::lrintf(v));
            std::memcpy(p, &s, 2);
            break;
        }
        case SampleFormat::S24: {
            float v = std::clamp(x * 8388608.0f, -8388608.0f, 8388607.0f);
            int32_t s = int32_t(std::lrintf(v));
            p[0] = uint8_t(s);
            p[1] = uint8_t(s >> 8);
            p[2] = uint8_t(s >> 16);
            break;
        }
        case SampleFormat::S32: {
            double v = std::clamp(double(x) * 2147483648.0, -2147483648.0, 2147483647.0);
            int32_t s = int32_t(std::llrint(v));
            std::memcpy(p, &s, 4);
            break;
        }
        default: std::memcpy(p, &x, 4); break;
    }
}

}  // namespace

void convert_samples(const void* src, SampleFormat sf, void* dst, SampleFormat df, size_t n) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    uint8_t* d = static_cast<uint8_t*>(dst);
    if (sf == df) { std::memcpy(d, s, n * sample_bytes(sf)); return; }
    const uint32_t sb = sample_bytes(sf), db = sample_bytes(df);
    for (size_t i = 0; i < n; ++i) write_sample(d + i * db, df, read_sample(s + i * sb, sf));
}

void remap_channels(const float* src, uint16_t sc, float* dst, uint16_t dc, size_t frames) {
    if (sc == dc) { std::memcpy(dst, src, frames * sc * sizeof(float)); return; }
    constexpr float kHalfRoot2 = 0.70710678f;
    for (size_t i = 0; i < frames; ++i) {
        const float* in = src + i * sc;
        float* out = dst + i * dc;
        if (sc == 1) {
            for (uint16_t c = 0; c < dc; ++c) out[c] = (c < 2 || dc == 1) ? in[0] : 0.0f;
        } else if (dc == 1) {
            float v = 0.5f * (in[0] + in[1]);
            if (sc >= 3) v += 0.5f * kHalfRoot2 * in[2];
            out[0] = v;
        } else if (dc == 2 && sc >= 6) {
            // ITU-R BS.775 downmix, LFE dropped. Channels 4/5 are back or side, 6/7 (7.1) are side.
            float l = in[0] + kHalfRoot2 * in[2] + kHalfRoot2 * in[4];
            float r = in[1] + kHalfRoot2 * in[2] + kHalfRoot2 * in[5];
            if (sc >= 8) { l += kHalfRoot2 * in[6]; r += kHalfRoot2 * in[7]; }
            out[0] = l;
            out[1] = r;
        } else {
            const uint16_t common = std::min(sc, dc);
            for (uint16_t c = 0; c < common; ++c) out[c] = in[c];
            for (uint16_t c = common; c < dc; ++c) out[c] = 0.0f;
        }
    }
}

AudioConverter::AudioConverter(const AudioFormat& cb, const AudioFormat& dev, Pull pull)
    : cb_(cb), dev_(dev), pull_(std::move(pull)), rate_in_(cb.sample_rate), rate_out_(dev.sample_rate) {
    cb_.format = SampleFormat::F32;
    passthrough_ = dev.format == SampleFormat::F32 && cb.sample_rate == dev.sample_rate && cb.channels == dev.channels;
}

void AudioConverter::reset() {
    phase_ = 0.0;
    pending_.clear();
}

void AudioConverter::render(void* dst, uint32_t dev_frames) {
    if (dev_frames == 0) return;
    if (passthrough_) { pull_(static_cast<float*>(dst), dev_frames); return; }

    const uint16_t cch = cb_.channels, dch = dev_.channels;
    const double step = double(rate_in_) / double(rate_out_);
    const bool same_rate = rate_in_ == rate_out_;

    // Callback frames needed so that interpolation can read index floor(pos) and floor(pos)+1 for the last output frame.
    const size_t last = size_t(phase_ + double(dev_frames - 1) * step);
    // ... and enough that everything consumed by this call is really present (consumed can exceed last + 2 when downsampling).
    const size_t consumed_target = size_t(phase_ + double(dev_frames) * step);
    const size_t needed = same_rate ? dev_frames : std::max(last + 2, consumed_target);
    size_t have = pending_.size() / cch;
    if (have < needed) {
        pending_.resize(needed * cch);
        pull_(pending_.data() + have * cch, uint32_t(needed - have));
    }

    resampled_.resize(size_t(dev_frames) * cch);
    if (same_rate) {
        std::memcpy(resampled_.data(), pending_.data(), size_t(dev_frames) * cch * sizeof(float));
    } else {
        for (uint32_t i = 0; i < dev_frames; ++i) {
            const double pos = phase_ + double(i) * step;
            const size_t idx = size_t(pos);
            const float frac = float(pos - double(idx));
            const float* a = pending_.data() + idx * cch;
            const float* b = a + cch;
            for (uint16_t c = 0; c < cch; ++c) resampled_[size_t(i) * cch + c] = a[c] + (b[c] - a[c]) * frac;
        }
    }
    // Consume what the output used.
    phase_ += double(dev_frames) * step;
    const size_t consumed = same_rate ? dev_frames : size_t(phase_);
    if (!same_rate) phase_ -= double(consumed);
    pending_.erase(pending_.begin(), pending_.begin() + std::min(pending_.size(), consumed * cch));

    const float* mixed = resampled_.data();
    if (cch != dch) {
        remapped_.resize(size_t(dev_frames) * dch);
        remap_channels(resampled_.data(), cch, remapped_.data(), dch, dev_frames);
        mixed = remapped_.data();
    }
    convert_samples(mixed, SampleFormat::F32, dst, dev_.format, size_t(dev_frames) * dch);
}

// ---- Null stream ------------------------------------------------------------------------------------------------------------------------------

namespace {

class NullStream final : public AudioStream {
public:
    explicit NullStream(const AudioStreamConfig& cfg) : cfg_(cfg) {
        cb_format_ = cfg.format;
        cb_format_.format = SampleFormat::F32;
        dev_format_ = cfg.null_device_format.sample_rate ? cfg.null_device_format : cb_format_;
        period_frames_ = cfg.period_frames ? cfg.period_frames : std::max<uint32_t>(1, dev_format_.sample_rate / 100);   // 10 ms
        volume_ = cfg.session_volume;
        converter_.reset(new AudioConverter(cb_format_, dev_format_, [this](float* out, uint32_t frames) { pull(out, frames); }));
    }
    ~NullStream() override { stop(); }

    Status start() override {
        if (running_) return Status::success();
        stop_flag_ = false;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return Status::success();
    }
    void stop() override {
        if (!running_ && !thread_.joinable()) return;
        stop_flag_ = true;
        if (thread_.joinable()) thread_.join();
        running_ = false;
    }
    AudioStreamStats stats() const override {
        std::lock_guard<std::mutex> g(mutex_);
        AudioStreamStats s = stats_;
        s.running = running_;
        return s;
    }
    void set_volume(float v) override { volume_ = std::clamp(v, 0.0f, 1.0f); }

private:
    void pull(float* out, uint32_t frames) {
        if (cfg_.callback) cfg_.callback(out, frames, cb_format_);
        else std::memset(out, 0, size_t(frames) * cb_format_.channels * sizeof(float));
        const float v = volume_;
        if (v != 1.0f) for (size_t i = 0, n = size_t(frames) * cb_format_.channels; i < n; ++i) out[i] *= v;
    }

    void run() {
        MmcssScope mmcss;
        if (cfg_.mmcss) mmcss.begin(L"Pro Audio");
        PreciseSleeper sleeper;
        std::vector<uint8_t> device_buffer(size_t(period_frames_) * dev_format_.frame_bytes());
        const int64_t period_qpc = qpc_from_ns(int64_t(1e9 * double(period_frames_) / double(dev_format_.sample_rate)));
        int64_t next = qpc_now() + period_qpc;
        double callback_sum = 0;
        uint64_t ticks = 0;
        {
            std::lock_guard<std::mutex> g(mutex_);
            stats_ = AudioStreamStats{};
            stats_.device_format = dev_format_;
            stats_.period_frames = stats_.buffer_frames = period_frames_;
            stats_.period_ms = stats_.buffer_ms = 1000.0 * period_frames_ / dev_format_.sample_rate;
            stats_.our_conversion = !converter_->passthrough();
            stats_.device_name = "null";
        }
        while (!stop_flag_) {
            sleeper.sleep_until_qpc(next);
            const int64_t t0 = qpc_now();
            converter_->render(device_buffer.data(), period_frames_);
            if (cfg_.null_sink) cfg_.null_sink(device_buffer.data(), period_frames_, dev_format_);
            const double ms = qpc_to_ms(qpc_now() - t0);
            callback_sum += ms;
            ++ticks;
            {
                std::lock_guard<std::mutex> g(mutex_);
                stats_.frames_rendered += period_frames_;
                stats_.max_callback_ms = std::max(stats_.max_callback_ms, ms);
                stats_.avg_callback_ms = callback_sum / double(ticks);
                if (ms > stats_.period_ms) ++stats_.callback_overruns;
            }
            next += period_qpc;
            const int64_t now = qpc_now();
            if (now > next + period_qpc * 4) { std::lock_guard<std::mutex> g(mutex_); ++stats_.glitches; next = now + period_qpc; }   // we fell far behind: resynchronise
        }
    }

    AudioStreamConfig cfg_;
    AudioFormat cb_format_, dev_format_;
    uint32_t period_frames_ = 0;
    std::unique_ptr<AudioConverter> converter_;
    std::thread thread_;
    std::atomic<bool> stop_flag_{false};
    bool running_ = false;
    std::atomic<float> volume_{1.0f};
    mutable std::mutex mutex_;
    AudioStreamStats stats_;
};

}  // namespace

std::unique_ptr<AudioStream> create_null_stream(const AudioStreamConfig& cfg, Status* status) {
    if (cfg.format.channels == 0 || cfg.format.sample_rate == 0) {
        if (status) *status = Status::fail("invalid audio format", ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    if (status) *status = Status::success();
    return std::unique_ptr<AudioStream>(new NullStream(cfg));
}

}  // namespace nue::win
