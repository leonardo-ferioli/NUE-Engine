// Windowed-sinc polyphase resampler. The design follows the usual structure of libsamplerate / the Speex resampler (a sinc kernel with a
// Kaiser window sampled at a number of fractional phases, linear interpolation between phases, a fixed-point read position so that long
// streams do not drift); the code is our own. Kernel length and Kaiser beta come from the standard Kaiser design formulas.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

using nuea::kPiD;

namespace {

struct Design { double atten_db; double passband; uint32_t phases; };
const Design kDesigns[4] = {{60.0, 0.80, 128}, {80.0, 0.85, 256}, {100.0, 0.90, 512}, {120.0, 0.94, 1024}};

double bessel_i0(double x) {
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 60; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-18) break;
    }
    return sum;
}

double kaiser_beta(double a) {
    if (a > 50.0) return 0.1102 * (a - 8.7);
    if (a >= 21.0) return 0.5842 * std::pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0);
    return 0.0;
}

}  // namespace

struct nue_resampler_s {
    uint32_t ch = 1;
    nue_resample_quality quality = NUE_RESAMPLE_HIGH;
    double in_rate = 1, out_rate = 1;
    // design (depends on the quality and on the cut-off class min(1, out/in))
    double design_ratio = 1;
    uint32_t taps = 0, half = 0, phases = 0;
    std::vector<float> table;               // (phases + 1) rows of `taps`
    uint64_t step = 0;                      // input frames per output frame, Q32
    uint64_t pos = 0;                       // position of the next output sample in `buf`, Q32 (integer part = frame index)
    std::vector<float> buf;                 // history + pending input, interleaved
    uint64_t buf_frames = 0;

    void design(double lo_ratio) {          // lo_ratio = min(1, out_rate / in_rate)
        const Design& d = kDesigns[quality];
        const double delta_f = 0.5 * (1.0 - d.passband);                         // transition width in cycles / sample of the LOWER rate
        double t_low = (d.atten_db - 7.95) / (14.36 * delta_f);                   // taps at the lower rate
        uint32_t t_in = uint32_t(std::ceil(t_low / lo_ratio));
        t_in += t_in & 1;                                                        // even
        uint32_t ph = d.phases;
        while (size_t(ph + 1) * t_in * sizeof(float) > size_t(24) * 1024 * 1024 && ph > 32) ph /= 2;
        taps = t_in; half = t_in / 2; phases = ph;
        design_ratio = lo_ratio;
        const double fc = (0.5 - delta_f * 0.5) * lo_ratio;                      // cut-off in cycles per input sample
        const double beta = kaiser_beta(d.atten_db);
        const double i0b = bessel_i0(beta);
        table.assign(size_t(phases + 1) * taps, 0.0f);
        for (uint32_t p = 0; p <= phases; ++p) {
            const double f = double(p) / double(phases);
            std::vector<double> row(taps);
            double sum = 0;
            for (uint32_t j = 0; j < taps; ++j) {
                const double x = f + double(half) - 1.0 - double(j);              // distance from the output instant, in input samples
                const double r = x / double(half);
                double w = 0.0;
                if (std::fabs(r) < 1.0) w = bessel_i0(beta * std::sqrt(1.0 - r * r)) / i0b;
                const double a = 2.0 * kPiD * fc * x;
                const double s = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
                row[j] = 2.0 * fc * s * w;
                sum += row[j];
            }
            for (uint32_t j = 0; j < taps; ++j) table[size_t(p) * taps + j] = float(row[j] / sum);       // unity gain at DC for every phase
        }
    }
    void set_step() {
        const double s = in_rate / out_rate;
        step = uint64_t(s * 4294967296.0 + 0.5);
    }
    void reset_state() {
        buf.assign(size_t(half - 1) * ch, 0.0f);          // zero history: the first output (time 0) sees silence before the signal
        buf_frames = half - 1;
        pos = uint64_t(half - 1) << 32;
    }
};

extern "C" {

NUE_API nue_result NUE_CALL nue_resampler_create(uint32_t channels, double in_rate, double out_rate, nue_resample_quality quality, nue_resampler* out) {
    if (!out || channels == 0 || channels > 64 || !(in_rate > 0) || !(out_rate > 0) || quality < NUE_RESAMPLE_FAST || quality > NUE_RESAMPLE_BEST) return NUE_ERR_INVALID_ARGUMENT;
    if (in_rate / out_rate > 1024.0 || out_rate / in_rate > 1024.0) return NUE_ERR_INVALID_ARGUMENT;
    auto* r = new (std::nothrow) nue_resampler_s();
    if (!r) return NUE_ERR_OUT_OF_MEMORY;
    r->ch = channels; r->quality = quality; r->in_rate = in_rate; r->out_rate = out_rate;
    r->design(std::min(1.0, out_rate / in_rate));
    r->set_step();
    r->reset_state();
    *out = r;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_resampler_destroy(nue_resampler r) { delete r; }
NUE_API void NUE_CALL nue_resampler_reset(nue_resampler r) { if (r) r->reset_state(); }

NUE_API nue_result NUE_CALL nue_resampler_set_rates(nue_resampler r, double in_rate, double out_rate) {
    if (!r || !(in_rate > 0) || !(out_rate > 0) || in_rate / out_rate > 1024.0 || out_rate / in_rate > 1024.0) return NUE_ERR_INVALID_ARGUMENT;
    r->in_rate = in_rate; r->out_rate = out_rate;
    const double lo = std::min(1.0, out_rate / in_rate);
    if (std::fabs(lo - r->design_ratio) > 0.02 * r->design_ratio) {
        const uint32_t old_half = r->half;
        r->design(lo);
        if (r->half > old_half) {                                           // longer filter than the history we have: pad with silence in front
            const uint64_t extra = r->half - old_half;
            r->buf.insert(r->buf.begin(), size_t(extra) * r->ch, 0.0f);
            r->buf_frames += extra;
            r->pos += extra << 32;
        }
    }
    r->set_step();
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_resampler_process(nue_resampler r, const float* in, uint32_t in_frames, uint32_t* in_used, float* out, uint32_t out_cap, uint32_t* out_frames) {
    if (!r || (!in && in_frames) || (!out && out_cap)) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t ch = r->ch, taps = r->taps, half = r->half;
    uint32_t used = 0, made = 0;
    while (made < out_cap) {
        const uint64_t idx = r->pos >> 32;
        // output at `idx` needs frames idx - half + 1 .. idx + half
        if (idx + half >= r->buf_frames) {
            if (used >= in_frames) break;
            const uint32_t chunk = std::min<uint32_t>(in_frames - used, 2048);
            r->buf.insert(r->buf.end(), in + size_t(used) * ch, in + size_t(used + chunk) * ch);
            r->buf_frames += chunk; used += chunk;
            continue;
        }
        const double frac_phase = double(r->pos & 0xFFFFFFFFull) * (1.0 / 4294967296.0) * double(r->phases);
        const uint32_t p0 = uint32_t(frac_phase);
        const float mu = float(frac_phase - double(p0));
        const float* r0 = &r->table[size_t(p0) * taps];
        const float* r1 = r0 + taps;
        const float* x = &r->buf[size_t(idx - (half - 1)) * ch];
        for (uint32_t c = 0; c < ch; ++c) {
            float a = 0, b = 0;
            const float* xp = x + c;
            for (uint32_t j = 0; j < taps; ++j, xp += ch) { a += r0[j] * *xp; b += r1[j] * *xp; }
            out[size_t(made) * ch + c] = a + (b - a) * mu;
        }
        ++made;
        r->pos += r->step;
        // drop frames that can no longer be needed (keep the history window)
        const uint64_t keep_from = (r->pos >> 32) - (half - 1);
        if (keep_from > 4096) {
            r->buf.erase(r->buf.begin(), r->buf.begin() + size_t(keep_from) * ch);
            r->buf_frames -= keep_from;
            r->pos -= keep_from << 32;
        }
    }
    if (in_used) *in_used = used;
    if (out_frames) *out_frames = made;
    return NUE_OK;
}

NUE_API uint32_t NUE_CALL nue_resampler_flush(nue_resampler r, float* out, uint32_t out_cap) {
    if (!r || !out) return 0;
    // push enough silence to finish every output whose window reaches into the signal's tail
    std::vector<float> zeros(size_t(r->half) * r->ch, 0.0f);
    uint32_t used = 0, made = 0;
    nue_resampler_process(r, zeros.data(), r->half, &used, out, out_cap, &made);
    return made;
}

NUE_API uint32_t NUE_CALL nue_resampler_latency_frames(nue_resampler r) { return r ? r->half : 0; }

NUE_API uint32_t NUE_CALL nue_resampler_output_frames_for(nue_resampler r, uint32_t in_frames) {
    if (!r) return 0;
    return uint32_t(double(in_frames) * r->out_rate / r->in_rate + 2.0);
}

NUE_API uint32_t NUE_CALL nue_resample_buffer(const float* in, uint32_t in_frames, uint32_t channels, double in_rate, double out_rate,
                                              nue_resample_quality quality, float* out, uint32_t out_cap) {
    if (!in || !out || !in_frames || !out_cap) return 0;
    nue_resampler r = nullptr;
    if (NUE_FAILED(nue_resampler_create(channels, in_rate, out_rate, quality, &r))) return 0;
    const uint32_t want = uint32_t(std::min<double>(std::floor(double(in_frames) * out_rate / in_rate + 0.5), double(out_cap)));
    uint32_t used = 0, made = 0;
    nue_resampler_process(r, in, in_frames, &used, out, want, &made);
    if (made < want) {                                                      // finish with the filter tail (silence after the end of the signal)
        std::vector<float> zeros(size_t(r->half + 2) * channels, 0.0f);
        uint32_t u2 = 0, m2 = 0;
        nue_resampler_process(r, zeros.data(), r->half + 2, &u2, out + size_t(made) * channels, want - made, &m2);
        made += m2;
    }
    nue_resampler_destroy(r);
    return made;
}

}  // extern "C"
