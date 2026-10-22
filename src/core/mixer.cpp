// Software mixer: voices (static / ring / callback), buses with effects, listener + 3D, deterministic block rendering in float32.
// Locking model: one short mutex per mixer. API calls take it for the few instructions they need; render() takes it per internal block
// (block_frames), so commands take effect at block boundaries and a block is never mixed with half-applied state. Nothing allocates while
// mixing (scratch buffers are sized at creation). This is a mutex, not a lock-free queue: see docs/STATUS.md.
#include "internal/audio_convert.h"
#include "internal/core_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- slot map with generation-checked 32-bit ids ---------------------------------------------------------------------------------------
template <class T>
struct SlotMap {
    std::vector<std::shared_ptr<T>> items;
    std::vector<uint16_t> gens;
    std::vector<uint32_t> free_list;
    size_t live = 0;

    uint32_t add(std::shared_ptr<T> v) {
        uint32_t slot;
        if (!free_list.empty()) { slot = free_list.back(); free_list.pop_back(); }
        else { slot = uint32_t(items.size()); items.emplace_back(); gens.push_back(0); }
        uint16_t g = uint16_t(gens[slot] + 1);
        if (g == 0) g = 1;
        gens[slot] = g;
        items[slot] = std::move(v);
        ++live;
        return (uint32_t(g) << 16) | (slot + 1);
    }
    T* get(uint32_t id) const {
        uint32_t idx = id & 0xFFFFu;
        if (idx == 0 || idx > items.size() || gens[idx - 1] != (id >> 16)) return nullptr;
        return items[idx - 1].get();
    }
    std::shared_ptr<T> share(uint32_t id) const {
        uint32_t idx = id & 0xFFFFu;
        if (idx == 0 || idx > items.size() || gens[idx - 1] != (id >> 16)) return nullptr;
        return items[idx - 1];
    }
    bool remove(uint32_t id) {
        uint32_t idx = id & 0xFFFFu;
        if (!get(id)) return false;
        items[idx - 1].reset();
        gens[idx - 1] = uint16_t(gens[idx - 1] + 1 == 0 ? 1 : gens[idx - 1] + 1);
        free_list.push_back(idx - 1);
        --live;
        return true;
    }
    uint32_t id_of_slot(uint32_t slot) const { return (uint32_t(gens[slot]) << 16) | (slot + 1); }
};

struct Buffer {
    nue_audio_spec spec;
    std::vector<float> data;      // float32, spec.channels interleaved
    uint32_t frames = 0, loop_start = 0, loop_end = 0;
};

// ---- effects ----------------------------------------------------------------------------------------------------------------------------
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[NUE_AUDIO_MAX_CHANNELS] = {}, z2[NUE_AUDIO_MAX_CHANNELS] = {};
    void design(bool lowpass, float cutoff, float q, float rate) {
        cutoff = std::min(std::max(cutoff, 10.0f), rate * 0.49f);
        if (q <= 0) q = 0.7071f;
        float w0 = 2.0f * kPi * cutoff / rate, c = std::cos(w0), s = std::sin(w0), alpha = s / (2.0f * q);
        float a0 = 1.0f + alpha;
        if (lowpass) { b0 = (1.0f - c) * 0.5f / a0; b1 = (1.0f - c) / a0; b2 = b0; }
        else { b0 = (1.0f + c) * 0.5f / a0; b1 = -(1.0f + c) / a0; b2 = b0; }
        a1 = -2.0f * c / a0;
        a2 = (1.0f - alpha) / a0;
    }
    void process(float* x, uint32_t frames, uint32_t ch) {
        for (uint32_t i = 0; i < frames; ++i)
            for (uint32_t c = 0; c < ch; ++c) {
                float in = x[i * ch + c];
                float out = b0 * in + z1[c];
                z1[c] = b1 * in - a1 * out + z2[c];
                z2[c] = b2 * in - a2 * out;
                x[i * ch + c] = out;
            }
    }
};

struct DelayLine {
    std::vector<float> buf;
    size_t pos = 0;
    void resize(size_t n) { buf.assign(std::max<size_t>(n, 1), 0.0f); pos = 0; }
};

struct Effect {
    nue_effect_desc desc;
    bool active = false;
    Biquad bq;
    DelayLine echo[NUE_AUDIO_MAX_CHANNELS];
    DelayLine comb[NUE_AUDIO_MAX_CHANNELS][4], ap[NUE_AUDIO_MAX_CHANNELS][2];   // reverb: 4 combs + 2 allpasses per channel
    float comb_lp[NUE_AUDIO_MAX_CHANNELS][4] = {};
    float env = 0;   // compressor envelope
};

struct Bus {
    nue_bus_id parent = 0;
    float volume = 1.0f;
    bool mute = false;
    std::vector<float> buf;
    Effect fx[NUE_BUS_MAX_EFFECTS];
};

// ---- voices -----------------------------------------------------------------------------------------------------------------------------
struct Voice {
    nue_voice_desc desc;
    nue_voice_kind kind = NUE_VOICE_STATIC;
    nue_voice_flags flags = 0;
    nue_bus_id bus = 0;
    int32_t priority = 0;
    uint64_t serial = 0;
    std::shared_ptr<Buffer> buffer;          // STATIC (shared with the buffer table: destroying the buffer does not cut playing voices)
    nue_audio_spec spec;                     // source format
    std::vector<float> ring;                 // RING
    uint32_t ring_frames = 0;
    nuei::Resampler pull_resampler;          // CALLBACK
    std::vector<uint8_t> pull_raw;
    std::vector<float> pull_float;
    bool pull_done = false;
    bool playing = false, paused = false, looping = false;
    double pos = 0;                          // source frames
    uint64_t played_total = 0;
    float volume = 1.0f, pan = 0.0f, pitch = 1.0f;
    uint32_t freq_hz = 0;
    nue_voice_3d p3d;
    float last_gain_l = -1, last_gain_r = -1;  // ramping state; < 0 = not initialised
    float effective_gain = 0;
    std::vector<uint32_t> notify_pos;        // RING notification positions
    nue_ring_notify_fn notify = nullptr;
    void* notify_ud = nullptr;
    std::vector<float> scratch;              // source-channel samples for one block
};

}  // namespace

struct nue_mixer_s {
    std::mutex mutex;
    nue_mixer_desc desc;
    uint32_t rate = 48000, channels = 2, block = 256, max_voices = 128;
    bool manual = false;
    nue_audio_stream stream = nullptr;
    SlotMap<Voice> voices;
    SlotMap<Buffer> buffers;
    std::vector<std::unique_ptr<Bus>> buses;   // index 0 = master; destroyed buses leave nullptr
    float master = 1.0f;
    nue_listener listener;
    nue_world_3d world;
    nue_mixer_stats stats;
    uint64_t serial = 0;
    float cpu = 0;
    std::vector<float> fx_tmp;
    struct Pending { nue_ring_notify_fn fn; void* ud; nue_voice_id voice; uint32_t pos; };
    std::vector<Pending> pending;   // ring notifications collected during a block, fired after the lock is released
};

namespace {

struct MixerRegistry {
    std::mutex mutex;
    std::vector<nue_mixer> all;
};
MixerRegistry& MR() { static MixerRegistry* r = new MixerRegistry(); return *r; }

bool known(nue_mixer m) { MixerRegistry& r = MR(); std::lock_guard<std::mutex> lock(r.mutex); return std::find(r.all.begin(), r.all.end(), m) != r.all.end(); }

float dot(const nue_vec3& a, const nue_vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
nue_vec3 sub(const nue_vec3& a, const nue_vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
nue_vec3 cross(const nue_vec3& a, const nue_vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(const nue_vec3& a) { return std::sqrt(dot(a, a)); }

// -- effect processing -------------------------------------------------------------------------------------------------------------------
void effect_setup(Effect& e, const nue_effect_desc& d, uint32_t channels, uint32_t rate) {
    e.desc = d;
    e.active = d.kind != NUE_EFFECT_NONE;
    if (e.desc.wet_dry <= 0.0f) e.desc.wet_dry = 1.0f;
    e.bq = Biquad();
    e.env = 0;
    switch (d.kind) {
        case NUE_EFFECT_LOWPASS: e.bq.design(true, d.params[0] > 0 ? d.params[0] : 1000.0f, d.params[1], float(rate)); break;
        case NUE_EFFECT_HIGHPASS: e.bq.design(false, d.params[0] > 0 ? d.params[0] : 200.0f, d.params[1], float(rate)); break;
        case NUE_EFFECT_ECHO: {
            size_t n = size_t(std::max(1.0f, d.params[0]) * float(rate) / 1000.0f);
            for (uint32_t c = 0; c < channels; ++c) e.echo[c].resize(n);
            break;
        }
        case NUE_EFFECT_REVERB: {
            static const int comb_len[4] = {1116, 1188, 1277, 1356}, ap_len[2] = {556, 441};   // Freeverb tunings at 44.1 kHz
            for (uint32_t c = 0; c < channels; ++c) {
                for (int i = 0; i < 4; ++i) { e.comb[c][i].resize(size_t(comb_len[i] + int(c) * 23) * rate / 44100); e.comb_lp[c][i] = 0; }
                for (int i = 0; i < 2; ++i) e.ap[c][i].resize(size_t(ap_len[i] + int(c) * 11) * rate / 44100);
            }
            break;
        }
        default: break;
    }
}

void effect_process(Effect& e, float* x, uint32_t frames, uint32_t ch, uint32_t rate, std::vector<float>& tmp) {
    if (!e.active) return;
    const float wet = e.desc.wet_dry;
    const size_t n = size_t(frames) * ch;
    if (wet < 1.0f) tmp.assign(x, x + n);
    switch (e.desc.kind) {
        case NUE_EFFECT_LOWPASS: case NUE_EFFECT_HIGHPASS: e.bq.process(x, frames, ch); break;
        case NUE_EFFECT_ECHO: {
            float fb = std::min(std::max(e.desc.params[1], 0.0f), 0.95f), mixw = std::min(std::max(e.desc.params[2], 0.0f), 1.0f);
            for (uint32_t i = 0; i < frames; ++i)
                for (uint32_t c = 0; c < ch; ++c) {
                    DelayLine& d = e.echo[c];
                    float delayed = d.buf[d.pos];
                    float in = x[i * ch + c];
                    d.buf[d.pos] = in + delayed * fb;
                    d.pos = (d.pos + 1) % d.buf.size();
                    x[i * ch + c] = in + delayed * mixw;
                }
            break;
        }
        case NUE_EFFECT_REVERB: {
            float room = std::min(std::max(e.desc.params[0], 0.0f), 1.0f), damp = std::min(std::max(e.desc.params[1], 0.0f), 1.0f);
            float fb = 0.7f + 0.28f * room, rw = std::min(std::max(e.desc.params[2], 0.0f), 1.0f);
            for (uint32_t i = 0; i < frames; ++i)
                for (uint32_t c = 0; c < ch; ++c) {
                    float in = x[i * ch + c] * 0.15f, sum = 0;
                    for (int k = 0; k < 4; ++k) {
                        DelayLine& d = e.comb[c][k];
                        float y = d.buf[d.pos];
                        e.comb_lp[c][k] = y * (1.0f - damp) + e.comb_lp[c][k] * damp;
                        d.buf[d.pos] = in + e.comb_lp[c][k] * fb;
                        d.pos = (d.pos + 1) % d.buf.size();
                        sum += y;
                    }
                    for (int k = 0; k < 2; ++k) {
                        DelayLine& d = e.ap[c][k];
                        float b = d.buf[d.pos], y = -sum + b;
                        d.buf[d.pos] = sum + b * 0.5f;
                        d.pos = (d.pos + 1) % d.buf.size();
                        sum = y;
                    }
                    x[i * ch + c] = x[i * ch + c] * (1.0f - rw) + sum * rw;
                }
            break;
        }
        case NUE_EFFECT_COMPRESSOR: {
            float thr = std::pow(10.0f, e.desc.params[0] / 20.0f), ratio = std::max(e.desc.params[1], 1.0f);
            float atk = std::exp(-1.0f / (std::max(e.desc.params[2], 0.1f) * 0.001f * float(rate)));
            float rel = std::exp(-1.0f / (std::max(e.desc.params[3], 1.0f) * 0.001f * float(rate)));
            float makeup = std::pow(10.0f, e.desc.params[4] / 20.0f);
            for (uint32_t i = 0; i < frames; ++i) {
                float peak = 0;
                for (uint32_t c = 0; c < ch; ++c) peak = std::max(peak, std::fabs(x[i * ch + c]));
                e.env = peak > e.env ? atk * e.env + (1 - atk) * peak : rel * e.env + (1 - rel) * peak;
                float gain = e.env > thr ? std::pow(e.env / thr, 1.0f / ratio - 1.0f) : 1.0f;
                for (uint32_t c = 0; c < ch; ++c) x[i * ch + c] *= gain * makeup;
            }
            break;
        }
        case NUE_EFFECT_CALLBACK:
            if (e.desc.callback) e.desc.callback(e.desc.userdata, x, frames, ch, rate);
            break;
        default: break;
    }
    if (wet < 1.0f)
        for (size_t i = 0; i < n; ++i) x[i] = tmp[i] * (1.0f - wet) + x[i] * wet;
}

// -- voice rendering ---------------------------------------------------------------------------------------------------------------------
// Produces up to `n` frames of the voice at the mixer rate into v.scratch (source channels); returns frames produced (< n when the source ended).
uint32_t voice_read(Voice& v, uint32_t n, double step) {
    const uint32_t sc = v.spec.channels;
    if (v.scratch.size() < size_t(n) * sc) v.scratch.resize(size_t(n) * sc);
    float* out = v.scratch.data();
    uint32_t made = 0;
    if (v.kind == NUE_VOICE_STATIC) {
        const Buffer& b = *v.buffer;
        const bool loop = v.looping;
        const bool has_range = b.loop_end > b.loop_start && b.loop_end <= b.frames;
        const uint32_t ls = has_range ? b.loop_start : 0;
        const uint32_t le = has_range ? b.loop_end : b.frames;
        for (; made < n; ++made) {
            uint32_t i = uint32_t(v.pos);
            if (i >= b.frames) break;
            float f = float(v.pos - double(i));
            uint32_t j = i + 1;
            if (loop && j >= le) j = ls;
            else if (j >= b.frames) j = i;
            for (uint32_t c = 0; c < sc; ++c) {
                float a = b.data[size_t(i) * sc + c], bb = b.data[size_t(j) * sc + c];
                out[size_t(made) * sc + c] = a + (bb - a) * f;
            }
            v.pos += step;
            if (loop && v.pos >= le) v.pos = ls + std::fmod(v.pos - ls, double(le - ls));
        }
        v.played_total += uint64_t(double(made) * step);
    } else if (v.kind == NUE_VOICE_RING) {
        const uint32_t rf = v.ring_frames;
        for (; made < n; ++made) {
            uint32_t i = uint32_t(v.pos) % rf;
            float f = float(v.pos - std::floor(v.pos));
            uint32_t j = (i + 1) % rf;
            for (uint32_t c = 0; c < sc; ++c) {
                float a = v.ring[size_t(i) * sc + c], bb = v.ring[size_t(j) * sc + c];
                out[size_t(made) * sc + c] = a + (bb - a) * f;
            }
            v.pos += step;
            if (v.pos >= double(rf)) v.pos -= double(rf);
        }
        v.played_total += uint64_t(double(n) * step);
    } else {   // CALLBACK
        uint32_t need = v.pull_resampler.input_needed(n, step);
        while (need > 0 && !v.pull_done) {
            uint32_t chunk = std::min<uint32_t>(need, 4096);
            const uint32_t fsz = nue_audio_frame_size(&v.spec);
            if (v.pull_raw.size() < size_t(chunk) * fsz) v.pull_raw.resize(size_t(chunk) * fsz);
            if (v.pull_float.size() < size_t(chunk) * sc) v.pull_float.resize(size_t(chunk) * sc);
            uint32_t got = v.desc.pull ? v.desc.pull(v.desc.userdata, v.pull_raw.data(), chunk) : 0;
            if (got > chunk) got = chunk;
            if (got) { nuei::to_float(v.spec, v.pull_raw.data(), got, v.pull_float.data()); v.pull_resampler.push(v.pull_float.data(), got); }
            if (got < chunk) v.pull_done = true;
            need -= chunk;
        }
        made = v.pull_resampler.produce(n, step, out);
        v.pos += double(made) * step;
        v.played_total += uint64_t(double(made) * step);
    }
    return made;
}

void render_block(nue_mixer m, float* out, uint32_t frames) {
    const uint32_t M = m->channels;
    std::vector<nue_voice_id> ended;
    for (auto& b : m->buses) if (b) std::fill(b->buf.begin(), b->buf.begin() + size_t(frames) * M, 0.0f);

    uint32_t active = 0;
    nue_vec3 right = cross(m->listener.up, m->listener.forward);   // left-handed like Direct3D/DirectSound: +x right, +y up, +z forward
    float rl = length(right);
    if (rl > 1e-6f) { right.x /= rl; right.y /= rl; right.z /= rl; }

    for (uint32_t slot = 0; slot < m->voices.items.size(); ++slot) {
        Voice* vp = m->voices.items[slot].get();
        if (!vp || !vp->playing || vp->paused) continue;
        Voice& v = *vp;
        ++active;
        const uint32_t sc = v.spec.channels;

        // pitch -> step in source frames per output frame
        const bool fixed_pitch = (v.flags & NUE_VOICE_NO_PITCH) != 0;
        double rate_ratio = (v.freq_hz && !fixed_pitch) ? double(v.freq_hz) / double(m->rate)
                                                         : double(v.spec.sample_rate) * (fixed_pitch ? 1.0 : double(v.pitch)) / double(m->rate);
        // 3D: attenuation, pan, cone, doppler
        float g3 = 1.0f, pan3 = 0.0f;
        const bool is3d = (v.flags & NUE_VOICE_3D) != 0;
        if (is3d) {
            const bool head = (v.flags & NUE_VOICE_HEAD_RELATIVE) != 0;
            nue_vec3 rel = head ? v.p3d.position : sub(v.p3d.position, m->listener.position);
            float d = length(rel);
            nue_vec3 rgt = head ? nue_vec3{1, 0, 0} : right;
            float mind = std::max(v.p3d.min_distance, 1e-3f), maxd = std::max(v.p3d.max_distance, mind);
            float dd = std::min(std::max(d, mind), maxd);
            float roll = v.p3d.rolloff * m->world.rolloff_factor;
            switch (v.p3d.model) {
                case NUE_ATTEN_INVERSE: g3 = mind / (mind + roll * (dd - mind)); break;
                case NUE_ATTEN_LINEAR: g3 = maxd > mind ? 1.0f - roll * (dd - mind) / (maxd - mind) : 1.0f; break;
                case NUE_ATTEN_EXPONENTIAL: g3 = std::pow(dd / mind, -roll); break;
                default: g3 = 1.0f; break;
            }
            g3 = std::min(std::max(g3, 0.0f), 1.0f);
            if (d > 1e-6f) pan3 = std::min(std::max(dot(rel, rgt) / d, -1.0f), 1.0f);
            nue_vec3 cd = v.p3d.cone_direction;
            if (length(cd) > 1e-6f && d > 1e-6f && v.p3d.cone_outer_angle_deg > v.p3d.cone_inner_angle_deg) {
                nue_vec3 to_l = {-rel.x / d, -rel.y / d, -rel.z / d};
                float cl = length(cd);
                float ang = std::acos(std::min(std::max(dot({cd.x / cl, cd.y / cl, cd.z / cl}, to_l), -1.0f), 1.0f)) * 180.0f / kPi * 2.0f;   // full cone angle
                float inner = v.p3d.cone_inner_angle_deg, outer = v.p3d.cone_outer_angle_deg;
                if (ang > outer) g3 *= v.p3d.cone_outer_gain;
                else if (ang > inner) g3 *= 1.0f + (v.p3d.cone_outer_gain - 1.0f) * ((ang - inner) / (outer - inner));
            }
            if (m->world.doppler_factor > 0 && d > 1e-6f) {
                nue_vec3 u = {-rel.x / d, -rel.y / d, -rel.z / d};   // source -> listener
                float vls = dot(u, m->listener.velocity), vss = dot(u, v.p3d.velocity);
                float c = std::max(m->world.speed_of_sound, 1.0f);
                float num = c - m->world.doppler_factor * vls, den = c - m->world.doppler_factor * vss;
                if (num > 1e-3f && den > 1e-3f) rate_ratio *= double(num / den);
            }
        }
        double step = std::max(rate_ratio, 1e-4);

        const double pos_before = v.pos;
        const uint32_t made = voice_read(v, frames, step);
        if (v.kind == NUE_VOICE_RING && v.notify && !v.notify_pos.empty()) {
            const double rf = double(v.ring_frames);
            double travelled = double(frames) * step;
            for (uint32_t p : v.notify_pos) {
                double d = double(p) - pos_before;
                if (d <= 0) d += rf;   // distance ahead of the old play cursor, in (0, rf]
                if (d <= travelled) m->pending.push_back({v.notify, v.notify_ud, m->voices.id_of_slot(slot), p});
            }
        }

        // pan and gain: ramp from the previous block's gain so parameter changes never click
        float pan = std::min(std::max(v.pan + pan3, -1.0f), 1.0f);
        float gain = v.volume * g3;
        v.effective_gain = gain;
        Bus* bus = (v.bus < m->buses.size() && m->buses[v.bus]) ? m->buses[v.bus].get() : m->buses[0].get();
        float* dst = bus->buf.data();
        const bool mono_pan = sc == 1 || (is3d && sc > 1);
        float tl, tr;
        if (mono_pan) { float th = (pan + 1.0f) * kPi * 0.25f; tl = gain * std::cos(th); tr = gain * std::sin(th); }   // constant power
        else { tl = gain * (pan > 0 ? 1.0f - pan : 1.0f); tr = gain * (pan < 0 ? 1.0f + pan : 1.0f); }               // balance
        if (v.last_gain_l < 0) { v.last_gain_l = tl; v.last_gain_r = tr; }
        const float gl0 = v.last_gain_l, gr0 = v.last_gain_r;
        const float* src = v.scratch.data();
        for (uint32_t i = 0; i < made; ++i) {
            float t = float(i + 1) / float(frames);
            float gl = gl0 + (tl - gl0) * t, gr = gr0 + (tr - gr0) * t;
            const float* s = src + size_t(i) * sc;
            float* o = dst + size_t(i) * M;
            if (mono_pan) {
                float x = s[0];
                if (sc > 1) { x = 0; for (uint32_t c = 0; c < sc; ++c) x += s[c]; x /= float(sc); }
                if (M >= 2) { o[0] += x * gl; o[1] += x * gr; } else o[0] += x * std::sqrt(gl * gl + gr * gr);   // one output channel: pan has no meaning, keep the power
            } else if (M == sc) {
                for (uint32_t c = 0; c < sc; ++c) o[c] += s[c] * (c == 0 ? gl : c == 1 ? gr : gain);
            } else {
                float tmp[NUE_AUDIO_MAX_CHANNELS];
                nuei::remap_channels(s, sc, 1, tmp, M);
                for (uint32_t c = 0; c < M; ++c) o[c] += tmp[c] * (c == 0 ? gl : c == 1 ? gr : gain);
            }
        }
        v.last_gain_l = tl; v.last_gain_r = tr;

        bool finished = v.kind != NUE_VOICE_RING && made < frames;
        if (v.kind == NUE_VOICE_STATIC && !v.looping && v.pos >= double(v.buffer->frames)) finished = true;
        if (finished) {
            nue_voice_end_callback cb = v.desc.on_end;
            void* ud = v.desc.userdata;
            nue_voice_id id = m->voices.id_of_slot(slot);
            v.playing = false;
            if (v.kind == NUE_VOICE_STATIC) v.pos = 0;
            v.last_gain_l = v.last_gain_r = -1;
            if (cb) cb(ud, id);
            if (v.flags & NUE_VOICE_AUTO_RELEASE) ended.push_back(id);
        }
    }
    for (nue_voice_id id : ended) m->voices.remove(id);

    // buses: children before parents (deepest first), then master
    std::vector<int> order;
    for (size_t i = 1; i < m->buses.size(); ++i) if (m->buses[i]) order.push_back(int(i));
    auto depth = [&](int i) { int d = 0; nue_bus_id p = m->buses[size_t(i)]->parent; while (p && p < m->buses.size() && m->buses[p] && d < 16) { ++d; p = m->buses[p]->parent; } return d; };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth(a) > depth(b); });
    order.push_back(0);
    for (int idx : order) {
        Bus& b = *m->buses[size_t(idx)];
        float* x = b.buf.data();
        for (auto& e : b.fx) effect_process(e, x, frames, M, m->rate, m->fx_tmp);
        float vol = b.mute ? 0.0f : b.volume;
        if (idx == 0) vol *= m->master;
        const size_t n = size_t(frames) * M;
        if (idx == 0) {
            for (size_t i = 0; i < n; ++i) out[i] = x[i] * vol;
        } else {
            Bus* parent = (b.parent < m->buses.size() && m->buses[b.parent]) ? m->buses[b.parent].get() : m->buses[0].get();
            float* p = parent->buf.data();
            for (size_t i = 0; i < n; ++i) p[i] += x[i] * vol;
        }
    }
    m->stats.active_voices = active;
    ++m->stats.blocks_mixed;
    for (uint32_t c = 0; c < M; ++c) m->stats.peak[c] = 0;
    for (uint32_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < M; ++c) {
            float a = std::fabs(out[size_t(i) * M + c]);
            if (a > m->stats.peak[c]) m->stats.peak[c] = a;
            if (a > 1.0f) ++m->stats.clipped_samples;
        }
}

// Callback of the output stream: the mixer renders straight into the device buffer.
void NUE_CALL stream_fill(void* userdata, void* out, uint32_t frames) { nue_mixer_render(static_cast<nue_mixer>(userdata), static_cast<float*>(out), frames); }

}  // namespace

namespace nuei {
void mixer_shutdown_all() {
    std::vector<nue_mixer> left;
    { MixerRegistry& r = MR(); std::lock_guard<std::mutex> lock(r.mutex); left = r.all; }
    if (!left.empty()) NUE_LOGW("mixer", "%zu mixer(s) were still open at shutdown and are being destroyed", left.size());
    for (nue_mixer m : left) nue_mixer_destroy(m);
}
}  // namespace nuei

#define MIXER_OR_RETURN(m) \
    if (!known(m)) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid mixer handle")
#define LOCK(m) std::lock_guard<std::mutex> lock_((m)->mutex)
#define VOICE_OR_RETURN(m, id, v) \
    MIXER_OR_RETURN(m); LOCK(m); Voice* v = (m)->voices.get(id); \
    if (!v) return NUE_ERR_INVALID_HANDLE
#define BUS_OR_RETURN(m, bus) \
    if ((bus) >= (m)->buses.size() || !(m)->buses[bus]) return NUE_ERR_INVALID_HANDLE

extern "C" {

// ---- unit helpers ------------------------------------------------------------------------------------------------------------------------

NUE_API float NUE_CALL nue_db_to_linear(float db) { return std::pow(10.0f, db / 20.0f); }
NUE_API float NUE_CALL nue_linear_to_db(float linear) { return linear <= 1e-10f ? -200.0f : 20.0f * std::log10(linear); }

NUE_API float NUE_CALL nue_centibels_to_linear(int32_t cb) {
    if (cb >= 0) return 1.0f;
    if (cb <= -10000) return 0.0f;   // DirectSound: -10000 is silence
    return std::pow(10.0f, float(cb) / 2000.0f);
}

NUE_API int32_t NUE_CALL nue_linear_to_centibels(float linear) {
    if (linear >= 1.0f) return 0;
    if (linear <= 1e-5f) return -10000;
    int32_t cb = int32_t(std::lround(2000.0f * std::log10(linear)));
    return cb < -10000 ? -10000 : cb;
}

NUE_API float NUE_CALL nue_pan_from_centibels(int32_t pan) {
    // DirectSound pan: 0 = centre; positive attenuates the left channel by `pan` centibels, negative the right one.
    if (pan > 10000) pan = 10000;
    if (pan < -10000) pan = -10000;
    float atten = nue_centibels_to_linear(-(pan < 0 ? -pan : pan));
    float side = 1.0f - atten;
    return pan >= 0 ? side : -side;
}

// ---- mixer -------------------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_mixer_create(const nue_mixer_desc* desc, nue_mixer* out) {
    if (!out) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_mixer_create: out is required");
    *out = nullptr;
    nue_mixer_desc d;
    NUE_STRUCT_INIT(&d);
    if (desc) std::memcpy(&d, desc, desc->struct_size < sizeof(d) ? desc->struct_size : sizeof(d));
    d.struct_size = sizeof(d);
    nue_mixer m = new (std::nothrow) nue_mixer_s();
    if (!m) return NUE_ERR_OUT_OF_MEMORY;
    m->desc = d;
    m->manual = (d.flags & NUE_MIXER_MANUAL) != 0;
    m->channels = d.channels ? d.channels : 2;
    m->block = d.block_frames ? d.block_frames : 256;
    m->max_voices = d.max_voices ? d.max_voices : 128;
    if (m->channels > NUE_AUDIO_MAX_CHANNELS || m->block > 16384 || m->max_voices > 4096) { delete m; return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "mixer parameters out of range"); }
    m->rate = d.sample_rate;
    if (!m->rate) {
        m->rate = 48000;
        if (!m->manual && nuei::initialized()) {
            nue_audio_device_info info;
            NUE_STRUCT_INIT(&info);
            if (NUE_SUCCEEDED(nue_audio_get_device_info(d.device, &info)) && info.preferred.sample_rate) m->rate = info.preferred.sample_rate;
        }
    }
    NUE_STRUCT_INIT(&m->listener);
    m->listener.forward = {0, 0, 1};
    m->listener.up = {0, 1, 0};
    NUE_STRUCT_INIT(&m->world);
    m->world.doppler_factor = 1.0f; m->world.speed_of_sound = 343.0f; m->world.rolloff_factor = 1.0f; m->world.distance_factor = 1.0f;
    NUE_STRUCT_INIT(&m->stats);
    m->buses.emplace_back(new Bus());
    m->buses[0]->buf.assign(size_t(m->block) * m->channels, 0.0f);
    m->fx_tmp.reserve(size_t(m->block) * m->channels);
    { MixerRegistry& r = MR(); std::lock_guard<std::mutex> lock(r.mutex); r.all.push_back(m); }
    if (!m->manual) {
        nue_audio_stream_desc sd;
        NUE_STRUCT_INIT(&sd);
        sd.spec = {NUE_AUDIO_F32, m->channels, m->rate};
        sd.callback = stream_fill;
        sd.userdata = m;
        sd.latency_ms = d.latency_ms;
        sd.device = d.device;
        sd.flags = (d.flags & NUE_MIXER_START_PAUSED) ? NUE_AUDIO_START_PAUSED : 0;
        sd.name = "nue mixer";
        nue_result r = nue_audio_stream_create(&sd, &m->stream);
        if (NUE_FAILED(r)) { nue_mixer_destroy(m); return r; }
    }
    *out = m;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_mixer_destroy(nue_mixer m) {
    if (!known(m)) return;
    if (m->stream) { nue_audio_stream_destroy(m->stream); m->stream = nullptr; }   // returns when the callback is gone: no render is running afterwards
    { MixerRegistry& r = MR(); std::lock_guard<std::mutex> lock(r.mutex); r.all.erase(std::find(r.all.begin(), r.all.end(), m)); }
    delete m;
}

NUE_API nue_result NUE_CALL nue_mixer_get_spec(nue_mixer m, nue_audio_spec* out) {
    MIXER_OR_RETURN(m);
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    *out = {NUE_AUDIO_F32, m->channels, m->rate};
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_mixer_start(nue_mixer m) { MIXER_OR_RETURN(m); return m->stream ? nue_audio_stream_start(m->stream) : NUE_OK_NOOP; }
NUE_API nue_result NUE_CALL nue_mixer_stop(nue_mixer m) { MIXER_OR_RETURN(m); return m->stream ? nue_audio_stream_stop(m->stream) : NUE_OK_NOOP; }
NUE_API nue_audio_stream NUE_CALL nue_mixer_get_stream(nue_mixer m) { return known(m) ? m->stream : nullptr; }

NUE_API nue_result NUE_CALL nue_mixer_render(nue_mixer m, float* out, uint32_t frame_count) {
    // Hot path (called from the audio thread for attached mixers): the registry lookup is skipped when the call comes from our own stream.
    if (!m) return NUE_ERR_INVALID_HANDLE;
    if (!out && frame_count) return NUE_ERR_INVALID_ARGUMENT;
    const int64_t t0 = nuei::now_ns();
    uint32_t done = 0;
    while (done < frame_count) {
        uint32_t n = std::min(m->block, frame_count - done);
        LOCK(m);
        render_block(m, out + size_t(done) * m->channels, n);
        done += n;
    }
    std::vector<nue_mixer_s::Pending> fire;
    { LOCK(m); fire.swap(m->pending); }
    for (const auto& p : fire) p.fn(p.ud, p.voice, p.pos);
    const double budget = double(frame_count) * 1e9 / double(m->rate);
    if (budget > 0) { LOCK(m); m->cpu = m->cpu * 0.9f + float(double(nuei::now_ns() - t0) / budget) * 0.1f; m->stats.cpu_load = m->cpu; }
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_mixer_set_master_volume(nue_mixer m, float v) { MIXER_OR_RETURN(m); LOCK(m); m->master = v < 0 ? 0 : v; return NUE_OK; }
NUE_API float NUE_CALL nue_mixer_get_master_volume(nue_mixer m) { if (!known(m)) return 0; LOCK(m); return m->master; }

NUE_API nue_result NUE_CALL nue_mixer_get_stats(nue_mixer m, nue_mixer_stats* out) {
    MIXER_OR_RETURN(m);
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    LOCK(m);
    uint32_t keep = out->struct_size;
    std::memcpy(out, &m->stats, keep < sizeof(m->stats) ? keep : sizeof(m->stats));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_mixer_reset_stats(nue_mixer m) {
    MIXER_OR_RETURN(m);
    LOCK(m);
    uint32_t active = m->stats.active_voices;
    NUE_STRUCT_INIT(&m->stats);
    m->stats.active_voices = active;
    return NUE_OK;
}

// ---- buses and effects -------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_bus_create(nue_mixer m, nue_bus_id parent, nue_bus_id* out) {
    MIXER_OR_RETURN(m);
    if (!out) return NUE_ERR_INVALID_ARGUMENT;
    LOCK(m);
    if (parent >= m->buses.size() || !m->buses[parent]) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid parent bus");
    std::unique_ptr<Bus> b(new Bus());
    b->parent = parent;
    b->buf.assign(size_t(m->block) * m->channels, 0.0f);
    for (size_t i = 1; i < m->buses.size(); ++i)
        if (!m->buses[i]) { m->buses[i] = std::move(b); *out = nue_bus_id(i); return NUE_OK; }
    m->buses.push_back(std::move(b));
    *out = nue_bus_id(m->buses.size() - 1);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_bus_destroy(nue_mixer m, nue_bus_id bus) {
    MIXER_OR_RETURN(m);
    LOCK(m);
    if (bus == 0 || bus >= m->buses.size() || !m->buses[bus]) return NUE_ERR_INVALID_HANDLE;
    nue_bus_id parent = m->buses[bus]->parent;
    for (auto& v : m->voices.items) if (v && v->bus == bus) v->bus = parent;
    for (size_t i = 1; i < m->buses.size(); ++i) if (m->buses[i] && m->buses[i]->parent == bus) m->buses[i]->parent = parent;
    m->buses[bus].reset();
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_bus_set_volume(nue_mixer m, nue_bus_id bus, float v) { MIXER_OR_RETURN(m); LOCK(m); BUS_OR_RETURN(m, bus); m->buses[bus]->volume = v < 0 ? 0 : v; return NUE_OK; }
NUE_API nue_result NUE_CALL nue_bus_set_mute(nue_mixer m, nue_bus_id bus, nue_bool mute) { MIXER_OR_RETURN(m); LOCK(m); BUS_OR_RETURN(m, bus); m->buses[bus]->mute = mute != 0; return NUE_OK; }

NUE_API nue_result NUE_CALL nue_bus_set_effect(nue_mixer m, nue_bus_id bus, uint32_t slot, const nue_effect_desc* effect) {
    MIXER_OR_RETURN(m);
    LOCK(m);
    BUS_OR_RETURN(m, bus);
    if (slot >= NUE_BUS_MAX_EFFECTS) return NUE_ERR_INVALID_ARGUMENT;
    Effect& e = m->buses[bus]->fx[slot];
    e = Effect();
    if (!effect || effect->kind == NUE_EFFECT_NONE) return NUE_OK;
    if (effect->kind == NUE_EFFECT_CALLBACK && !effect->callback) return NUE_ERR_INVALID_ARGUMENT;
    effect_setup(e, *effect, m->channels, m->rate);
    return NUE_OK;
}

// ---- buffers -----------------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_buffer_create(nue_mixer m, const nue_buffer_desc* desc, nue_buffer_id* out) {
    MIXER_OR_RETURN(m);
    if (!desc || !out || !desc->data) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_buffer_create: desc, data and out are required");
    const nue_audio_spec& sp = desc->spec;
    if (!nuei::bytes_per_sample(sp.format) || sp.channels < 1 || sp.channels > NUE_AUDIO_MAX_CHANNELS || sp.sample_rate < 1000 || sp.sample_rate > 768000 || desc->frame_count == 0)
        return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "invalid buffer format or size");
    std::shared_ptr<Buffer> b = std::make_shared<Buffer>();
    b->spec = sp;
    b->frames = desc->frame_count;
    b->data.resize(size_t(desc->frame_count) * sp.channels);
    nuei::to_float(sp, desc->data, desc->frame_count, b->data.data());   // decoded once; the source memory is not retained after this call
    b->loop_start = desc->loop_start;
    b->loop_end = desc->loop_end;
    LOCK(m);
    *out = m->buffers.add(std::move(b));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_buffer_destroy(nue_mixer m, nue_buffer_id id) {
    MIXER_OR_RETURN(m);
    LOCK(m);
    return m->buffers.remove(id) ? NUE_OK : NUE_ERR_INVALID_HANDLE;   // playing voices keep their own reference
}

// ---- voices ------------------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_voice_create(nue_mixer m, const nue_voice_desc* desc, nue_voice_id* out) {
    MIXER_OR_RETURN(m);
    if (!desc || !out) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "nue_voice_create: desc and out are required");
    *out = 0;
    std::shared_ptr<Voice> v = std::make_shared<Voice>();
    v->desc = *desc;
    v->kind = desc->kind;
    v->flags = desc->flags;
    v->bus = desc->bus;
    v->priority = desc->priority;
    v->volume = desc->volume != 0.0f ? desc->volume : 1.0f;
    v->pan = std::min(std::max(desc->pan, -1.0f), 1.0f);
    v->pitch = desc->pitch != 0.0f ? desc->pitch : 1.0f;
    NUE_STRUCT_INIT(&v->p3d);
    v->p3d.min_distance = 1.0f; v->p3d.max_distance = 1e9f; v->p3d.rolloff = 1.0f;
    v->looping = (desc->flags & NUE_VOICE_LOOP) != 0;
    LOCK(m);
    if (v->bus >= m->buses.size() || !m->buses[v->bus]) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid bus");
    if (desc->kind == NUE_VOICE_STATIC) {
        v->buffer = m->buffers.share(desc->buffer);
        if (!v->buffer) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid buffer id");
        v->spec = v->buffer->spec;
    } else {
        const nue_audio_spec& sp = desc->spec;
        if (!nuei::bytes_per_sample(sp.format) || sp.channels < 1 || sp.channels > NUE_AUDIO_MAX_CHANNELS || sp.sample_rate < 1000)
            return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "invalid voice source format");
        v->spec = sp;
        if (desc->kind == NUE_VOICE_RING) {
            if (desc->ring_frames < 2) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "ring_frames must be at least 2");
            v->ring_frames = desc->ring_frames;
            v->ring.assign(size_t(desc->ring_frames) * sp.channels, 0.0f);
            v->looping = true;
        } else if (desc->kind == NUE_VOICE_CALLBACK) {
            if (!desc->pull) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "callback voices need a pull function");
            v->pull_resampler.reset(sp.channels);
        } else return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "unknown voice kind");
    }
    v->scratch.assign(size_t(m->block) * v->spec.channels + 16, 0.0f);
    v->serial = ++m->serial;
    if (m->voices.live >= m->max_voices) {
        // make room: prefer a voice that is not playing; among equals the lowest priority, then the oldest
        uint32_t victim = UINT32_MAX;
        for (uint32_t s = 0; s < m->voices.items.size(); ++s) {
            Voice* c = m->voices.items[s].get();
            if (!c) continue;
            if (victim == UINT32_MAX) { victim = s; continue; }
            Voice* w = m->voices.items[victim].get();
            bool c_idle = !c->playing, w_idle = !w->playing;
            if (c_idle != w_idle) { if (c_idle) victim = s; continue; }
            if (c->priority < w->priority || (c->priority == w->priority && c->serial < w->serial)) victim = s;
        }
        Voice* w = victim == UINT32_MAX ? nullptr : m->voices.items[victim].get();
        if (!w || (w->playing && w->priority > v->priority)) return nuei::fail(NUE_ERR_OVERFLOW, "all %u voices are in use by higher priority sounds", m->max_voices);
        if (w->playing) ++m->stats.virtualized_voices;
        m->voices.remove(m->voices.id_of_slot(victim));
    }
    if (desc->flags & NUE_VOICE_START_PLAYING) v->playing = true;
    *out = m->voices.add(std::move(v));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_destroy(nue_mixer m, nue_voice_id id) { MIXER_OR_RETURN(m); LOCK(m); return m->voices.remove(id) ? NUE_OK : NUE_ERR_INVALID_HANDLE; }

NUE_API nue_result NUE_CALL nue_voice_play(nue_mixer m, nue_voice_id id) {
    VOICE_OR_RETURN(m, id, v);
    if (v->playing && !v->paused) return NUE_OK_NOOP;
    v->playing = true;
    v->paused = false;
    v->pull_done = false;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_stop(nue_mixer m, nue_voice_id id) {
    VOICE_OR_RETURN(m, id, v);
    v->playing = false;
    v->paused = false;
    if (v->kind == NUE_VOICE_STATIC) v->pos = 0;   // RING keeps its position (DirectSound Stop)
    v->last_gain_l = v->last_gain_r = -1;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_pause(nue_mixer m, nue_voice_id id, nue_bool paused) { VOICE_OR_RETURN(m, id, v); if (v->playing) v->paused = paused != 0; return NUE_OK; }
NUE_API nue_result NUE_CALL nue_voice_set_looping(nue_mixer m, nue_voice_id id, nue_bool l) { VOICE_OR_RETURN(m, id, v); if (v->kind != NUE_VOICE_RING) v->looping = l != 0; return NUE_OK; }
NUE_API nue_result NUE_CALL nue_voice_set_volume(nue_mixer m, nue_voice_id id, float x) { VOICE_OR_RETURN(m, id, v); v->volume = x < 0 ? 0 : x; return NUE_OK; }
NUE_API nue_result NUE_CALL nue_voice_set_pan(nue_mixer m, nue_voice_id id, float x) { VOICE_OR_RETURN(m, id, v); v->pan = std::min(std::max(x, -1.0f), 1.0f); return NUE_OK; }

NUE_API nue_result NUE_CALL nue_voice_set_pitch(nue_mixer m, nue_voice_id id, float ratio) {
    VOICE_OR_RETURN(m, id, v);
    if (ratio < 0.01f || ratio > 100.0f) return NUE_ERR_INVALID_ARGUMENT;
    v->pitch = ratio;
    v->freq_hz = 0;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_set_frequency(nue_mixer m, nue_voice_id id, uint32_t hz) {
    VOICE_OR_RETURN(m, id, v);
    if (hz && (hz < 100 || hz > 200000)) return NUE_ERR_INVALID_ARGUMENT;
    v->freq_hz = hz;
    if (!hz) v->pitch = 1.0f;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_set_bus(nue_mixer m, nue_voice_id id, nue_bus_id bus) {
    VOICE_OR_RETURN(m, id, v);
    if (bus >= m->buses.size() || !m->buses[bus]) return NUE_ERR_INVALID_HANDLE;
    v->bus = bus;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_set_position(nue_mixer m, nue_voice_id id, uint64_t frame) {
    VOICE_OR_RETURN(m, id, v);
    if (v->kind == NUE_VOICE_CALLBACK) return NUE_ERR_UNSUPPORTED;
    uint64_t limit = v->kind == NUE_VOICE_RING ? v->ring_frames : v->buffer->frames;
    if (frame >= limit) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "position beyond the end of the source");
    v->pos = double(frame);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_get_state(nue_mixer m, nue_voice_id id, nue_voice_state* out) {
    VOICE_OR_RETURN(m, id, v);
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    nue_voice_state s;
    std::memset(&s, 0, sizeof(s));
    s.struct_size = sizeof(s);
    s.playing = v->playing && !v->paused;
    s.paused = v->paused;
    s.looping = v->looping;
    s.position_frames = uint64_t(v->pos);
    s.frames_played_total = v->played_total;
    s.volume = v->volume; s.pan = v->pan; s.pitch = v->pitch;
    s.effective_gain = v->effective_gain;
    uint32_t keep = out->struct_size;
    std::memcpy(out, &s, keep < sizeof(s) ? keep : sizeof(s));
    out->struct_size = keep;
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_voice_is_playing(nue_mixer m, nue_voice_id id) {
    if (!known(m)) return NUE_FALSE;
    LOCK(m);
    Voice* v = m->voices.get(id);
    return v && v->playing && !v->paused ? NUE_TRUE : NUE_FALSE;
}

// ---- ring voices ---------------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_voice_ring_write(nue_mixer m, nue_voice_id id, uint32_t offset, const void* data, uint32_t frames) {
    MIXER_OR_RETURN(m);
    if (!data && frames) return NUE_ERR_INVALID_ARGUMENT;
    nue_audio_spec spec;
    uint32_t rf;
    {
        LOCK(m);
        Voice* v = m->voices.get(id);
        if (!v) return NUE_ERR_INVALID_HANDLE;
        if (v->kind != NUE_VOICE_RING) return nuei::fail(NUE_ERR_BAD_STATE, "not a ring voice");
        spec = v->spec; rf = v->ring_frames;
    }
    if (offset >= rf || frames > rf) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "ring write outside the ring (offset %u, %u frames, ring %u)", offset, frames, rf);
    std::vector<float> conv(size_t(frames) * spec.channels);
    nuei::to_float(spec, data, frames, conv.data());   // converted outside the lock
    LOCK(m);
    Voice* v = m->voices.get(id);
    if (!v) return NUE_ERR_INVALID_HANDLE;
    const uint32_t ch = spec.channels;
    for (uint32_t i = 0; i < frames; ++i) std::memcpy(&v->ring[size_t((offset + i) % rf) * ch], &conv[size_t(i) * ch], ch * sizeof(float));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_ring_fill_silence(nue_mixer m, nue_voice_id id, uint32_t offset, uint32_t frames) {
    VOICE_OR_RETURN(m, id, v);
    if (v->kind != NUE_VOICE_RING) return nuei::fail(NUE_ERR_BAD_STATE, "not a ring voice");
    if (offset >= v->ring_frames || frames > v->ring_frames) return NUE_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < frames; ++i) std::fill_n(&v->ring[size_t((offset + i) % v->ring_frames) * v->spec.channels], v->spec.channels, 0.0f);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_ring_get_cursors(nue_mixer m, nue_voice_id id, uint32_t* play, uint32_t* write) {
    VOICE_OR_RETURN(m, id, v);
    if (v->kind != NUE_VOICE_RING) return nuei::fail(NUE_ERR_BAD_STATE, "not a ring voice");
    uint32_t p = uint32_t(v->pos) % v->ring_frames;
    // safe write position: the block the mixer may already have mixed plus the device latency, in source frames at the current rate
    double lead_out = double(m->block);
    if (m->stream) { nue_audio_latency l; NUE_STRUCT_INIT(&l); if (NUE_SUCCEEDED(nue_audio_stream_get_latency(m->stream, &l))) lead_out += double(l.device_latency_ns) * m->rate / 1e9; }
    double ratio = v->freq_hz ? double(v->freq_hz) / double(m->rate) : double(v->spec.sample_rate) * v->pitch / double(m->rate);
    uint32_t lead = uint32_t(std::ceil(lead_out * ratio));
    if (play) *play = p;
    if (write) *write = (p + lead) % v->ring_frames;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_ring_set_notifications(nue_mixer m, nue_voice_id id, const uint32_t* positions, uint32_t count, nue_ring_notify_fn cb, void* ud) {
    VOICE_OR_RETURN(m, id, v);
    if (v->kind != NUE_VOICE_RING) return nuei::fail(NUE_ERR_BAD_STATE, "not a ring voice");
    if (count && (!positions || !cb)) return NUE_ERR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < count; ++i) if (positions[i] >= v->ring_frames) return nuei::fail(NUE_ERR_INVALID_ARGUMENT, "notification position %u outside the ring", positions[i]);
    v->notify_pos.assign(positions, positions + count);
    v->notify = count ? cb : nullptr;
    v->notify_ud = ud;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_ring_get_size(nue_mixer m, nue_voice_id id, uint32_t* out) {
    VOICE_OR_RETURN(m, id, v);
    if (v->kind != NUE_VOICE_RING) return nuei::fail(NUE_ERR_BAD_STATE, "not a ring voice");
    if (out) *out = v->ring_frames;
    return NUE_OK;
}

// ---- 3D ------------------------------------------------------------------------------------------------------------------------------------

NUE_API nue_result NUE_CALL nue_mixer_set_listener(nue_mixer m, const nue_listener* l) {
    MIXER_OR_RETURN(m);
    if (!l) return NUE_ERR_INVALID_ARGUMENT;
    LOCK(m);
    m->listener = *l;
    m->listener.struct_size = sizeof(nue_listener);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_mixer_set_world_3d(nue_mixer m, const nue_world_3d* w) {
    MIXER_OR_RETURN(m);
    if (!w) return NUE_ERR_INVALID_ARGUMENT;
    LOCK(m);
    m->world = *w;
    m->world.struct_size = sizeof(nue_world_3d);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_voice_set_3d(nue_mixer m, nue_voice_id id, const nue_voice_3d* p) {
    VOICE_OR_RETURN(m, id, v);
    if (!p) return NUE_ERR_INVALID_ARGUMENT;
    v->p3d = *p;
    v->p3d.struct_size = sizeof(nue_voice_3d);
    return NUE_OK;
}

}  // extern "C"
