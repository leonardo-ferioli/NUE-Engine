#include <algorithm>

#include "nue_legacy/dsound_engine.hpp"

namespace nue::dsound {

std::unique_ptr<Engine> Engine::create(const EngineConfig& config, std::unique_ptr<OutputSink> sink) {
    if (!sink) return nullptr;
    std::unique_ptr<Engine> e(new Engine());
    e->cfg_ = config;
    e->device_ = make_device(config.device);
    e->device_->set_spatial(config.spatial);
    e->sink_ = std::move(sink);
    SinkDesc desc;
    desc.spec = {config.sample_rate, 2, speaker::stereo};
    desc.latency_ms = config.latency_ms;
    desc.match_device_channels = config.match_device_channels;
    if (!e->sink_->open(desc, &Engine::pull, e.get())) return nullptr;
    e->device_->set_format_callback(&Engine::primary_changed, e.get());
    e->apply_output(e->sink_->actual());
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
    if (!sink_) return;
    sink_->stop();
    sink_->close();
    started_ = false;
}

void Engine::pull(void* self, float* out, uint32_t frames) { static_cast<Engine*>(self)->device_->render(out, frames); }

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
