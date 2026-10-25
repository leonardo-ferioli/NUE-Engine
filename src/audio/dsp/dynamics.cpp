// Dynamics: feed-forward compressor with a soft knee (gain computer after Giannoulis, Massberg and Reiss, "Digital Dynamic Range Compressor
// Design", JAES 2012), look-ahead brick-wall limiter (sliding minimum of the required gain, box-filtered over the look-ahead, so the
// ceiling holds by construction), and the dynamic range modes built from the two.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <vector>

using nuea::clampf;
using nuea::db_to_lin;
using nuea::lin_to_db;

namespace {

// ---------------------------------------------------------------------------------------------------------------- compressor

struct CompParams {
    float threshold_db, ratio, knee_db, attack_ms, release_ms, makeup_db, link;
    uint32_t flags;
};

CompParams sanitize(const nue_compressor_desc& d) {
    CompParams p;
    p.threshold_db = clampf(d.threshold_db, -80.0f, 0.0f);
    p.ratio = clampf(d.ratio, 1.0f, 100.0f);
    p.knee_db = clampf(d.knee_db, 0.0f, 40.0f);
    p.attack_ms = clampf(d.attack_ms, 0.05f, 1000.0f);
    p.release_ms = clampf(d.release_ms, 1.0f, 10000.0f);
    p.makeup_db = clampf(d.makeup_db, -24.0f, 48.0f);
    p.link = clampf(d.link, 0.0f, 1.0f);
    p.flags = d.flags;
    return p;
}

// Static curve: output level (dB) for an input level (dB).
float comp_curve(float x, const CompParams& p) {
    const float over = x - p.threshold_db;
    const float slope = 1.0f / p.ratio - 1.0f;
    if (p.knee_db <= 0.0f) return over <= 0 ? x : x + slope * over;
    if (2.0f * over < -p.knee_db) return x;
    if (2.0f * over > p.knee_db) return x + slope * over;
    const float t = over + p.knee_db * 0.5f;
    return x + slope * t * t / (2.0f * p.knee_db);
}

}  // namespace

struct nue_compressor_s {
    uint32_t ch, rate;
    nuea::ParamBox<CompParams> box;
    uint32_t seen = 0;
    CompParams p;
    float att = 0, rel = 0;                 // smoothing coefficients
    float env_db = -120.0f;                 // smoothed gain reduction state (dB, <= 0 when reducing: we smooth the reduction amount)
    float gr = 0;                           // current gain reduction in dB (>= 0)
    float last_gr_db = 0;
    std::vector<float> rms;                 // per channel mean-square state (RMS detector)
    float auto_gain = 1.0f;
    void apply(const CompParams& np) {
        p = np;
        att = std::exp(-1.0f / (0.001f * p.attack_ms * float(rate)));
        rel = std::exp(-1.0f / (0.001f * p.release_ms * float(rate)));
        if (p.flags & NUE_COMPRESSOR_AUTO_MAKEUP) {
            // gain that a full-scale signal would lose, restored: -(curve(0) - 0)
            auto_gain = db_to_lin(-(comp_curve(0.0f, p) - 0.0f));
        } else {
            auto_gain = db_to_lin(p.makeup_db);
        }
    }
};

extern "C" {

NUE_API void NUE_CALL nue_compressor_desc_default(nue_compressor_desc* d, uint32_t channels, uint32_t rate) {
    std::memset(d, 0, sizeof(*d));
    d->struct_size = sizeof(*d);
    d->channels = channels; d->sample_rate = rate;
    d->threshold_db = -18.0f; d->ratio = 3.0f; d->knee_db = 6.0f; d->attack_ms = 10.0f; d->release_ms = 120.0f; d->makeup_db = 0.0f; d->link = 1.0f;
}

NUE_API nue_result NUE_CALL nue_compressor_create(const nue_compressor_desc* d, nue_compressor* out) {
    if (!d || !out || d->channels == 0 || d->channels > 64 || d->sample_rate < 1000) return NUE_ERR_INVALID_ARGUMENT;
    auto* c = new (std::nothrow) nue_compressor_s();
    if (!c) return NUE_ERR_OUT_OF_MEMORY;
    c->ch = d->channels; c->rate = d->sample_rate;
    c->rms.assign(c->ch, 0.0f);
    const CompParams p = sanitize(*d);
    c->box.set(p);
    c->apply(p);
    c->seen = 2;
    *out = c;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_compressor_destroy(nue_compressor c) { delete c; }

NUE_API void NUE_CALL nue_compressor_reset(nue_compressor c) {
    if (!c) return;
    c->gr = 0; c->last_gr_db = 0;
    std::fill(c->rms.begin(), c->rms.end(), 0.0f);
}

NUE_API nue_result NUE_CALL nue_compressor_set_params(nue_compressor c, const nue_compressor_desc* d) {
    if (!c || !d) return NUE_ERR_INVALID_ARGUMENT;
    c->box.set(sanitize(*d));
    return NUE_OK;
}

NUE_API void NUE_CALL nue_compressor_process(nue_compressor c, float* x, uint32_t frames) {
    if (!c || !x) return;
    CompParams np;
    if (c->box.get(np, c->seen)) c->apply(np);
    const uint32_t ch = c->ch;
    const bool rms = (c->p.flags & NUE_COMPRESSOR_RMS_DETECT) != 0;
    const float rms_k = std::exp(-1.0f / (0.005f * float(c->rate)));            // 5 ms window
    float worst_gr = 0;
    for (uint32_t f = 0; f < frames; ++f) {
        float* s = x + size_t(f) * ch;
        // detector: loudest channel, linked with the others by `link`
        float peak = 0, mean = 0;
        for (uint32_t k = 0; k < ch; ++k) {
            float a = std::fabs(s[k]);
            if (rms) { c->rms[k] = a * a + rms_k * (c->rms[k] - a * a); a = std::sqrt(std::max(c->rms[k], 0.0f)); }
            peak = std::max(peak, a);
            mean += a;
        }
        mean /= float(ch);
        const float level = c->p.link * peak + (1.0f - c->p.link) * mean;
        const float x_db = lin_to_db(level);
        const float target = x_db - comp_curve(x_db, c->p);                 // wanted gain reduction, >= 0
        // branching smoothing: attack when the reduction grows, release when it shrinks
        const float coef = target > c->gr ? c->att : c->rel;
        c->gr = target + coef * (c->gr - target);
        const float gain = db_to_lin(-c->gr) * c->auto_gain;
        for (uint32_t k = 0; k < ch; ++k) s[k] *= gain;
        worst_gr = std::max(worst_gr, c->gr);
    }
    c->last_gr_db = -worst_gr;
}

NUE_API float NUE_CALL nue_compressor_gain_reduction_db(nue_compressor c) { return c ? c->last_gr_db : 0.0f; }

NUE_API void NUE_CALL nue_compressor_dsp(void* h, float* x, uint32_t frames, uint32_t channels, uint32_t) {
    nue_compressor c = static_cast<nue_compressor>(h);
    if (c && channels == c->ch) nue_compressor_process(c, x, frames);
}

}  // extern "C"

// ---------------------------------------------------------------------------------------------------------------- limiter

namespace {

struct LimParams { float ceiling, release_ms; };

// True-peak estimation: 16 tap Kaiser windowed sinc interpolators at 1/4, 1/2 and 3/4 between two samples (4x oversampling, the usual BS.1770
// approach). The kernels are built once.
struct TruePeakKernels {
    float h[3][16];
    TruePeakKernels() {
        const double beta = 8.0;
        auto i0 = [](double x) { double sum = 1, term = 1; for (int k = 1; k < 40; ++k) { term *= (x * x * 0.25) / (double(k) * k); sum += term; } return sum; };
        for (int k = 0; k < 3; ++k) {
            const double f = 0.25 * double(k + 1);
            double total = 0;
            for (int j = 0; j < 16; ++j) {
                const double d = (7.0 + f) - double(j);                           // distance from the interpolation point
                const double sinc = std::fabs(d) < 1e-12 ? 1.0 : std::sin(nuea::kPiD * d) / (nuea::kPiD * d);
                const double r = d / 8.0;
                const double w = std::fabs(r) < 1.0 ? i0(beta * std::sqrt(1.0 - r * r)) / i0(beta) : 0.0;
                h[k][j] = float(sinc * w);
                total += h[k][j];
            }
            for (int j = 0; j < 16; ++j) h[k][j] = float(h[k][j] / total);
        }
    }
};
const TruePeakKernels& tp_kernels() { static const TruePeakKernels k; return k; }

}  // namespace

struct nue_limiter_s {
    uint32_t ch, rate, look, dly, flags;      // look = window of the sliding minimum / moving average, dly = look - 1 = latency
    nuea::ParamBox<LimParams> box;
    uint32_t seen = 0;
    LimParams p;
    float rel_coef = 0;
    // delay line of the audio (look frames) and a ring of the required gain
    std::vector<float> delay;               // look * ch
    std::vector<float> hist;                // last 16 input frames per channel for the true-peak interpolation (16 * ch, oldest first)
    std::vector<float> need;                // required gain per frame, ring of `look` entries (the window of the sliding minimum)
    std::vector<float> box_ring;            // ring of the sliding-min output for the moving average
    std::deque<std::pair<uint64_t, float>> minq;
    uint64_t t = 0;
    double box_sum = 0;
    float rel_gain = 1.0f;
    float last_gr_db = 0;
    uint32_t pos = 0, dpos = 0;
    void apply(const LimParams& np) {
        p = np;
        rel_coef = std::exp(-1.0f / (0.001f * p.release_ms * float(rate)));
    }
};

extern "C" {

NUE_API void NUE_CALL nue_limiter_desc_default(nue_limiter_desc* d, uint32_t channels, uint32_t rate) {
    std::memset(d, 0, sizeof(*d));
    d->struct_size = sizeof(*d);
    d->channels = channels; d->sample_rate = rate;
    d->ceiling_db = -1.0f; d->lookahead_ms = 5.0f; d->release_ms = 80.0f;
}

NUE_API nue_result NUE_CALL nue_limiter_create(const nue_limiter_desc* d, nue_limiter* out) {
    if (!d || !out || d->channels == 0 || d->channels > 64 || d->sample_rate < 1000) return NUE_ERR_INVALID_ARGUMENT;
    auto* l = new (std::nothrow) nue_limiter_s();
    if (!l) return NUE_ERR_OUT_OF_MEMORY;
    l->ch = d->channels; l->rate = d->sample_rate; l->flags = d->flags;
    l->look = std::max<uint32_t>((d->flags & NUE_LIMITER_TRUE_PEAK) ? 10u : 2u, uint32_t(clampf(d->lookahead_ms, 0.5f, 20.0f) * 0.001f * float(d->sample_rate)));
    l->dly = l->look - 1;
    l->delay.assign(size_t(l->dly) * l->ch, 0.0f);
    l->hist.assign(size_t(16) * l->ch, 0.0f);
    l->box_ring.assign(l->look, 1.0f);
    l->box_sum = double(l->look);
    LimParams p{db_to_lin(clampf(d->ceiling_db, -48.0f, 0.0f)), clampf(d->release_ms, 1.0f, 2000.0f)};
    l->box.set(p);
    l->apply(p);
    l->seen = 2;
    *out = l;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_limiter_destroy(nue_limiter l) { delete l; }

NUE_API void NUE_CALL nue_limiter_reset(nue_limiter l) {
    if (!l) return;
    std::fill(l->delay.begin(), l->delay.end(), 0.0f);
    std::fill(l->hist.begin(), l->hist.end(), 0.0f);
    std::fill(l->box_ring.begin(), l->box_ring.end(), 1.0f);
    l->box_sum = double(l->look);
    l->minq.clear();
    l->t = 0; l->pos = 0; l->dpos = 0; l->rel_gain = 1.0f; l->last_gr_db = 0;
}

NUE_API nue_result NUE_CALL nue_limiter_set_params(nue_limiter l, const nue_limiter_desc* d) {
    if (!l || !d) return NUE_ERR_INVALID_ARGUMENT;
    l->box.set(LimParams{db_to_lin(clampf(d->ceiling_db, -48.0f, 0.0f)), clampf(d->release_ms, 1.0f, 2000.0f)});
    return NUE_OK;
}

NUE_API uint32_t NUE_CALL nue_limiter_latency_frames(nue_limiter l) { return l ? l->dly : 0; }
NUE_API float NUE_CALL nue_limiter_gain_reduction_db(nue_limiter l) { return l ? l->last_gr_db : 0.0f; }

NUE_API void NUE_CALL nue_limiter_process(nue_limiter l, float* x, uint32_t frames) {
    if (!l || !x) return;
    LimParams np;
    if (l->box.get(np, l->seen)) l->apply(np);
    const uint32_t ch = l->ch, look = l->look;
    const bool tp = (l->flags & NUE_LIMITER_TRUE_PEAK) != 0;
    float worst = 1.0f;
    for (uint32_t f = 0; f < frames; ++f) {
        float* s = x + size_t(f) * ch;
        // 1. peak of the incoming frame (sample peak, or the interpolated peak between the previous frame and this one)
        float peak = 0;
        for (uint32_t k = 0; k < ch; ++k) peak = std::max(peak, std::fabs(s[k]));
        if (tp) {
            // slide the 16 frame window, then look at the three points between the frames 8 and 7 frames back: the peak found there constrains
            // the gain from now on, which is before the audio it belongs to leaves the delay line (look >= 9 frames)
            std::memmove(&l->hist[0], &l->hist[ch], size_t(15) * ch * sizeof(float));
            std::memcpy(&l->hist[size_t(15) * ch], s, ch * sizeof(float));
            const TruePeakKernels& K = tp_kernels();
            for (uint32_t k = 0; k < ch; ++k) {
                peak = std::max(peak, std::fabs(l->hist[size_t(7) * ch + k]));
                for (int q = 0; q < 3; ++q) {
                    float v = 0;
                    for (int j = 0; j < 16; ++j) v += K.h[q][j] * l->hist[size_t(j) * ch + k];
                    peak = std::max(peak, std::fabs(v));
                }
            }
        }
        const float need = peak > l->p.ceiling ? l->p.ceiling / peak : 1.0f;
        // 2. sliding minimum over the last `look` required gains (monotonic deque)
        while (!l->minq.empty() && l->minq.back().second >= need) l->minq.pop_back();
        l->minq.emplace_back(l->t, need);
        if (l->minq.front().first + look <= l->t) l->minq.pop_front();
        const float mn = l->minq.front().second;
        // 3. moving average of that minimum over `look` frames: reaches the target exactly when the peak leaves the delay line
        l->box_sum += double(mn) - double(l->box_ring[l->pos]);
        l->box_ring[l->pos] = mn;
        float g = float(l->box_sum / double(look));
        // 4. release: never above the smoothed box gain, recovers with the release time constant
        l->rel_gain = std::min(g, 1.0f - l->rel_coef * (1.0f - l->rel_gain));
        g = l->rel_gain;
        // 5. apply to the delayed audio
        float* d = &l->delay[size_t(l->dpos) * ch];
        for (uint32_t k = 0; k < ch; ++k) { const float in = s[k]; s[k] = d[k] * g; d[k] = in; }
        worst = std::min(worst, g);
        l->dpos = (l->dpos + 1) % l->dly;
        l->pos = (l->pos + 1) % look;
        ++l->t;
    }
    l->last_gr_db = lin_to_db(worst);
}

NUE_API void NUE_CALL nue_limiter_dsp(void* h, float* x, uint32_t frames, uint32_t channels, uint32_t) {
    nue_limiter l = static_cast<nue_limiter>(h);
    if (l && channels == l->ch) nue_limiter_process(l, x, frames);
}

}  // extern "C"

// ---------------------------------------------------------------------------------------------------------------- dynamic range modes

struct nue_drc_s {
    nue_compressor comp = nullptr;
    nue_limiter lim = nullptr;
    std::atomic<int> mode{0};
    int applied = -1;
    uint32_t ch, rate;
};

namespace {
void drc_apply(nue_drc_s* d, int mode) {
    nue_compressor_desc c;
    nue_compressor_desc_default(&c, d->ch, d->rate);
    nue_limiter_desc l;
    nue_limiter_desc_default(&l, d->ch, d->rate);
    switch (mode) {
    case NUE_DRC_SPEAKERS:   c.threshold_db = -20; c.ratio = 2.0f; c.knee_db = 6;  c.attack_ms = 15; c.release_ms = 200; l.ceiling_db = -1.0f; break;
    case NUE_DRC_HEADPHONES: c.threshold_db = -16; c.ratio = 1.5f; c.knee_db = 8;  c.attack_ms = 20; c.release_ms = 250; l.ceiling_db = -2.0f; l.flags = NUE_LIMITER_TRUE_PEAK; break;
    case NUE_DRC_TV:         c.threshold_db = -30; c.ratio = 4.0f; c.knee_db = 8;  c.attack_ms = 8;  c.release_ms = 300; l.ceiling_db = -3.0f; l.flags = NUE_LIMITER_TRUE_PEAK; break;
    case NUE_DRC_NIGHT:      c.threshold_db = -38; c.ratio = 6.0f; c.knee_db = 10; c.attack_ms = 5;  c.release_ms = 400; c.flags = NUE_COMPRESSOR_AUTO_MAKEUP; l.ceiling_db = -6.0f; l.flags = NUE_LIMITER_TRUE_PEAK; break;
    default: c.ratio = 1.0f; l.ceiling_db = 0.0f; break;
    }
    nue_compressor_set_params(d->comp, &c);
    nue_limiter_set_params(d->lim, &l);
}
}  // namespace

extern "C" {

NUE_API nue_result NUE_CALL nue_drc_create(uint32_t channels, uint32_t rate, nue_drc_mode mode, nue_drc* out) {
    if (!out || mode < NUE_DRC_OFF || mode > NUE_DRC_NIGHT) return NUE_ERR_INVALID_ARGUMENT;
    auto* d = new (std::nothrow) nue_drc_s();
    if (!d) return NUE_ERR_OUT_OF_MEMORY;
    d->ch = channels; d->rate = rate;
    nue_compressor_desc c;
    nue_compressor_desc_default(&c, channels, rate);
    nue_limiter_desc l;
    nue_limiter_desc_default(&l, channels, rate);
    l.flags = NUE_LIMITER_TRUE_PEAK;        // the limiter keeps the detector for every mode (flags are fixed at creation)
    nue_result r = nue_compressor_create(&c, &d->comp);
    if (!NUE_FAILED(r)) r = nue_limiter_create(&l, &d->lim);
    if (NUE_FAILED(r)) { nue_compressor_destroy(d->comp); delete d; return r; }
    d->mode.store(mode);
    drc_apply(d, mode);
    d->applied = mode;
    *out = d;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_drc_destroy(nue_drc d) {
    if (!d) return;
    nue_compressor_destroy(d->comp);
    nue_limiter_destroy(d->lim);
    delete d;
}

NUE_API nue_result NUE_CALL nue_drc_set_mode(nue_drc d, nue_drc_mode mode) {
    if (!d || mode < NUE_DRC_OFF || mode > NUE_DRC_NIGHT) return NUE_ERR_INVALID_ARGUMENT;
    d->mode.store(mode);
    return NUE_OK;
}

NUE_API uint32_t NUE_CALL nue_drc_latency_frames(nue_drc d) { return d ? nue_limiter_latency_frames(d->lim) : 0; }

NUE_API void NUE_CALL nue_drc_process(nue_drc d, float* x, uint32_t frames) {
    if (!d || !x) return;
    const int m = d->mode.load();
    if (m != d->applied) { drc_apply(d, m); d->applied = m; }
    nue_compressor_process(d->comp, x, frames);
    nue_limiter_process(d->lim, x, frames);       // always in the chain so the latency does not change with the mode
}

NUE_API void NUE_CALL nue_drc_dsp(void* h, float* x, uint32_t frames, uint32_t channels, uint32_t) {
    nue_drc d = static_cast<nue_drc>(h);
    if (d && channels == d->ch) nue_drc_process(d, x, frames);
}

}  // extern "C"
