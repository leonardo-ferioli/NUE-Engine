// Shared by the DirectSound layer tests: check macros, a deterministic output sink and signal helpers. Headless: no device, no window.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nue_legacy/dsound_engine.hpp"

static int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                      \
    do {                                                                                                 \
        ++g_checks;                                                                                      \
        if (!(cond)) {                                                                                   \
            ++g_failures;                                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                  \
        }                                                                                                \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                            \
    do {                                                                                                 \
        ++g_checks;                                                                                      \
        const double va = double(a), vb = double(b);                                                     \
        if (!(std::fabs(va - vb) <= double(tol))) {                                                      \
            ++g_failures;                                                                                \
            std::printf("FAIL %s:%d  %s = %.6f, expected %.6f +- %g\n", __FILE__, __LINE__, #a, va, vb, double(tol)); \
        }                                                                                                \
    } while (0)
#define CHECK_HR(expr, expected)                                                                                              \
    do {                                                                                                                      \
        ++g_checks;                                                                                                           \
        const uint32_t got_ = uint32_t(expr);                                                                                 \
        if (got_ != uint32_t(expected)) {                                                                                     \
            ++g_failures;                                                                                                     \
            std::printf("FAIL %s:%d  %s = 0x%08x, expected 0x%08x\n", __FILE__, __LINE__, #expr, got_, uint32_t(expected));   \
        }                                                                                                                     \
    } while (0)

namespace nue::dsound::testing {

// A deterministic sink: no device and no thread. The test drives time with advance(frames); every block the callback produced is hashed
// (FNV-1a over the samples quantised to 16 bits, so the hash does not depend on float formatting).
class ManualSink final : public OutputSink {
public:
    explicit ManualSink(uint32_t block_frames = 256, uint32_t simulated_latency_frames = 0) : block_(block_frames), latency_frames_(simulated_latency_frames) {}
    bool open(const SinkDesc& desc, SinkCallback callback, void* user) override {
        if (!callback || desc.spec.channels == 0 || desc.spec.channels > 8) return false;
        spec_ = desc.spec;
        if (spec_.sample_rate == 0) spec_.sample_rate = 48000;
        callback_ = callback;
        user_ = user;
        scratch_.assign(size_t(block_) * spec_.channels, 0.0f);
        played_ = 0;
        reset_hash();
        return true;
    }
    OutputLayout actual() const override { return spec_; }
    bool start() override { running_ = true; return true; }
    bool stop() override { running_ = false; return true; }
    void close() override { running_ = false; callback_ = nullptr; }
    bool running() const override { return running_; }
    uint64_t frames_played() const override { return played_ > latency_frames_ ? played_ - latency_frames_ : 0; }
    SinkLatency latency() const override {
        SinkLatency l;
        l.device_buffer_frames = block_;
        l.device_latency_ns = spec_.sample_rate ? uint64_t(latency_frames_) * 1000000000ull / spec_.sample_rate : 0;
        l.total_latency_ns = l.device_latency_ns;
        return l;
    }
    SinkStats stats() const override { return {played_, 0, peak_, 0}; }

    // Runs the callback for `frames` frames in blocks of `block_frames` (the last block may be shorter). No-op unless started.
    void advance(uint32_t frames) {
        if (!running_ || !callback_) return;
        while (frames) {
            const uint32_t n = std::min(frames, block_);
            std::fill(scratch_.begin(), scratch_.begin() + size_t(n) * spec_.channels, 0.0f);
            callback_(user_, scratch_.data(), n);
            const size_t count = size_t(n) * spec_.channels;
            float peak = 0;
            for (size_t i = 0; i < count; ++i) {
                const float v = scratch_[i];
                peak = std::max(peak, std::fabs(v));
                const float clamped = std::clamp(v, -1.0f, 1.0f);
                const auto q = uint16_t(int16_t(std::lrintf(clamped * 32767.0f)));
                hash_ = (hash_ ^ (q & 0xffu)) * 0x100000001b3ull;
                hash_ = (hash_ ^ (q >> 8)) * 0x100000001b3ull;
            }
            peak_ = peak;
            if (keep_) samples_.insert(samples_.end(), scratch_.begin(), scratch_.begin() + std::ptrdiff_t(count));
            played_ += n;
            frames -= n;
        }
    }
    uint64_t hash() const { return hash_; }
    uint64_t frames_rendered() const { return played_; }
    void keep_samples(bool keep) { keep_ = keep; }
    const std::vector<float>& samples() const { return samples_; }
    void reset_hash() { hash_ = 0xcbf29ce484222325ull; }

private:
    uint32_t block_, latency_frames_;
    OutputLayout spec_;
    SinkCallback callback_ = nullptr;
    void* user_ = nullptr;
    bool running_ = false, keep_ = false;
    uint64_t played_ = 0, hash_ = 0xcbf29ce484222325ull;
    float peak_ = 0;
    std::vector<float> scratch_, samples_;
};

inline WaveFormat pcm16(uint32_t rate, uint16_t channels) {
    WaveFormat f;
    f.channels = channels;
    f.sample_rate = rate;
    f.bits = 16;
    return f;
}

inline std::vector<uint8_t> s16_bytes(const std::vector<int16_t>& v) {
    std::vector<uint8_t> b(v.size() * 2);
    std::memcpy(b.data(), v.data(), b.size());
    return b;
}

// Interleaved 16-bit sine, `frames` frames, `channels` channels, same signal in every channel.
inline std::vector<uint8_t> sine16(uint32_t frames, uint16_t channels, double freq, double rate, double amp = 0.5) {
    std::vector<int16_t> v(size_t(frames) * channels);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t c = 0; c < channels; ++c) v[size_t(i) * channels + c] = int16_t(std::lrint(amp * 32767.0 * std::sin(2.0 * 3.14159265358979 * freq * i / rate)));
    return s16_bytes(v);
}

inline int zero_crossings(const std::vector<float>& x, size_t stride, size_t channel) {
    int n = 0;
    for (size_t i = stride + channel; i < x.size(); i += stride)
        if ((x[i - stride] < 0) != (x[i] < 0)) ++n;
    return n;
}

inline double rms(const std::vector<float>& x, size_t stride, size_t channel, size_t from = 0, size_t to = ~size_t(0)) {
    double s = 0;
    size_t n = 0;
    for (size_t i = from * stride + channel; i < x.size() && i < to * stride; i += stride, ++n) s += double(x[i]) * x[i];
    return n ? std::sqrt(s / double(n)) : 0.0;
}

inline Bytes bytes_of(const std::vector<uint8_t>& v, size_t offset, size_t n) { return Bytes(reinterpret_cast<const std::byte*>(v.data()) + offset, n); }

}  // namespace nue::dsound::testing
