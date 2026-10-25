// Audio tests. The default run never opens a real audio device and never makes sound: conversion maths, the converter, the Null stream
// (a tone rendered through the complete pipeline into memory), device enumeration (read only).
// Opt-in, on a machine where a short silent stream is acceptable:
//   NUE_TEST_WASAPI=1            opens the default device in shared mode with the session volume at 0 (silent) and checks cadence/statistics
//   NUE_TEST_WASAPI_EXCLUSIVE=1  opens the default device exclusively for 0.3 s with silent data (interrupts other apps' audio briefly)
//   NUE_TEST_WASAPI_LOOPBACK=1   additionally renders a -90 dBFS tone and verifies it through WASAPI loopback capture (inaudible)
#include <audioclient.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "nue_win/audio.hpp"
#include "nue_win/power.hpp"
#include "test_framework.hpp"

using namespace nue::win;

constexpr double kPi = 3.14159265358979323846;

// Single-bin DFT magnitude (Goertzel), amplitude-normalised so that a sine of amplitude A at `freq` returns about A.
static double tone_amplitude(const float* samples, size_t frames, size_t stride, size_t channel, double rate, double freq) {
    double re = 0, im = 0;
    for (size_t i = 0; i < frames; ++i) {
        const double ph = 2.0 * kPi * freq * double(i) / rate;
        const double v = samples[i * stride + channel];
        re += v * std::cos(ph);
        im -= v * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / double(frames);
}

struct ToneGenerator {
    double freq, amplitude, rate;
    uint64_t pos = 0;
    void fill(float* out, uint32_t frames, uint16_t channels) {
        for (uint32_t i = 0; i < frames; ++i, ++pos) {
            const float v = float(amplitude * std::sin(2.0 * kPi * freq * double(pos) / rate));
            for (uint16_t c = 0; c < channels; ++c) out[size_t(i) * channels + c] = v;
        }
    }
};

static void test_sample_conversion() {
    TEST_BEGIN("sample conversion");
    const float in[] = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 0.25f};
    int16_t s16[8];
    convert_samples(in, SampleFormat::F32, s16, SampleFormat::S16, 8);
    CHECK(s16[0] == 0 && s16[1] == 16384 && s16[2] == -16384 && s16[3] == 32767 && s16[4] == -32768 && s16[5] == 32767 && s16[6] == -32768);   // clamps, no wraparound
    uint8_t s24[24];
    convert_samples(in, SampleFormat::F32, s24, SampleFormat::S24, 8);
    CHECK(s24[3] == 0x00 && s24[4] == 0x00 && s24[5] == 0x40);   // 0.5 -> 0x400000 little endian
    CHECK(s24[9] == 0xFF && s24[10] == 0xFF && s24[11] == 0x7F);   // +1.0 clamps to 0x7FFFFF
    int32_t s32[8];
    convert_samples(in, SampleFormat::F32, s32, SampleFormat::S32, 8);
    CHECK(s32[1] == 0x40000000 && s32[3] == 0x7FFFFFFF && s32[4] == INT32_MIN);
    // round trips
    float back[8];
    convert_samples(s16, SampleFormat::S16, back, SampleFormat::F32, 8);
    CHECK(std::fabs(back[1] - 0.5f) < 1e-4f && back[4] == -1.0f);
    convert_samples(s24, SampleFormat::S24, back, SampleFormat::F32, 8);
    CHECK(std::fabs(back[1] - 0.5f) < 1e-6f && std::fabs(back[2] + 0.5f) < 1e-6f);
    convert_samples(s32, SampleFormat::S32, back, SampleFormat::F32, 8);
    CHECK(std::fabs(back[1] - 0.5f) < 1e-7f);
    // integer -> integer goes through float without surprises
    int32_t wide[4];
    const int16_t narrow[4] = {0, 1000, -1000, 32767};
    convert_samples(narrow, SampleFormat::S16, wide, SampleFormat::S32, 4);
    CHECK(wide[1] == 1000 * 65536 && wide[2] == -1000 * 65536);
    const float nan = std::nanf("");
    convert_samples(&nan, SampleFormat::F32, s16, SampleFormat::S16, 1);
    CHECK(s16[0] == 0);   // NaN from a buggy callback becomes silence, never noise
}

static void test_channels() {
    TEST_BEGIN("channel remapping");
    float out[8];
    const float mono[] = {0.5f};
    remap_channels(mono, 1, out, 2, 1);
    CHECK(out[0] == 0.5f && out[1] == 0.5f);
    remap_channels(mono, 1, out, 6, 1);
    CHECK(out[0] == 0.5f && out[1] == 0.5f && out[2] == 0 && out[5] == 0);
    const float st[] = {1.0f, 0.0f};
    remap_channels(st, 2, out, 1, 1);
    CHECK(std::fabs(out[0] - 0.5f) < 1e-6f);
    remap_channels(st, 2, out, 8, 1);
    CHECK(out[0] == 1.0f && out[1] == 0 && out[7] == 0);
    const float surround[] = {0.1f, 0.2f, 0.4f, 0.9f, 0.3f, 0.5f};   // FL FR FC LFE BL BR
    remap_channels(surround, 6, out, 2, 1);
    CHECK(std::fabs(out[0] - (0.1f + 0.70710678f * 0.4f + 0.70710678f * 0.3f)) < 1e-6f);
    CHECK(std::fabs(out[1] - (0.2f + 0.70710678f * 0.4f + 0.70710678f * 0.5f)) < 1e-6f);   // the LFE (0.9) does not leak into the stereo mix
    const float s71[] = {0.1f, 0.2f, 0.0f, 0.0f, 0.0f, 0.0f, 0.4f, 0.8f};
    remap_channels(s71, 8, out, 2, 1);
    CHECK(std::fabs(out[0] - (0.1f + 0.70710678f * 0.4f)) < 1e-6f && std::fabs(out[1] - (0.2f + 0.70710678f * 0.8f)) < 1e-6f);
    float four[4];
    const float q[] = {1, 2, 3, 4};
    remap_channels(q, 4, out, 2, 1);
    CHECK(out[0] == 1 && out[1] == 2);
    remap_channels(st, 2, four, 4, 1);
    CHECK(four[0] == 1 && four[2] == 0 && four[3] == 0);
}

static std::vector<float> render_in_chunks(const AudioFormat& cb, const AudioFormat& dev, double freq, const std::vector<uint32_t>& chunks, size_t total_frames) {
    ToneGenerator tone{freq, 0.5, double(cb.sample_rate)};
    AudioConverter conv(cb, dev, [&](float* out, uint32_t frames) { tone.fill(out, frames, cb.channels); });
    std::vector<float> all;
    size_t done = 0, i = 0;
    while (done < total_frames) {
        uint32_t n = std::min<size_t>(chunks[i++ % chunks.size()], total_frames - done);
        std::vector<float> buf(size_t(n) * dev.channels);
        conv.render(buf.data(), n);
        all.insert(all.end(), buf.begin(), buf.end());
        done += n;
    }
    return all;
}

static void test_converter() {
    TEST_BEGIN("AudioConverter");
    const AudioFormat cb44{44100, 2, SampleFormat::F32};
    const AudioFormat dev48{48000, 2, SampleFormat::F32};
    CHECK(!AudioConverter(cb44, dev48, [](float*, uint32_t) {}).passthrough());
    CHECK(AudioConverter(dev48, dev48, [](float*, uint32_t) {}).passthrough());
    CHECK(!AudioConverter(dev48, {48000, 2, SampleFormat::S16}, [](float*, uint32_t) {}).passthrough());

    // Chunking must not change the result: rendering in awkward chunk sizes equals rendering at once, and the tone keeps its frequency and level.
    const size_t total = 48000;
    const auto whole = render_in_chunks(cb44, dev48, 1000.0, {uint32_t(total)}, total);
    const auto chunked = render_in_chunks(cb44, dev48, 1000.0, {7, 480, 1, 333, 2048, 13}, total);
    CHECK(whole.size() == chunked.size());
    double worst = 0;
    for (size_t i = 0; i < whole.size(); ++i) worst = std::max(worst, double(std::fabs(whole[i] - chunked[i])));
    CHECK_MSG(worst < 1e-6, "chunking changed the output by %g", worst);
    const double a1000 = tone_amplitude(whole.data(), total, 2, 0, 48000.0, 1000.0);
    const double a1500 = tone_amplitude(whole.data(), total, 2, 0, 48000.0, 1500.0);
    INFO("44.1k -> 48k: 1 kHz amplitude %.4f (expect 0.5), 1.5 kHz leakage %.5f", a1000, a1500);
    CHECK(std::fabs(a1000 - 0.5) < 0.02 && a1500 < 0.01);

    // downsample 48k -> 22.05k mono
    const AudioFormat cb48{48000, 2, SampleFormat::F32}, dev22{22050, 1, SampleFormat::F32};
    const auto down = render_in_chunks(cb48, dev22, 440.0, {441}, 22050);
    const double a440 = tone_amplitude(down.data(), 22050, 1, 0, 22050.0, 440.0);
    INFO("48k stereo -> 22.05k mono: 440 Hz amplitude %.4f", a440);
    CHECK(std::fabs(a440 - 0.5) < 0.03);

    // 6 channels from a stereo callback and S16 output
    const AudioFormat dev6{48000, 6, SampleFormat::S16};
    ToneGenerator tone{440.0, 0.5, 48000.0};
    AudioConverter c6(cb48, dev6, [&](float* out, uint32_t n) { tone.fill(out, n, 2); });
    std::vector<int16_t> pcm(480 * 6);
    c6.render(pcm.data(), 480);
    bool centre_silent = true, front_active = false;
    for (int i = 0; i < 480; ++i) { centre_silent &= pcm[i * 6 + 2] == 0 && pcm[i * 6 + 3] == 0; front_active |= std::abs(pcm[i * 6]) > 1000; }
    CHECK(centre_silent && front_active);

    // reset drops history
    ToneGenerator t2{1000.0, 0.5, 44100.0};
    AudioConverter r(cb44, dev48, [&](float* out, uint32_t n) { t2.fill(out, n, 2); });
    std::vector<float> a(480 * 2);
    r.render(a.data(), 480);
    r.reset();
    r.render(a.data(), 480);   // must not crash or read stale data
    CHECK(true);
}

static void test_null_stream() {
    TEST_BEGIN("Null stream: a tone through the whole pipeline, in memory");
    struct Case { AudioFormat cb; AudioFormat dev; double freq; };
    const Case cases[] = {
        {{48000, 2, SampleFormat::F32}, {48000, 2, SampleFormat::F32}, 997.0},
        {{44100, 2, SampleFormat::F32}, {48000, 2, SampleFormat::S16}, 1234.0},
        {{48000, 2, SampleFormat::F32}, {44100, 1, SampleFormat::S32}, 440.0},
    };
    for (const Case& c : cases) {
        std::mutex m;
        std::vector<float> captured;   // device data converted back to float, first channel only
        ToneGenerator tone{c.freq, 0.4, double(c.cb.sample_rate)};
        AudioStreamConfig cfg;
        cfg.format = c.cb;
        cfg.period_frames = c.cb.sample_rate / 100;
        cfg.null_device_format = c.dev;
        cfg.mmcss = false;
        cfg.callback = [&](float* out, uint32_t frames, const AudioFormat& f) { tone.fill(out, frames, f.channels); };
        cfg.null_sink = [&](const void* data, uint32_t frames, const AudioFormat& f) {
            std::vector<float> tmp(size_t(frames) * f.channels);
            convert_samples(data, f.format, tmp.data(), SampleFormat::F32, tmp.size());
            std::lock_guard<std::mutex> g(m);
            for (uint32_t i = 0; i < frames; ++i) captured.push_back(tmp[size_t(i) * f.channels]);
        };
        Status st;
        auto stream = create_null_stream(cfg, &st);
        CHECK_OK(st);
        CHECK(stream != nullptr);
        CHECK_OK(stream->start());
        std::this_thread::sleep_for(std::chrono::milliseconds(520));
        const AudioStreamStats s = stream->stats();
        stream->stop();
        CHECK(!stream->stats().running);
        INFO("%u Hz %uch %s -> %u Hz %uch %s: %llu frames in ~520 ms, callback max %.3f ms, overruns %u, glitches %u, our conversion %d", c.cb.sample_rate, c.cb.channels,
             sample_format_name(c.cb.format), c.dev.sample_rate, c.dev.channels, sample_format_name(c.dev.format), (unsigned long long)s.frames_rendered, s.max_callback_ms, s.callback_overruns,
             s.glitches, int(s.our_conversion));
        const double expected = 0.52 * c.dev.sample_rate;
        CHECK(s.frames_rendered > expected * 0.85 && s.frames_rendered < expected * 1.15);   // paced in real time, not free running
        CHECK(s.callback_overruns == 0);
        std::lock_guard<std::mutex> g(m);
        CHECK(captured.size() >= c.dev.sample_rate / 4);
        const size_t n = std::min<size_t>(captured.size(), c.dev.sample_rate / 4);
        const double a = tone_amplitude(captured.data(), n, 1, 0, double(c.dev.sample_rate), c.freq);
        CHECK_MSG(std::fabs(a - 0.4) < 0.03, "tone amplitude %.4f at %.0f Hz", a, c.freq);
    }
}

static void test_devices() {
    TEST_BEGIN("audio devices (read only)");
    for (AudioFlow flow : {AudioFlow::Render, AudioFlow::Capture}) {
        std::vector<AudioDeviceInfo> devs;
        Status s = enumerate_audio_devices(flow, devs);
        CHECK_OK(s);
        INFO("%s devices: %zu", flow == AudioFlow::Render ? "render" : "capture", devs.size());
        int defaults = 0;
        for (const auto& d : devs) {
            defaults += d.is_default;
            INFO("  %s%s: mix %u Hz %uch %s, exclusive periods %.2f..%.2f ms, shared %.2f..%.2f ms (low-latency path: %s)", d.name.c_str(), d.is_default ? " [default]" : "", d.mix_format.sample_rate,
                 d.mix_format.channels, sample_format_name(d.mix_format.format), d.minimum_period_ms, d.default_period_ms, d.shared_minimum_period_ms, d.shared_default_period_ms,
                 d.supports_low_latency_shared ? "yes" : "no");
            CHECK(!d.id.empty());
            CHECK(d.mix_format.sample_rate > 0 && d.mix_format.channels > 0);
        }
        CHECK(devs.empty() || defaults <= 1);
        if (!devs.empty()) { AudioDeviceInfo def; CHECK(default_audio_device(flow, def) || defaults == 0); }
    }
}

// ---- opt-in live tests ---------------------------------------------------------------------------------------------------------------------------

// Loopback capture of the default render endpoint (what the mixer sends to the speakers, before the endpoint volume).
class LoopbackCapture {
public:
    // Read-only: the endpoint (master) volume of the default device, to explain the level seen by the loopback tap.
    static float endpoint_volume() {
        float v = -1;
        IMMDeviceEnumerator* e = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&e)))) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eConsole, &d))) {
                IAudioEndpointVolume* ev = nullptr;
                if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ev)))) { ev->GetMasterVolumeLevelScalar(&v); ev->Release(); }
                d->Release();
            }
            e->Release();
        }
        return v;
    }
    bool start() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
        IMMDeviceEnumerator* e = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&e)))) return false;
        IMMDevice* d = nullptr;
        HRESULT hr = e->GetDefaultAudioEndpoint(eRender, eConsole, &d);
        e->Release();
        if (FAILED(hr)) return false;
        hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
        d->Release();
        if (FAILED(hr) || FAILED(client_->GetMixFormat(&mix_))) return false;
        if (mix_->wFormatTag != WAVE_FORMAT_EXTENSIBLE && mix_->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) return false;   // the shared mix is float on every supported Windows
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, mix_, nullptr))) return false;
        if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_)))) return false;
        return SUCCEEDED(client_->Start());
    }
    void poll() {
        UINT32 packet = 0;
        while (SUCCEEDED(capture_->GetNextPacketSize(&packet)) && packet) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            const size_t n = size_t(frames) * mix_->nChannels;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) samples_.insert(samples_.end(), n, 0.0f);
            else samples_.insert(samples_.end(), reinterpret_cast<float*>(data), reinterpret_cast<float*>(data) + n);
            capture_->ReleaseBuffer(frames);
        }
    }
    void stop() {
        if (client_) client_->Stop();
        if (capture_) capture_->Release();
        if (client_) client_->Release();
        if (mix_) CoTaskMemFree(mix_);
        capture_ = nullptr; client_ = nullptr; mix_ = nullptr;
    }
    std::vector<float> samples_;
    WAVEFORMATEX* mix_ = nullptr;
private:
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
};

static void test_wasapi_live(bool loopback) {
    TEST_BEGIN(loopback ? "WASAPI live + loopback verification (-90 dBFS tone)" : "WASAPI live, silent session");
    AudioDeviceInfo def;
    if (!default_audio_device(AudioFlow::Render, def)) { SKIP("no default render device"); return; }

    LoopbackCapture cap;
    double ambient = 0;
    if (loopback) {
        if (!cap.start()) { SKIP("loopback capture unavailable"); return; }
        for (int i = 0; i < 40; ++i) { Sleep(5); cap.poll(); }
        double sum = 0;
        for (float v : cap.samples_) sum += double(v) * v;
        ambient = cap.samples_.empty() ? 0 : std::sqrt(sum / double(cap.samples_.size()));
        INFO("ambient loopback level before our stream: %.2e RMS", ambient);
        if (ambient > 1e-4) { SKIP("other audio is playing on the default device"); cap.stop(); return; }
        cap.samples_.clear();
    }

    const double amplitude = loopback ? 3.2e-5 : 0.25;   // -90 dBFS is below the 16-bit LSB of any DAC: inaudible; the silent run uses session volume 0 instead
    ToneGenerator tone{997.0, amplitude, double(def.mix_format.sample_rate)};
    std::atomic<uint64_t> callbacks{0};
    AudioStreamConfig cfg;
    cfg.format = {def.mix_format.sample_rate, def.mix_format.channels >= 2 ? uint16_t(2) : def.mix_format.channels, SampleFormat::F32};
    cfg.session_volume = loopback ? 1.0f : 0.0f;
    cfg.callback = [&](float* out, uint32_t frames, const AudioFormat& f) { tone.fill(out, frames, f.channels); ++callbacks; };
    Status st;
    auto stream = create_wasapi_stream(cfg, &st);
    CHECK_OK(st);
    const Status start = stream->start();
    CHECK_OK(start);
    if (!start.ok) { cap.stop(); return; }
    const int64_t t_start = qpc_now();
    for (int i = 0; i < 60; ++i) { Sleep(10); if (loopback) cap.poll(); }
    const AudioStreamStats s = stream->stats();
    const double elapsed_s = qpc_to_ms(qpc_now() - t_start) / 1000.0;   // Sleep(10) lasts ~15.6 ms at the default timer resolution
    INFO("device \"%s\": %u Hz %uch %s, %s%s%s, period %u frames (%.2f ms), buffer %u (%.2f ms), stream latency %.2f ms", s.device_name.c_str(), s.device_format.sample_rate, s.device_format.channels,
         sample_format_name(s.device_format.format), s.exclusive ? "exclusive" : "shared", s.low_latency_path ? ", IAudioClient3 low latency" : "", s.engine_conversion ? ", engine conversion" : "",
         s.period_frames, s.period_ms, s.buffer_frames, s.buffer_ms, s.stream_latency_ms);
    INFO("elapsed %.3f s, callbacks %llu, frames %llu, glitches %u, restarts %u, callback avg %.3f ms max %.3f ms", elapsed_s, (unsigned long long)callbacks.load(), (unsigned long long)s.frames_rendered, s.glitches, s.restarts,
         s.avg_callback_ms, s.max_callback_ms);
    CHECK(s.running);
    CHECK(callbacks.load() >= 20);
    const double expected = elapsed_s * s.device_format.sample_rate;
    CHECK(double(s.frames_rendered) > expected * 0.9 - s.buffer_frames && double(s.frames_rendered) < expected * 1.1 + s.buffer_frames);
    CHECK(s.glitches <= 2);
    if (loopback) {
        for (int i = 0; i < 10; ++i) { Sleep(10); cap.poll(); }
        stream->stop();
        cap.poll();
        const uint16_t ch = cap.mix_->nChannels;
        const size_t frames = cap.samples_.size() / ch;
        CHECK(frames > cap.mix_->nSamplesPerSec / 4);
        // The capture started before our stream and kept running after it: take the loudest 100 ms block (the tone fully inside it).
        const size_t block = cap.mix_->nSamplesPerSec / 10;
        double got = 0, other = 0;
        for (size_t at = 0; at + block <= frames; at += block / 2) {
            const double g = tone_amplitude(cap.samples_.data() + at * ch, block, ch, 0, double(cap.mix_->nSamplesPerSec), 997.0);
            if (nuetest::env_set("NUE_TEST_VERBOSE")) INFO("  block at %.2f s: 997 Hz %.3e", double(at) / cap.mix_->nSamplesPerSec, g);
            if (g > got) { got = g; other = tone_amplitude(cap.samples_.data() + at * ch, block, ch, 0, double(cap.mix_->nSamplesPerSec), 1700.0); }
        }
        INFO("endpoint master volume (scalar, read only): %.3f", LoopbackCapture::endpoint_volume());
        INFO("loopback: 997 Hz amplitude %.3e (rendered %.3e), 1700 Hz %.3e", got, amplitude, other);
        // Some drivers/Windows builds tap the loopback after the endpoint volume (seen here at endpoint volume 0.12 -> about 0.4x), so the absolute level is
        // not asserted: the tone must be there, at the right frequency, not louder than what we rendered, and well above the neighbouring bin.
        CHECK(got > amplitude * 0.02 && got < amplitude * 1.2);
        CHECK(got > other * 15);
        cap.stop();
    } else {
        stream->stop();
    }
    CHECK(!stream->stats().running);
}

static void test_wasapi_exclusive() {
    TEST_BEGIN("WASAPI exclusive mode with silent data");
    AudioDeviceInfo def;
    if (!default_audio_device(AudioFlow::Render, def)) { SKIP("no default render device"); return; }
    std::atomic<uint64_t> callbacks{0};
    AudioStreamConfig cfg;
    cfg.format = {def.mix_format.sample_rate, 2, SampleFormat::F32};
    cfg.share = ShareMode::Exclusive;
    cfg.callback = [&](float* out, uint32_t frames, const AudioFormat& f) { std::memset(out, 0, size_t(frames) * f.channels * sizeof(float)); ++callbacks; };   // silence: the device is taken for a moment, nothing is played
    Status st;
    auto stream = create_wasapi_stream(cfg, &st);
    CHECK_OK(st);
    const Status start = stream->start();
    if (!start.ok) { SKIP("exclusive mode refused: %s (0x%08X) -- allowed to apps? device in use?", start.what, start.code); return; }
    const int64_t t0 = qpc_now();
    Sleep(300);
    const AudioStreamStats s = stream->stats();
    const double elapsed = qpc_to_ms(qpc_now() - t0) / 1000.0;
    stream->stop();
    INFO("exclusive: %u Hz %uch %s, period %u frames (%.2f ms), buffer %u frames, callbacks %llu in %.3f s, glitches %u, engine conversion %d, our conversion %d", s.device_format.sample_rate,
         s.device_format.channels, sample_format_name(s.device_format.format), s.period_frames, s.period_ms, s.buffer_frames, (unsigned long long)callbacks.load(), elapsed, s.glitches,
         int(s.engine_conversion), int(s.our_conversion));
    CHECK(s.exclusive);
    CHECK(callbacks.load() > 5);
    CHECK(double(s.frames_rendered) > elapsed * s.device_format.sample_rate * 0.5);
}

int main() {
    test_sample_conversion();
    test_channels();
    test_converter();
    test_null_stream();
    test_devices();
    if (nuetest::env_set("NUE_TEST_WASAPI") || nuetest::env_set("NUE_TEST_WASAPI_LOOPBACK") || nuetest::env_set("NUE_TEST_WASAPI_EXCLUSIVE")) {
        if (nuetest::env_set("NUE_TEST_WASAPI")) test_wasapi_live(false);
        if (nuetest::env_set("NUE_TEST_WASAPI_LOOPBACK")) test_wasapi_live(true);
        if (nuetest::env_set("NUE_TEST_WASAPI_EXCLUSIVE")) test_wasapi_exclusive();
    } else {
        INFO("(live WASAPI tests skipped: set NUE_TEST_WASAPI=1 / NUE_TEST_WASAPI_LOOPBACK=1)");
    }
    return finish("audio_tests");
}
