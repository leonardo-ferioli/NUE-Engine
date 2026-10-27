// OutputSink conformance: what the DirectSound cursor logic needs from ANY sink, run on every sink the layer ships or tests with. A sink for a new platform
// (AAudio/Oboe, CoreAudio, PipeWire/ALSA, WASAPI exclusive...) is accepted when it passes this file with its own `advance` (time moves, the callback runs).
//
//   - open() reports the format the callback will be asked for; start/stop/running agree; close() waits for the callback and a closed sink can be opened again;
//   - latency() reports the time from handing a frame to hearing it (the play cursor is reported that far behind what was mixed);
//   - frames_played() is a monotonic device clock that advances with time and never runs ahead of what the callback produced;
//   - through an Engine, the DirectSound play cursor of a looping ring moves at every 10 ms poll (a client rebuilds its audio after 100 identical polls),
//     at the real-time rate of the output, and its jump never exceeds what was mixed.
// Sinks tested here: the manual sink (tests) and the native stream on the NUE null backend with its virtual clock. The real endpoint is checked by
// nue_dsound_endpoint_smoke (hardware, tool).
#include <chrono>
#include <functional>
#include <thread>

#include "nue/nue.h"
#include "nue/null_backend.h"
#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

namespace {

struct Counter {
    uint64_t frames = 0;
    uint32_t calls = 0;
    bool bad_request = false;
    uint32_t channels = 2;
    static void fn(void* user, float* out, uint32_t frames) {
        auto* c = static_cast<Counter*>(user);
        if (frames == 0 || !out) c->bad_request = true;
        for (uint32_t i = 0; i < frames * c->channels; ++i) out[i] = 0.0f;
        c->frames += frames;
        ++c->calls;
    }
};

// `advance(ms)` moves the sink's time forward by `ms` milliseconds so that its callback runs.
void conformance(const char* name, OutputSink& sink, const std::function<void(uint32_t)>& advance) {
    std::printf("sink: %s\n", name);
    Counter counter;
    SinkDesc desc;
    desc.spec = {48000, 2, speaker::stereo};
    desc.latency_ms = 20;
    CHECK(sink.open(desc, &Counter::fn, &counter));
    const OutputLayout actual = sink.actual();
    CHECK(actual.sample_rate >= 8000 && actual.channels >= 1 && actual.channels <= 8);
    counter.channels = actual.channels;
    CHECK(!sink.running());
    advance(50);
    CHECK(counter.calls == 0);                       // nothing runs before start
    CHECK(sink.start() && sink.running());
    const SinkLatency lat = sink.latency();
    CHECK(lat.device_buffer_frames > 0);
    CHECK(lat.total_latency_ns >= lat.device_latency_ns);
    uint64_t last = sink.frames_played();
    bool monotonic = true;
    for (int i = 0; i < 100; ++i) {
        advance(10);
        const uint64_t now = sink.frames_played();
        monotonic = monotonic && now >= last;
        last = now;
    }
    CHECK(monotonic);
    CHECK(counter.calls > 0 && !counter.bad_request);
    // About one second of virtual time: the device clock is near it, never ahead of what the callback produced.
    CHECK(sink.frames_played() <= counter.frames);
    CHECK_NEAR(double(sink.frames_played()) / actual.sample_rate, 1.0, 0.1);
    CHECK(sink.stats().underruns == 0);
    CHECK(sink.stop() && !sink.running());
    const uint32_t calls_stopped = counter.calls;
    advance(100);
    CHECK(counter.calls == calls_stopped);           // paused: no callbacks, the position is kept
    const uint64_t kept = sink.frames_played();
    advance(100);
    CHECK(sink.frames_played() == kept);
    sink.close();
    advance(100);
    CHECK(counter.calls == calls_stopped);           // closed: no callback after close() returns
    CHECK(sink.open(desc, &Counter::fn, &counter));  // and it can be opened again
    CHECK(sink.start());
    advance(20);
    CHECK(counter.calls > calls_stopped);
    sink.close();
}

// The DirectSound cursor through an Engine on this sink.
void cursor_contract(const char* name, std::unique_ptr<OutputSink> sink, const std::function<void(uint32_t)>& advance) {
    std::printf("cursor contract: %s\n", name);
    EngineConfig config;
    config.latency_ms = 20;
    auto engine = Engine::create(config, std::move(sink));
    CHECK(engine != nullptr);
    if (!engine) return;
    auto& d = engine->device();
    dsx::BufferDesc sd;
    sd.flags = 0x18000;
    sd.bytes = 22016 * 4;
    sd.has_format = true;
    sd.format = pcm16(44100, 2);
    auto ring = d.create_buffer(sd);
    CHECK(ring.hr == dsx::hr::ok);
    CHECK(engine->start());
    d.play(ring.id, 0, dsx::play_flag::looping);
    uint32_t last = ~0u;
    int frozen = 0, max_frozen = 0;
    double moved_bytes = 0;
    const uint32_t polls = 200;
    for (uint32_t i = 0; i < polls; ++i) {
        advance(10);
        uint32_t cursor = 0;
        CHECK(d.get_current_position(ring.id, &cursor, nullptr) == dsx::hr::ok);
        if (last != ~0u) {
            const uint32_t jump = (cursor + sd.bytes - last) % sd.bytes;
            moved_bytes += jump;
            CHECK(jump <= 4 * 44100 * 4 / 100 * 2);   // no jump of more than 20 ms of audio at a 10 ms poll
        }
        frozen = (cursor == last) ? frozen + 1 : 0;
        max_frozen = std::max(max_frozen, frozen);
        last = cursor;
    }
    CHECK(max_frozen == 0);
    CHECK_NEAR(moved_bytes / 4.0 / 44100.0, (polls - 1) * 0.010, 0.02);   // the cursor ran at the real-time rate
    engine->shutdown();
}

// A sink whose device is gone: it starts, reports running, and never calls back (an unplugged endpoint that is being re-acquired).
class SilentSink final : public OutputSink {
public:
    bool open(const SinkDesc& desc, SinkCallback, void*) override { spec_ = desc.spec; if (!spec_.sample_rate) spec_.sample_rate = 48000; return true; }
    OutputLayout actual() const override { return spec_; }
    bool start() override { running_ = true; return true; }
    bool stop() override { running_ = false; return true; }
    void close() override { running_ = false; }
    bool running() const override { return running_; }
    uint64_t frames_played() const override { return 0; }
    SinkLatency latency() const override { return {}; }
    SinkStats stats() const override { return {}; }

private:
    OutputLayout spec_;
    bool running_ = false;
};

// The DirectSound clock must not freeze when the output stops calling back (a client rebuilds its audio after 100 identical cursor reads), and must hand
// over to the sink as soon as it is back.
void clock_without_output() {
    std::printf("clock without output\n");
    EngineConfig config;
    config.sample_rate = 48000;
    config.keep_clock_without_output = true;
    config.watchdog_ms = 100;
    auto engine = Engine::create(config, std::make_unique<SilentSink>());
    CHECK(engine != nullptr);
    if (!engine) return;
    auto& d = engine->device();
    dsx::BufferDesc sd;
    sd.flags = 0x18000;
    sd.bytes = 22016 * 4;
    sd.has_format = true;
    sd.format = pcm16(44100, 2);
    auto ring = d.create_buffer(sd);
    CHECK(engine->start());
    d.play(ring.id, 0, dsx::play_flag::looping);
    const auto t0 = std::chrono::steady_clock::now();
    uint32_t last = 0;
    double moved_bytes = 0;
    int frozen = 0, max_frozen = 0;
    bool first = true;
    for (int i = 0; i < 60; ++i) {   // 600 ms of 10 ms polls in real time
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint32_t cur = 0;
        d.get_current_position(ring.id, &cur, nullptr);
        if (!first) {
            moved_bytes += (cur + sd.bytes - last) % sd.bytes;
            frozen = cur == last ? frozen + 1 : 0;
            if (i >= 15) max_frozen = std::max(max_frozen, frozen);   // before the watchdog takes over (watchdog_ms) nothing moves; a client tolerates 100 polls
        }
        first = false;
        last = cur;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch() - t0.time_since_epoch()).count();
    const double heard = moved_bytes / 4.0 / 44100.0;
    std::printf("  silent output: cursor moved %.3f s in %.3f s of wall time, watchdog rendered %llu frames, longest frozen run %d\n", heard, elapsed,
                static_cast<unsigned long long>(engine->watchdog_frames()), max_frozen);
    CHECK(engine->watchdog_frames() > 48000 * 3 / 10);   // it took over after 100 ms
    CHECK(heard > 0.3 && heard < elapsed + 0.05);        // the cursor ran at about the real-time rate (the first 100 ms plus the sink latency are not covered)
    CHECK(max_frozen < 3);
    engine->shutdown();
}

// With a sink that does call back, the watchdog stays out of the way.
void watchdog_stays_idle() {
    std::printf("watchdog idle with a live sink\n");
    EngineConfig config;
    config.sample_rate = 48000;
    config.keep_clock_without_output = true;
    config.watchdog_ms = 100;
    auto sink = std::make_unique<ManualSink>(480, 0);
    ManualSink* raw = sink.get();
    auto engine = Engine::create(config, std::move(sink));
    CHECK(engine && engine->start());
    if (!engine) return;
    for (int i = 0; i < 40; ++i) {   // a callback every 10 ms of wall time for 400 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        raw->advance(480);
    }
    CHECK(engine->watchdog_frames() == 0);
    engine->shutdown();
}

}  // namespace

int main() {
    {
        ManualSink sink(256, 480);
        conformance("manual", sink, [&](uint32_t ms) { sink.advance(48 * ms); });
    }
    {
        auto sink = std::make_unique<ManualSink>(256, 480);
        ManualSink* raw = sink.get();
        cursor_contract("manual", std::move(sink), [raw](uint32_t ms) { raw->advance(48 * ms); });
    }
    {
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.app_name = "dsound sink test";
        d.app_id = "dsound-sink-test";
        d.org_name = "NUE";
        d.backend = NUE_BACKEND_NULL;
        d.flags = NUE_INIT_HEADLESS | NUE_INIT_DETERMINISTIC | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD;
        d.log_level = NUE_LOG_WARN;
        CHECK(NUE_SUCCEEDED(nue_init(&d)));
        auto advance = [](uint32_t ms) { nue_null_advance_time(int64_t(ms) * 1000000); };
        {
            auto sink = make_nue_sink();
            conformance("native stream (null backend)", *sink, advance);
        }
        cursor_contract("native stream (null backend)", make_nue_sink(), advance);
        nue_shutdown();
    }
    clock_without_output();
    watchdog_stays_idle();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
