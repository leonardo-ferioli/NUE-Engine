#include <algorithm>
#include <chrono>
#include <vector>

#include "nue_legacy/dsound_engine.hpp"

namespace nue::dsound {

std::unique_ptr<Engine> Engine::create(const EngineConfig& config, std::unique_ptr<OutputSink> sink) {
    if (!sink) return nullptr;
    std::unique_ptr<Engine> e(new Engine());
    e->cfg_ = config;
    e->device_ = make_device(config.device);
    if (config.trace) e->device_ = make_recording_device(std::move(e->device_), *config.trace);
    e->device_->set_spatial(config.spatial);
    e->sink_ = std::move(sink);
    SinkDesc desc;
    desc.spec = {config.sample_rate, 2, speaker::stereo};
    desc.latency_ms = config.latency_ms;
    desc.match_device_channels = config.match_device_channels;
    if (!e->sink_->open(desc, &Engine::pull, e.get())) return nullptr;
    e->device_->set_format_callback(&Engine::primary_changed, e.get());
    e->apply_output(e->sink_->actual());
    if (config.keep_clock_without_output) e->watchdog_thread_ = std::thread([p = e.get()] { p->watchdog(); });
    return e;
}

Engine::~Engine() { shutdown(); }

void Engine::apply_output(const OutputLayout& actual) {
    const SinkLatency l = sink_->latency();
    const uint32_t latency_frames = uint32_t(l.device_latency_ns * actual.sample_rate / 1000000000ull);
    device_->set_output(actual, latency_frames);
}

bool Engine::start() {
    if (started_) return true;
    started_ = sink_->start();
    return started_;
}

void Engine::shutdown() {
    stop_watchdog_ = true;
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    if (!sink_) return;
    sink_->stop();
    sink_->close();
    started_ = false;
}

void Engine::pull(void* self, float* out, uint32_t frames) {
    Engine& e = *static_cast<Engine*>(self);
    e.last_callback_ns_.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    e.device_->render(out, frames);
}

// The clock when nobody is listening: while the sink has not called back for watchdog_ms the watchdog renders at the output rate into a scratch buffer, so the play
// cursor keeps moving at the real-time rate; it hands over as soon as the sink calls back again (the sink's clock then drives the device).
void Engine::watchdog() {
    using clock = std::chrono::steady_clock;
    auto now_ns = [] { return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count(); };
    std::vector<float> scratch;
    bool covering = false;
    int64_t anchor = 0;       // wall time up to which the watchdog has rendered
    double owed = 0;          // fractional frames carried between ticks
    while (!stop_watchdog_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!started_.load()) { covering = false; continue; }
        const int64_t now = now_ns();
        const int64_t silent = now - last_callback_ns_.load(std::memory_order_relaxed);
        if (silent < int64_t(cfg_.watchdog_ms) * 1000000) { covering = false; continue; }
        if (!covering) { covering = true; anchor = now; owed = 0; }
        const OutputLayout o = device_->output();
        owed += double(now - anchor) * 1e-9 * double(o.sample_rate);
        anchor = now;
        while (owed >= 256.0 && covering) {
            const uint32_t frames = uint32_t(std::min(owed, 4096.0));
            scratch.assign(size_t(frames) * o.channels, 0.0f);
            device_->render(scratch.data(), frames);
            watchdog_frames_ += frames;
            owed -= frames;
        }
    }
}

void Engine::primary_changed(void* self, const WaveFormat& format) {
    Engine& e = *static_cast<Engine*>(self);
    if (!e.cfg_.follow_primary_channels || !e.sink_) return;
    const OutputLayout now = e.sink_->actual();
    const uint32_t want = std::clamp<uint32_t>(format.channels, 2u, 8u);   // a mono primary still plays on both speakers
    if (want == now.channels) return;
    const bool was_running = e.started_;
    e.sink_->stop();
    e.sink_->close();
    SinkDesc desc;
    desc.spec = {now.sample_rate, want, speaker::default_mask(want)};
    desc.latency_ms = e.cfg_.latency_ms;
    desc.match_device_channels = e.cfg_.match_device_channels;
    if (!e.sink_->open(desc, &Engine::pull, &e)) {
        // Could not open the wider layout: go back to what worked.
        desc.spec = now;
        if (!e.sink_->open(desc, &Engine::pull, &e)) return;
    }
    e.apply_output(e.sink_->actual());
    ++e.reopens_;
    if (was_running) e.sink_->start();
}

}  // namespace nue::dsound
