// Windows audio output: WASAPI (shared and exclusive, event driven, low-latency shared path via IAudioClient3), device enumeration,
// default-device change and hot-plug handling, format conversion (sample type, channel layout, sample rate) done by us when the device
// does not accept the game's format, plus a Null stream with the same pipeline for tests and headless runs.
//
// The callback always produces interleaved 32-bit float in the format the game asked for; everything between that and the device is this
// module's job. The core audio API is a thin layer over AudioStream.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

enum class SampleFormat : uint8_t { S16, S24, S32, F32 };   // S24 = packed 3 bytes per sample
inline uint32_t sample_bytes(SampleFormat f) { return f == SampleFormat::S16 ? 2u : (f == SampleFormat::S24 ? 3u : 4u); }
const char* sample_format_name(SampleFormat f);

struct AudioFormat {
    uint32_t sample_rate = 48000;
    uint16_t channels = 2;
    SampleFormat format = SampleFormat::F32;
    uint32_t frame_bytes() const { return sample_bytes(format) * channels; }
    bool operator==(const AudioFormat& o) const { return sample_rate == o.sample_rate && channels == o.channels && format == o.format; }
    bool operator!=(const AudioFormat& o) const { return !(*this == o); }
};

// ---- Conversion (pure, no OS) ---------------------------------------------------------------------------------------------------------------
// Interleaved sample type conversion with the same channel count.
void convert_samples(const void* src, SampleFormat sf, void* dst, SampleFormat df, size_t sample_count);
// Channel layout conversion on interleaved float. Layouts follow the WAVEFORMATEXTENSIBLE order: FL FR FC LFE BL BR SL SR.
// mono -> N: copied to FL and FR; N -> mono: average of FL, FR (+ centre at -3 dB); 5.1 / 7.1 -> stereo: ITU downmix without LFE;
// anything else: common channels copied, missing ones silent.
void remap_channels(const float* src, uint16_t src_channels, float* dst, uint16_t dst_channels, size_t frames);

// Pull-style converter: the device asks for `dev_frames` in the device format; the converter pulls float frames from `pull` in the
// callback format, resamples (linear, stateful, no clicks across calls), remaps channels and converts the sample type.
class AudioConverter {
public:
    using Pull = std::function<void(float* interleaved, uint32_t frames)>;
    AudioConverter(const AudioFormat& callback_format, const AudioFormat& device_format, Pull pull);
    void render(void* dst, uint32_t dev_frames);
    void reset();   // after a device change: drop the resampler history
    bool passthrough() const { return passthrough_; }      // float, same rate and channels: pull straight into the device buffer
    bool resampling() const { return rate_in_ != rate_out_; }
    const AudioFormat& callback_format() const { return cb_; }
    const AudioFormat& device_format() const { return dev_; }
private:
    AudioFormat cb_, dev_;
    Pull pull_;
    uint32_t rate_in_, rate_out_;
    bool passthrough_ = false;
    double phase_ = 0.0;                 // position inside `pending_`, in callback frames
    std::vector<float> pending_;         // callback frames not yet consumed (interleaved)
    std::vector<float> resampled_;       // device-rate frames in callback channels
    std::vector<float> remapped_;        // device-rate frames in device channels (float)
};

// ---- Devices -----------------------------------------------------------------------------------------------------------------------------------
enum class AudioFlow : uint8_t { Render, Capture };
struct AudioDeviceInfo {
    std::string id;              // endpoint id (UTF-8); pass it to AudioStreamConfig::device_id
    std::string name;            // friendly name
    bool is_default = false;     // default for the console role
    bool is_default_communications = false;
    AudioFlow flow = AudioFlow::Render;
    AudioFormat mix_format;      // the shared-mode engine format
    uint32_t channel_mask = 0;
    double default_period_ms = 0, minimum_period_ms = 0;        // IAudioClient::GetDevicePeriod (the exclusive-mode periods)
    double shared_default_period_ms = 0, shared_minimum_period_ms = 0;   // IAudioClient3::GetSharedModeEnginePeriod (0 when unsupported)
    bool supports_low_latency_shared = false;
};
// COM is initialised (MTA) on the calling thread when needed.
Status enumerate_audio_devices(AudioFlow flow, std::vector<AudioDeviceInfo>& out);
bool default_audio_device(AudioFlow flow, AudioDeviceInfo& out);

// ---- Streams -----------------------------------------------------------------------------------------------------------------------------------
enum class ShareMode : uint8_t { Shared, Exclusive };
using AudioRenderCallback = std::function<void(float* interleaved, uint32_t frames, const AudioFormat& format)>;

struct AudioStreamConfig {
    std::string device_id;                 // empty = default device; with follow_default_device the stream moves when Windows changes it
    AudioFormat format;                    // what the callback produces (sample type is always F32 on the callback side)
    ShareMode share = ShareMode::Shared;
    uint32_t period_frames = 0;            // wanted period at format.sample_rate; 0 = the smallest the mode supports (low latency) or the engine default
    bool low_latency = true;               // shared mode: use IAudioClient3 to shrink the engine period when the device allows it
    bool follow_default_device = true;
    bool mmcss = true;                     // render thread registered as "Pro Audio" (falls back to "Audio")
    bool allow_engine_conversion = true;   // shared mode: let the Windows audio engine convert (AUTOCONVERTPCM) instead of doing it ourselves
    float session_volume = 1.0f;           // applied to this stream's audio session only
    AudioRenderCallback callback;
    // Null stream only: receives the data after conversion to `null_device_format`.
    AudioFormat null_device_format;
    std::function<void(const void* data, uint32_t frames, const AudioFormat& format)> null_sink;
};

struct AudioStreamStats {
    bool running = false;
    bool exclusive = false;
    bool low_latency_path = false;         // IAudioClient3
    bool engine_conversion = false;        // Windows converts between our format and the mix format
    bool our_conversion = false;           // we convert (rate, channels or sample type)
    AudioFormat device_format;
    uint32_t period_frames = 0, buffer_frames = 0;
    double period_ms = 0, buffer_ms = 0, stream_latency_ms = 0;
    uint64_t frames_rendered = 0;
    uint32_t glitches = 0;                 // wakeups where the device buffer had already run dry
    uint32_t restarts = 0;                 // device changes / invalidations survived
    uint32_t callback_overruns = 0;        // callback took longer than one period
    double max_callback_ms = 0, avg_callback_ms = 0;
    std::string device_name;
    std::string last_error;
};

class AudioStream {
public:
    virtual ~AudioStream() = default;
    virtual Status start() = 0;
    virtual void stop() = 0;
    virtual AudioStreamStats stats() const = 0;
    virtual void set_volume(float volume) = 0;     // 0..1, session volume (WASAPI) or a gain in the null stream
};

std::unique_ptr<AudioStream> create_wasapi_stream(const AudioStreamConfig& config, Status* status = nullptr);
// Same threading, pacing, conversion and statistics, no device: paced by a high-resolution timer, output handed to `null_sink`.
std::unique_ptr<AudioStream> create_null_stream(const AudioStreamConfig& config, Status* status = nullptr);

}  // namespace nue::win
