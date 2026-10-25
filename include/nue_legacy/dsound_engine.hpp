// legacy_dsound: the engine ties a Device (DirectSound semantics) to an OutputSink. The sink is the one place where the layer meets an output
// device; the shipped implementation (make_nue_sink) is a native audio stream (nue_audio_stream, callback model). A test, or a host that
// has its own audio path, supplies another one.
#pragma once
#include <cstdint>
#include <memory>
#include <string_view>

#include "nue_legacy/dsound_device.hpp"

namespace nue::dsound {

// [audio thread] Fill `frames` frames of interleaved float32 in the sink's format. Always fill the whole block (silence if nothing to play).
// Real-time rules as in nue_audio_callback: no allocation, no I/O, no logging.
using SinkCallback = void (*)(void* user, float* out, uint32_t frames);

struct SinkDesc {
    OutputLayout spec;                // what the layer wants to produce; the sink may report a different `actual` layout
    uint32_t latency_ms = 40;         // target total latency (nue_audio_stream_desc.latency_ms), 0 = sink default
    bool low_latency = false;         // NUE_AUDIO_LOW_LATENCY
    std::string_view name = "legacy_dsound";   // shown in OS mixers
};

struct SinkLatency {
    uint32_t device_buffer_frames = 0;
    uint64_t device_latency_ns = 0;   // from handing a frame to the device to hearing it
    uint64_t total_latency_ns = 0;
};

struct SinkStats {
    uint64_t frames_played = 0;       // total frames the device consumed since start
    uint64_t underruns = 0;
    float peak_level = 0;
    float cpu_load = 0;
};

// Thread: open/close/start/stop from one controlling thread; the queries are callable from any thread; the callback runs on the sink's thread.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    // Opens (but does not start) an output. After a successful open `actual()` is the format the callback will be asked for; it equals
    // `desc.spec` unless the sink had to choose another one (a device with fewer channels): the engine then re-configures the device.
    virtual bool open(const SinkDesc& desc, SinkCallback callback, void* user) = 0;
    virtual OutputLayout actual() const = 0;
    virtual bool start() = 0;
    virtual bool stop() = 0;      // pause: keeps the position
    virtual void close() = 0;     // waits for the audio thread to leave the callback
    virtual bool running() const = 0;
    virtual uint64_t frames_played() const = 0;   // device clock
    virtual SinkLatency latency() const = 0;
    virtual SinkStats stats() const = 0;
    // The underlying native object, or null: a nue_audio_stream for the native sink (tests read the null backend's captured output through it).
    virtual void* native_handle() const { return nullptr; }
};

// The native sink: a nue_audio_stream with a callback on the default output device (device 0 follows the system default). nue_init must have
// been called with the audio subsystem (a headless run uses the null backend).
std::unique_ptr<OutputSink> make_nue_sink();

struct EngineConfig {
    Config device;
    uint32_t sample_rate = 0;            // what the engine asks the sink for; 0 = the default device's preferred rate (48000 if unknown)
    uint32_t latency_ms = 40;
    SpatialOptions spatial;
    bool follow_primary_channels = true; // when the application sets a 4/6 channel primary format, ask the sink for that many channels (falls back to its own layout)
};

class Engine {
public:
    // Opens the sink (not started). Returns nullptr when the sink cannot open.
    static std::unique_ptr<Engine> create(const EngineConfig& config, std::unique_ptr<OutputSink> sink);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start();
    void shutdown();                      // stops and closes the sink; the device stays usable for inspection
    Device& device() { return *device_; }
    OutputSink& sink() { return *sink_; }
    uint32_t reopen_count() const { return reopens_; }   // times the sink was reopened to follow a new primary format

private:
    Engine() = default;
    static void pull(void* self, float* out, uint32_t frames);
    static void primary_changed(void* self, const WaveFormat& format);
    void apply_output(const OutputLayout& actual);

    EngineConfig cfg_;
    std::unique_ptr<Device> device_;
    std::unique_ptr<OutputSink> sink_;
    bool started_ = false;
    uint32_t reopens_ = 0;
};

}  // namespace nue::dsound
