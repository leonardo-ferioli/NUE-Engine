// Audio streams (push and callback models) on top of one backend device, plus format / channel / rate conversion.
#include "internal/audio_convert.h"
#include "internal/core_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace nuei {

// ---- conversion ----------------------------------------------------------------------------------------------------------------

uint32_t bytes_per_sample(nue_audio_format f) {
    switch (f) {
        case NUE_AUDIO_S16: return 2;
        case NUE_AUDIO_F32: return 4;
        case NUE_AUDIO_U8: return 1;
        default: return 0;
    }
}

void to_float(const nue_audio_spec& spec, const void* src, uint32_t frames, float* dst) {
    const size_t n = size_t(frames) * spec.channels;
    switch (spec.format) {
        case NUE_AUDIO_F32: std::memcpy(dst, src, n * sizeof(float)); break;
        case NUE_AUDIO_S16: { const int16_t* s = static_cast<const int16_t*>(src); for (size_t i = 0; i < n; ++i) dst[i] = float(s[i]) * (1.0f / 32768.0f); break; }
        case NUE_AUDIO_U8: { const uint8_t* s = static_cast<const uint8_t*>(src); for (size_t i = 0; i < n; ++i) dst[i] = (float(s[i]) - 128.0f) * (1.0f / 128.0f); break; }
        default: std::memset(dst, 0, n * sizeof(float)); break;
    }
}

void from_float(const nue_audio_spec& spec, const float* src, uint32_t frames, void* dst) {
    const size_t n = size_t(frames) * spec.channels;
    switch (spec.format) {
        case NUE_AUDIO_F32: std::memcpy(dst, src, n * sizeof(float)); break;
        case NUE_AUDIO_S16: {
            int16_t* d = static_cast<int16_t*>(dst);
            for (size_t i = 0; i < n; ++i) { float v = src[i] * 32768.0f; v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v; d[i] = int16_t(std::lrintf(v)); }
            break;
        }
        case NUE_AUDIO_U8: {
            uint8_t* d = static_cast<uint8_t*>(dst);
            for (size_t i = 0; i < n; ++i) { float v = src[i] * 128.0f + 128.0f; v = v > 255.0f ? 255.0f : v < 0.0f ? 0.0f : v; d[i] = uint8_t(std::lrintf(v)); }
            break;
        }
        default: std::memset(dst, 0, n * bytes_per_sample(spec.format)); break;
    }
}

// Channel layouts: 1 FC/mono, 2 FL FR, 4 FL FR BL BR, 6 FL FR FC LFE BL BR, 8 FL FR FC LFE BL BR SL SR. Other counts copy the common prefix.
void remap_channels(const float* src, uint32_t src_ch, uint32_t frames, float* dst, uint32_t dst_ch) {
    if (src_ch == dst_ch) { std::memcpy(dst, src, size_t(frames) * src_ch * sizeof(float)); return; }
    const float k = 0.70710678f;
    for (uint32_t f = 0; f < frames; ++f) {
        const float* s = src + size_t(f) * src_ch;
        float* d = dst + size_t(f) * dst_ch;
        for (uint32_t c = 0; c < dst_ch; ++c) d[c] = 0;
        if (src_ch == 1) {
            if (dst_ch >= 2) { d[0] = s[0] * k; d[1] = s[0] * k; if (dst_ch >= 3 && dst_ch != 4) d[2] = 0; }
            else d[0] = s[0];
        } else if (dst_ch == 1) {
            float sum = 0;
            for (uint32_t c = 0; c < src_ch; ++c) if (!(src_ch >= 6 && c == 3)) sum += s[c];   // skip LFE
            d[0] = sum / float(src_ch >= 6 ? src_ch - 1 : src_ch);
        } else if (dst_ch == 2) {
            float l = s[0], r = s[1];
            if (src_ch == 4) { l += k * s[2]; r += k * s[3]; }
            else if (src_ch >= 6) {
                l += k * s[2] + k * s[4]; r += k * s[2] + k * s[5];
                if (src_ch >= 8) { l += k * s[6]; r += k * s[7]; }
                l *= 0.5f; r *= 0.5f;   // keep headroom: the matrix sums up to ~2.4 of unity
            }
            d[0] = l; d[1] = r;
        } else {
            uint32_t common = src_ch < dst_ch ? src_ch : dst_ch;
            for (uint32_t c = 0; c < common; ++c) d[c] = s[c];
        }
    }
}

void Resampler::reset(uint32_t channels) {
    channels_ = channels;
    pos_ = 0;
    fifo_.clear();
}

uint32_t Resampler::input_needed(uint32_t out_frames, double step) const {
    double need = pos_ + double(out_frames) * step;
    uint32_t n = uint32_t(std::ceil(need)) + 1;   // +1: interpolation reads idx+1
    uint32_t have = uint32_t(fifo_.size() / channels_);
    return n > have ? n - have : 0;
}

void Resampler::push(const float* frames, uint32_t count) {
    fifo_.insert(fifo_.end(), frames, frames + size_t(count) * channels_);
}

uint32_t Resampler::produce(uint32_t out_frames, double step, float* out) {
    const uint32_t ch = channels_;
    uint32_t avail = uint32_t(fifo_.size() / ch);
    uint32_t made = 0;
    double p = pos_;
    while (made < out_frames) {
        uint32_t idx = uint32_t(p);
        if (idx + 1 >= avail) break;
        float f = float(p - idx);
        const float* a = &fifo_[size_t(idx) * ch];
        const float* b = a + ch;
        for (uint32_t c = 0; c < ch; ++c) out[size_t(made) * ch + c] = a[c] + (b[c] - a[c]) * f;
        p += step;
        ++made;
    }
    uint32_t consumed = uint32_t(p);
    if (consumed > avail) consumed = avail;
    fifo_.erase(fifo_.begin(), fifo_.begin() + size_t(consumed) * ch);
    pos_ = p - consumed;
    return made;
}

}  // namespace nuei

extern "C" {

NUE_API uint32_t NUE_CALL nue_audio_bytes_per_sample(nue_audio_format format) { return nuei::bytes_per_sample(format); }
NUE_API uint32_t NUE_CALL nue_audio_frame_size(const nue_audio_spec* spec) { return spec ? nuei::bytes_per_sample(spec->format) * spec->channels : 0; }

NUE_API uint32_t NUE_CALL nue_audio_convert(const nue_audio_spec* ss, const void* src, uint32_t src_frames, const nue_audio_spec* ds, void* dst, uint32_t dst_cap) {
    if (!ss || !ds || !src || !dst || ss->channels == 0 || ds->channels == 0 || ss->channels > NUE_AUDIO_MAX_CHANNELS || ds->channels > NUE_AUDIO_MAX_CHANNELS) return 0;
    if (!nuei::bytes_per_sample(ss->format) || !nuei::bytes_per_sample(ds->format) || !ss->sample_rate || !ds->sample_rate) return 0;
    std::vector<float> a(size_t(src_frames) * ss->channels);
    nuei::to_float(*ss, src, src_frames, a.data());
    std::vector<float> b(size_t(src_frames) * ds->channels);
    nuei::remap_channels(a.data(), ss->channels, src_frames, b.data(), ds->channels);
    uint32_t out_frames = src_frames;
    if (ss->sample_rate != ds->sample_rate) {
        nuei::Resampler rs;
        rs.reset(ds->channels);
        const double step = double(ss->sample_rate) / double(ds->sample_rate);
        rs.push(b.data(), src_frames);
        uint32_t want = uint32_t(double(src_frames) / step);
        want = std::min(want, dst_cap);
        std::vector<float> c(size_t(want) * ds->channels);
        out_frames = rs.produce(want, step, c.data());
        nuei::from_float(*ds, c.data(), out_frames, dst);
        return out_frames;
    }
    out_frames = std::min(src_frames, dst_cap);
    nuei::from_float(*ds, b.data(), out_frames, dst);
    return out_frames;
}

}  // extern "C"

// ---- streams ---------------------------------------------------------------------------------------------------------------------

struct nue_audio_stream_s {
    nue_audio_spec app_spec;
    nue_audio_spec dev_spec;
    nue_audio_stream_flags flags = 0;
    nue_audio_callback callback = nullptr;
    void* userdata = nullptr;
    void* device = nullptr;
    uint32_t app_frame_size = 0, dev_frame_size = 0;
    uint32_t device_buffer_frames = 0;
    uint64_t device_latency_ns = 0;
    std::string name;
    nue_audio_device_id device_id = 0;

    // push queue: byte ring, single consumer (audio thread), producers serialised by push_mutex
    std::vector<uint8_t> ring;
    size_t ring_frames = 0;
    std::atomic<uint64_t> write_idx{0}, read_idx{0}, discard_to{0};
    std::mutex push_mutex;
    std::condition_variable push_cv;   // signalled when the consumer freed room
    std::mutex wait_mutex;

    std::atomic<bool> running{false};
    std::atomic<bool> background_paused{false};
    std::atomic<bool> swapping{false};
    std::atomic<float> volume{1.0f};
    std::atomic<uint64_t> frames_played{0}, underruns{0}, dropped{0};
    std::atomic<float> peak{0};
    std::atomic<float> cpu_load{0};
    std::atomic<int> in_callback{0};

    // conversion scratch (audio thread only)
    nuei::Resampler resampler;
    std::vector<uint8_t> app_block;      // frames in app format pulled from queue / callback
    std::vector<float> f_app, f_remap, f_out;
    bool needs_convert = false;
};

namespace {

struct StreamRegistry {
    std::mutex mutex;
    std::vector<nue_audio_stream> streams;
    bool watcher_installed = false;
};
StreamRegistry& reg() { static StreamRegistry* r = new StreamRegistry(); return *r; }

const nue_backend_audio* audio_backend() {
    const nue_backend* b = nuei::backend();
    return b ? b->audio : nullptr;
}

// Pull `frames` app-format frames from the queue or the user callback. Silence on underrun.
void pull_app_frames(nue_audio_stream s, uint8_t* dst, uint32_t frames) {
    if (s->callback) {
        s->callback(s->userdata, dst, frames);
        s->frames_played += frames;
        return;
    }
    const size_t fsz = s->app_frame_size;
    uint64_t r = s->read_idx.load(std::memory_order_acquire);
    const uint64_t discard = s->discard_to.load(std::memory_order_acquire);
    if (discard > r) r = discard;
    const uint64_t w = s->write_idx.load(std::memory_order_acquire);
    uint64_t avail = w > r ? w - r : 0;
    uint32_t take = uint32_t(std::min<uint64_t>(avail, frames));
    for (uint32_t i = 0; i < take; ++i) std::memcpy(dst + size_t(i) * fsz, &s->ring[size_t((r + i) % s->ring_frames) * fsz], fsz);
    if (take < frames) {
        std::memset(dst + size_t(take) * fsz, 0, size_t(frames - take) * fsz);
        if (s->running.load() && take == 0) ++s->underruns;
        else if (take < frames) ++s->underruns;
    }
    s->read_idx.store(r + take, std::memory_order_release);
    s->frames_played += take;
    if (take) s->push_cv.notify_all();
}

void apply_volume_and_peak(nue_audio_stream s, void* out, uint32_t frames) {
    const float vol = s->volume.load(std::memory_order_relaxed);
    const size_t n = size_t(frames) * s->dev_spec.channels;
    float peak = 0;
    if (s->dev_spec.format == NUE_AUDIO_F32) {
        float* p = static_cast<float*>(out);
        for (size_t i = 0; i < n; ++i) { float v = p[i] * vol; p[i] = v; float a = std::fabs(v); if (a > peak) peak = a; }
    } else if (s->dev_spec.format == NUE_AUDIO_S16) {
        int16_t* p = static_cast<int16_t*>(out);
        for (size_t i = 0; i < n; ++i) {
            float v = float(p[i]) * vol;
            float a = std::fabs(v) * (1.0f / 32768.0f);
            if (a > peak) peak = a;
            v = v > 32767.0f ? 32767.0f : v < -32768.0f ? -32768.0f : v;
            p[i] = int16_t(std::lrintf(v));
        }
    }
    s->peak.store(peak, std::memory_order_relaxed);
}

// [audio thread]
void NUE_CALL device_callback(void* userdata, void* out, uint32_t frames) {
    nue_audio_stream s = static_cast<nue_audio_stream>(userdata);
    if (s->swapping.load() || !s->running.load() || s->background_paused.load()) {
        std::memset(out, s->dev_spec.format == NUE_AUDIO_U8 ? 128 : 0, size_t(frames) * s->dev_frame_size);
        return;
    }
    s->in_callback.fetch_add(1);
    const int64_t t0 = nuei::now_ns();
    if (!s->needs_convert) {
        if (s->app_block.size() < size_t(frames) * s->app_frame_size) s->app_block.resize(size_t(frames) * s->app_frame_size);
        pull_app_frames(s, static_cast<uint8_t*>(out), frames);
    } else {
        // app format -> float -> channel remap -> resample -> device format
        const double step = double(s->app_spec.sample_rate) / double(s->dev_spec.sample_rate);
        const bool resample = s->app_spec.sample_rate != s->dev_spec.sample_rate;
        const uint32_t dch = s->dev_spec.channels, ach = s->app_spec.channels;
        uint32_t src_frames = resample ? s->resampler.input_needed(frames, step) : frames;
        if (s->app_block.size() < size_t(src_frames) * s->app_frame_size) s->app_block.resize(size_t(src_frames) * s->app_frame_size);
        if (s->f_app.size() < size_t(src_frames) * ach) s->f_app.resize(size_t(src_frames) * ach);
        if (s->f_remap.size() < size_t(std::max(src_frames, frames)) * dch) s->f_remap.resize(size_t(std::max(src_frames, frames)) * dch);
        if (s->f_out.size() < size_t(frames) * dch) s->f_out.resize(size_t(frames) * dch);
        if (src_frames) {
            pull_app_frames(s, s->app_block.data(), src_frames);
            nuei::to_float(s->app_spec, s->app_block.data(), src_frames, s->f_app.data());
            nuei::remap_channels(s->f_app.data(), ach, src_frames, s->f_remap.data(), dch);
        }
        if (resample) {
            if (src_frames) s->resampler.push(s->f_remap.data(), src_frames);
            uint32_t made = s->resampler.produce(frames, step, s->f_out.data());
            if (made < frames) std::memset(&s->f_out[size_t(made) * dch], 0, size_t(frames - made) * dch * sizeof(float));
        } else {
            std::memcpy(s->f_out.data(), s->f_remap.data(), size_t(frames) * dch * sizeof(float));
        }
        nuei::from_float(s->dev_spec, s->f_out.data(), frames, out);
    }
    apply_volume_and_peak(s, out, frames);
    const int64_t t1 = nuei::now_ns();
    const double budget = double(frames) * 1e9 / double(s->dev_spec.sample_rate);
    if (budget > 0) { float load = float(double(t1 - t0) / budget); s->cpu_load.store(s->cpu_load.load() * 0.9f + load * 0.1f); }
    s->in_callback.fetch_sub(1);
}

nue_bool NUE_CALL lifecycle_watcher(void*, const nue_event* e) {
    if (e->type != NUE_EVENT_APP_PAUSING && e->type != NUE_EVENT_APP_RESUMING) return NUE_TRUE;
    StreamRegistry& r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    for (nue_audio_stream s : r.streams) {
        if (s->flags & NUE_AUDIO_KEEP_RUNNING_IN_BACKGROUND) continue;
        s->background_paused = e->type == NUE_EVENT_APP_PAUSING;
        const nue_backend_audio* a = audio_backend();
        if (a && a->pause && s->device && s->running.load()) a->pause(s->device, e->type == NUE_EVENT_APP_PAUSING ? NUE_TRUE : NUE_FALSE);
    }
    return NUE_TRUE;
}

nue_result open_device(nue_audio_stream s, uint32_t latency_ms) {
    const nue_backend_audio* a = audio_backend();
    nue_backend_device_open o;
    NUE_STRUCT_INIT(&o);
    o.device = s->device_id;
    o.spec = s->app_spec;
    o.low_latency = (s->flags & NUE_AUDIO_LOW_LATENCY) ? NUE_TRUE : NUE_FALSE;
    o.buffer_frames = latency_ms ? std::max<uint32_t>(32, uint32_t(uint64_t(latency_ms) * s->app_spec.sample_rate / 1000 / 2)) : 0;
    o.callback = device_callback;
    o.userdata = s;
    o.stream_name = s->name.c_str();
    void* dev = nullptr;
    nue_backend_device_info info;
    NUE_STRUCT_INIT(&info);
    nue_result r = a->open(&o, &dev, &info);
    if (NUE_FAILED(r)) return r;
    s->device = dev;
    s->dev_spec = info.actual;
    s->dev_frame_size = nue_audio_frame_size(&s->dev_spec);
    s->device_buffer_frames = info.buffer_frames;
    s->device_latency_ns = info.latency_ns;
    s->needs_convert = std::memcmp(&s->dev_spec, &s->app_spec, sizeof(nue_audio_spec)) != 0;
    if ((s->flags & NUE_AUDIO_EXACT_FORMAT) && s->needs_convert) {
        a->close(dev);
        s->device = nullptr;
        return nuei::fail(NUE_ERR_UNSUPPORTED, "the device cannot play the requested format exactly (NUE_AUDIO_EXACT_FORMAT)");
    }
    s->resampler.reset(s->dev_spec.channels);
    return NUE_OK;
}

}  // namespace

namespace nuei {

void audio_shutdown() {
    std::vector<nue_audio_stream> left;
    { StreamRegistry& r = reg(); std::lock_guard<std::mutex> lock(r.mutex); left = r.streams; r.watcher_installed = false; }
    if (!left.empty()) NUE_LOGW("audio", "%zu audio stream(s) were still open at shutdown and are being closed", left.size());
    for (nue_audio_stream s : left) nue_audio_stream_destroy(s);
}

void* audio_stream_backend_device(nue_audio_stream stream) {
    StreamRegistry& r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (std::find(r.streams.begin(), r.streams.end(), stream) == r.streams.end()) return nullptr;
    return stream->device;
}

}  // namespace nuei

extern "C" {

NUE_API nue_result NUE_CALL nue_audio_list_devices(nue_bool capture, nue_audio_device_info* infos, uint32_t capacity, uint32_t* out_count) {
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    const nue_backend_audio* a = audio_backend();
    if (!a || !a->list_devices) return nuei::fail(NUE_ERR_UNSUPPORTED, "this backend has no audio support");
    return a->list_devices(capture, infos, capacity, out_count);
}

NUE_API nue_result NUE_CALL nue_audio_get_device_info(nue_audio_device_id id, nue_audio_device_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    uint32_t count = 0;
    nue_result r = nue_audio_list_devices(NUE_FALSE, nullptr, 0, &count);
    if (NUE_FAILED(r)) return r;
    std::vector<nue_audio_device_info> v(count);
    for (auto& d : v) NUE_STRUCT_INIT(&d);
    r = nue_audio_list_devices(NUE_FALSE, v.data(), count, &count);
    if (NUE_FAILED(r)) return r;
    for (uint32_t i = 0; i < count && i < v.size(); ++i) {
        if ((id == 0 && v[i].is_default) || (id != 0 && v[i].id == id)) {
            uint32_t keep = out->struct_size;
            std::memcpy(out, &v[i], keep < sizeof(v[i]) ? keep : sizeof(v[i]));
            out->struct_size = keep;
            return NUE_OK;
        }
    }
    return nuei::fail(NUE_ERR_NOT_FOUND, "audio device %u not found", id);
}

NUE_API nue_result NUE_CALL nue_audio_stream_create(const nue_audio_stream_desc* desc, nue_audio_stream* out) {
    if (!desc || !out) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_audio_stream_create: desc and out are required");
    *out = nullptr;
    if (nuei::require_init() < 0) return NUE_ERR_NOT_INITIALIZED;
    if (!nuei::subsystem_enabled(NUE_SUBSYSTEM_AUDIO)) return nuei::fail(NUE_ERR_BAD_STATE, "the audio subsystem was not requested at nue_init");
    const nue_backend_audio* a = audio_backend();
    if (!a || !a->open) return nuei::fail(NUE_ERR_UNSUPPORTED, "this backend has no audio support");
    const nue_audio_spec& sp = desc->spec;
    if ((sp.format != NUE_AUDIO_S16 && sp.format != NUE_AUDIO_F32) || sp.channels < 1 || sp.channels > NUE_AUDIO_MAX_CHANNELS || sp.sample_rate < 8000 || sp.sample_rate > 384000)
        return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "unsupported stream format (format %d, %u channels, %u Hz)", int(sp.format), sp.channels, sp.sample_rate);
    if (desc->flags & NUE_AUDIO_CAPTURE) return nuei::fail(NUE_ERR_UNSUPPORTED, "capture streams are not implemented yet");

    nue_audio_stream s = new (std::nothrow) nue_audio_stream_s();
    if (!s) return NUE_ERR_OUT_OF_MEMORY;
    s->app_spec = sp;
    s->app_frame_size = nue_audio_frame_size(&sp);
    s->flags = desc->flags;
    s->callback = desc->callback;
    s->userdata = desc->userdata;
    s->device_id = desc->device;
    s->name = desc->name ? desc->name : "NUE stream";
    if (!s->callback) {
        uint32_t qf = desc->queue_frames ? desc->queue_frames : sp.sample_rate / 4;
        s->ring_frames = qf;
        s->ring.assign(size_t(qf) * s->app_frame_size, 0);
    }
    uint32_t latency_ms = desc->latency_ms;
    nue_result r = open_device(s, latency_ms);
    if (NUE_FAILED(r)) { delete s; return r; }
    // scratch for the audio thread, sized for typical callbacks so the callback does not allocate
    uint32_t cap = std::max<uint32_t>(s->device_buffer_frames, 512) * 2;
    s->app_block.reserve(size_t(cap) * s->app_frame_size * 2);
    s->f_app.reserve(size_t(cap) * 2 * sp.channels);
    s->f_remap.reserve(size_t(cap) * 2 * s->dev_spec.channels);
    s->f_out.reserve(size_t(cap) * s->dev_spec.channels);
    {
        StreamRegistry& rg = reg();
        std::lock_guard<std::mutex> lock(rg.mutex);
        rg.streams.push_back(s);
        if (!rg.watcher_installed) { nue_event_add_watcher(lifecycle_watcher, nullptr); rg.watcher_installed = true; }
    }
    if (!(desc->flags & NUE_AUDIO_START_PAUSED)) nue_audio_stream_start(s);
    NUE_LOGD("audio", "stream '%s' opened: app %u ch %u Hz, device %u ch %u Hz, buffer %u frames", s->name.c_str(), sp.channels, sp.sample_rate,
             s->dev_spec.channels, s->dev_spec.sample_rate, s->device_buffer_frames);
    *out = s;
    return NUE_OK;
}

static bool known_stream(nue_audio_stream s) {
    StreamRegistry& r = reg();
    std::lock_guard<std::mutex> lock(r.mutex);
    return std::find(r.streams.begin(), r.streams.end(), s) != r.streams.end();
}

NUE_API void NUE_CALL nue_audio_stream_destroy(nue_audio_stream s) {
    if (!s) return;
    {
        StreamRegistry& r = reg();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = std::find(r.streams.begin(), r.streams.end(), s);
        if (it == r.streams.end()) return;
        r.streams.erase(it);
    }
    s->running = false;
    const nue_backend_audio* a = audio_backend();
    if (a && a->close && s->device) a->close(s->device);   // returns when the callback is no longer running
    s->push_cv.notify_all();
    delete s;
}

NUE_API nue_result NUE_CALL nue_audio_stream_start(nue_audio_stream s) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    const nue_backend_audio* a = audio_backend();
    s->running = true;
    if (a && a->pause && s->device) a->pause(s->device, s->background_paused.load() ? NUE_TRUE : NUE_FALSE);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_stop(nue_audio_stream s) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    s->running = false;
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_audio_stream_is_running(nue_audio_stream s) { return known_stream(s) && s->running.load() ? NUE_TRUE : NUE_FALSE; }

NUE_API nue_result NUE_CALL nue_audio_stream_flush(nue_audio_stream s) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    if (s->callback) return NUE_ERR_BAD_STATE;
    std::lock_guard<std::mutex> lock(s->push_mutex);
    uint64_t w = s->write_idx.load();
    uint64_t r = std::max(s->read_idx.load(), s->discard_to.load());
    s->dropped += w > r ? w - r : 0;
    s->discard_to.store(w);
    s->push_cv.notify_all();
    return NUE_OK;
}

static uint32_t free_frames_locked(nue_audio_stream s) {
    uint64_t w = s->write_idx.load();
    uint64_t r = std::max(s->read_idx.load(), s->discard_to.load());
    uint64_t used = w > r ? w - r : 0;
    return used >= s->ring_frames ? 0 : uint32_t(s->ring_frames - used);
}

static uint32_t copy_into_ring(nue_audio_stream s, const uint8_t* src, uint32_t frames) {
    uint32_t room = free_frames_locked(s);
    uint32_t n = std::min(room, frames);
    uint64_t w = s->write_idx.load();
    for (uint32_t i = 0; i < n; ++i) std::memcpy(&s->ring[size_t((w + i) % s->ring_frames) * s->app_frame_size], src + size_t(i) * s->app_frame_size, s->app_frame_size);
    s->write_idx.store(w + n, std::memory_order_release);
    return n;
}

NUE_API nue_result NUE_CALL nue_audio_stream_push(nue_audio_stream s, const void* frames, uint32_t frame_count, uint32_t* accepted) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    if (s->callback) return nuei::fail(NUE_ERR_BAD_STATE, "this stream uses the callback model; it cannot be pushed to");
    if (!frames && frame_count) return NUE_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(s->push_mutex);
    uint32_t n = copy_into_ring(s, static_cast<const uint8_t*>(frames), frame_count);
    if (accepted) *accepted = n;
    if (n < frame_count) { s->dropped += frame_count - n; return NUE_OK_PARTIAL; }
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_push_wait(nue_audio_stream s, const void* frames, uint32_t frame_count, int64_t timeout_ns, uint32_t* accepted) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    if (s->callback) return nuei::fail(NUE_ERR_BAD_STATE, "this stream uses the callback model; it cannot be pushed to");
    const uint8_t* src = static_cast<const uint8_t*>(frames);
    uint32_t done = 0;
    const int64_t deadline = timeout_ns < 0 ? INT64_MAX : nuei::now_ns() + timeout_ns;
    while (done < frame_count) {
        {
            std::lock_guard<std::mutex> lock(s->push_mutex);
            done += copy_into_ring(s, src + size_t(done) * s->app_frame_size, frame_count - done);
        }
        if (done >= frame_count) break;
        if (deadline != INT64_MAX && nuei::now_ns() >= deadline) break;
        if (!s->running.load()) break;   // nobody is draining: waiting would never end
        std::unique_lock<std::mutex> wl(s->wait_mutex);
        s->push_cv.wait_for(wl, std::chrono::milliseconds(2));
        if (nuei::deterministic()) { wl.unlock(); nue_sleep_ns(1000000, NUE_SLEEP_COARSE); }   // virtual time must move for the device to drain
    }
    if (accepted) *accepted = done;
    return done == frame_count ? NUE_OK : (done ? NUE_OK_PARTIAL : NUE_ERR_TIMEOUT);
}

NUE_API uint32_t NUE_CALL nue_audio_stream_queued_frames(nue_audio_stream s) {
    if (!known_stream(s) || s->callback) return 0;
    uint64_t w = s->write_idx.load(), r = std::max(s->read_idx.load(), s->discard_to.load());
    return w > r ? uint32_t(w - r) : 0;
}

NUE_API uint32_t NUE_CALL nue_audio_stream_free_frames(nue_audio_stream s) {
    if (!known_stream(s) || s->callback) return 0;
    std::lock_guard<std::mutex> lock(s->push_mutex);
    return free_frames_locked(s);
}

NUE_API nue_result NUE_CALL nue_audio_stream_get_position(nue_audio_stream s, uint64_t* frames, int64_t* time_ns) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    if (frames) *frames = s->frames_played.load();
    if (time_ns) *time_ns = nuei::now_ns();
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_get_latency(nue_audio_stream s, nue_audio_latency* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    nue_audio_latency l;
    NUE_STRUCT_INIT(&l);
    l.device_buffer_frames = s->device_buffer_frames;
    l.device_latency_ns = s->device_latency_ns;
    l.queued_frames = nue_audio_stream_queued_frames(s);
    l.total_latency_ns = l.device_latency_ns + uint64_t(l.queued_frames) * 1000000000ull / s->app_spec.sample_rate;
    uint32_t keep = out->struct_size;
    std::memcpy(out, &l, keep < sizeof(l) ? keep : sizeof(l));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_set_latency(nue_audio_stream s, uint32_t latency_ms) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    const nue_backend_audio* a = audio_backend();
    if (!a || !a->open || !a->close) return NUE_ERR_UNSUPPORTED;
    s->swapping = true;   // callbacks that happen during the swap output silence
    void* old = s->device;
    a->close(old);
    s->device = nullptr;
    nue_result r = open_device(s, latency_ms);
    s->swapping = false;
    if (NUE_FAILED(r)) return r;
    if (a->pause) a->pause(s->device, (s->running.load() && !s->background_paused.load()) ? NUE_FALSE : NUE_TRUE);
    uint64_t want = uint64_t(latency_ms) * 1000000ull;
    uint64_t got = s->device_latency_ns;
    return (got > want * 2 || got * 2 < want) ? NUE_OK_PARTIAL : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_set_volume(nue_audio_stream s, float volume) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    s->volume = volume < 0 ? 0 : volume > 4 ? 4 : volume;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_get_stats(nue_audio_stream s, nue_audio_stream_stats* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    nue_audio_stream_stats v;
    NUE_STRUCT_INIT(&v);
    v.frames_played = s->frames_played.load();
    v.underruns = s->underruns.load();
    v.dropped_frames = s->dropped.load();
    v.peak_level = s->peak.load();
    v.cpu_load = s->cpu_load.load();
    uint32_t keep = out->struct_size;
    std::memcpy(out, &v, keep < sizeof(v) ? keep : sizeof(v));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_audio_stream_get_spec(nue_audio_stream s, nue_audio_spec* app_spec, nue_audio_spec* device_spec) {
    if (!known_stream(s)) return NUE_ERR_INVALID_HANDLE;
    if (app_spec) *app_spec = s->app_spec;
    if (device_spec) *device_spec = s->dev_spec;
    return NUE_OK;
}

}  // extern "C"
