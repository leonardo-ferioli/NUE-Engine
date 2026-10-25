// Reverb: early reflections + input diffusion + an 8 line feedback delay network (Jot-style: orthogonal Hadamard feedback matrix, per-line
// decay gain and high-frequency damping designed from the wanted RT60 at low and high frequencies, slowly modulated delay lengths).
// Parameters are the EFX / EAX reverb model; conversions from I3DL2 (DirectSound) and EAX 2/3 property sets live in i3dl2.cpp.
//
// References for the structure: J.-M. Jot and A. Chaigne, "Digital delay networks for designing artificial reverberators" (AES 1991);
// J. Dattorro, "Effect Design" part 1 (JAES 1997) for the allpass diffusion; Freeverb for the damping idea. No code was taken from them.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

using nuea::clampf;
using nuea::kPi;

namespace {

constexpr uint32_t kLines = 8;
constexpr uint32_t kEarlyTaps = 8;
constexpr uint32_t kAllpass = 4;

// Delay line lengths in milliseconds of the late field at density 1 (mutually prime in samples at 48 kHz: 1429 1777 1973 2099 2587 2837 3229 3547)
const float kLineMs[kLines] = {29.77f, 37.02f, 41.10f, 43.73f, 53.90f, 59.10f, 67.27f, 73.90f};
const float kAllpassMs[kAllpass] = {3.15f, 4.53f, 7.87f, 11.21f};
const float kEarlyMs[kEarlyTaps] = {0.0f, 2.3f, 4.1f, 6.7f, 9.4f, 12.3f, 15.7f, 19.5f};
const float kEarlyGain[kEarlyTaps] = {1.0f, -0.8f, 0.7f, -0.6f, 0.5f, -0.4f, 0.33f, -0.25f};

// Hadamard 8x8 rows (Sylvester order); row 0 is all ones and is left to the feedback network.
float hadamard(uint32_t row, uint32_t col) {
    uint32_t v = row & col, s = 0;
    while (v) { s ^= v & 1; v >>= 1; }
    return s ? -1.0f : 1.0f;
}

struct Delay {
    std::vector<float> buf;
    uint32_t mask = 0, w = 0;
    void alloc(uint32_t n) { uint32_t size = 16; while (size < n + 4) size <<= 1; buf.assign(size, 0.0f); mask = size - 1; w = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); w = 0; }
    inline void push(float v) { buf[w] = v; w = (w + 1) & mask; }
    // value written `d` samples ago (d >= 1), linear interpolation for fractional delays
    inline float read(float d) const {
        const uint32_t di = uint32_t(d);
        const float f = d - float(di);
        const float a = buf[(w - di) & mask], b = buf[(w - di - 1) & mask];
        return a + (b - a) * f;
    }
    inline float read_i(uint32_t d) const { return buf[(w - d) & mask]; }
};

struct OnePole { float z = 0; inline float lp(float x, float a) { z = a * x + (1.0f - a) * z; return z; } };

}  // namespace

struct nue_reverb_s {
    uint32_t ch = 2, rate = 48000;
    nue_channel_mask mask = 0;
    int lfe_index = -1;
    nuea::ParamBox<nue_reverb_params> box;
    uint32_t seen = 0;
    nue_reverb_params p;

    // early
    Delay pre;                               // input pre-delay line (also feeds the late field)
    float early_delay_s = 0;
    // diffusion
    Delay ap[kAllpass];
    float ap_g = 0.5f;
    // FDN
    Delay line[kLines];
    float len_tgt[kLines], len_cur[kLines];
    float g[kLines], damp_a[kLines];         // decay gain (at DC) and one-pole coefficient per line
    OnePole damp[kLines];
    float lfo_ph[kLines], lfo_inc[kLines], mod_depth_samples = 0;
    Delay late_pre;                          // late field start delay
    float late_delay_samples = 0;
    // echo
    Delay echo;
    float echo_len = 0, echo_depth = 0;
    // tone
    float tone_hf_a = 0.1f, tone_lf_a = 0.01f, gain_hf = 1, gain_lf = 1, master = 1;
    float refl_gain = 1, late_gain = 1;
    float refl_pan_l = 1, refl_pan_r = 1, late_pan_l = 1, late_pan_r = 1;
    OnePole early_lp_l, early_lp_r;
    float early_lp_a = 1.0f;

    void init(uint32_t channels, nue_channel_mask m, uint32_t sr) {
        ch = channels; rate = sr; mask = m;
        lfe_index = (m & NUE_SPK_LFE) ? int(nue_channel_mask_count(m & (NUE_SPK_LFE - 1))) : -1;
        pre.alloc(uint32_t(0.65f * float(sr)));
        late_pre.alloc(uint32_t(0.25f * float(sr)));
        echo.alloc(uint32_t(0.3f * float(sr)));
        for (uint32_t i = 0; i < kAllpass; ++i) ap[i].alloc(uint32_t(kAllpassMs[i] * 0.001f * float(sr)) + 4);
        for (uint32_t i = 0; i < kLines; ++i) {
            line[i].alloc(uint32_t(kLineMs[i] * 0.001f * float(sr) * 1.5f) + 64);
            lfo_ph[i] = float(i) * 0.7853982f;
        }
        nue_reverb_params def;
        nue_reverb_params_default(&def);
        design(def);
        for (uint32_t i = 0; i < kLines; ++i) len_cur[i] = len_tgt[i];
        reset();
    }
    void reset() {
        pre.clear(); late_pre.clear(); echo.clear();
        for (auto& a : ap) a.clear();
        for (auto& l : line) l.clear();
        for (auto& d : damp) d.z = 0;
        early_lp_l.z = early_lp_r.z = 0;
    }
    static float pole_for(float ratio_gain, float cos_w) {
        // one pole lowpass (1-p)/(1-p z^-1), DC gain 1, with |H(w)| = ratio_gain (< 1): solve (1-p)^2 = r^2 (1 - 2 p cos w + p^2)
        const float r2 = ratio_gain * ratio_gain;
        if (r2 >= 0.9999f) return 0.0f;
        const float a = 1.0f - r2 * cos_w;
        const float disc = a * a - (1.0f - r2) * (1.0f - r2);
        return (a - std::sqrt(std::max(disc, 0.0f))) / (1.0f - r2);
    }
    void design(const nue_reverb_params& np) {
        p = np;
        const float sr = float(rate);
        const float dens = clampf(p.density, 0.0f, 1.0f);
        const float scale = 0.35f + 0.65f * dens;
        const float t60 = clampf(p.decay_time, 0.1f, 20.0f);
        float ratio = clampf(p.decay_hf_ratio, 0.1f, 2.0f);
        if (p.decay_hf_limit) {
            const float air = clampf(p.air_absorption_gain_hf, 0.5f, 1.0f);
            const float db_per_m = -20.0f * std::log10(air);
            if (db_per_m > 1e-6f) ratio = std::min(ratio, 60.0f / (db_per_m * 343.0f * t60));
        }
        ratio = std::min(ratio, 1.0f);                           // HF decaying slower than the mids is not modelled (would need a boost in the loop)
        const float w_hf = 2.0f * kPi * clampf(p.hf_reference, 1000.0f, 20000.0f) / sr;
        const float cos_w = std::cos(std::min(w_hf, kPi));
        for (uint32_t i = 0; i < kLines; ++i) {
            const float len = kLineMs[i] * 0.001f * sr * scale;
            len_tgt[i] = std::max(std::round(len), 8.0f);                         // whole samples: a fractional read would low-pass the loop and shorten the decay
            const float d = len_tgt[i];
            g[i] = std::pow(10.0f, -3.0f * d / (sr * t60));                       // loop gain at DC for an RT60 of t60
            const float g_hf = std::pow(10.0f, -3.0f * d / (sr * t60 * ratio));  // loop gain at the reference frequency
            damp_a[i] = pole_for(g_hf / g[i], cos_w);                              // pole of the lowpass (0 = none)
        }
        const float diff = clampf(p.diffusion, 0.0f, 1.0f);
        ap_g = 0.30f + 0.40f * diff;
        mod_depth_samples = clampf(p.modulation_depth, 0.0f, 1.0f) * 0.0006f * sr;
        const float mod_hz = 1.0f / clampf(p.modulation_time, 0.04f, 4.0f);
        for (uint32_t i = 0; i < kLines; ++i) lfo_inc[i] = 2.0f * kPi * mod_hz * (1.0f + 0.083f * float(i)) / sr;
        early_delay_s = clampf(p.reflections_delay, 0.0f, 0.3f);
        late_delay_samples = clampf(p.late_delay, 0.0f, 0.1f) * sr;
        echo_len = clampf(p.echo_time, 0.075f, 0.25f) * sr;
        echo_depth = clampf(p.echo_depth, 0.0f, 1.0f);
        master = clampf(p.gain, 0.0f, 1.0f);
        gain_hf = clampf(p.gain_hf, 0.0f, 1.0f);
        gain_lf = clampf(p.gain_lf, 0.0f, 1.0f);
        refl_gain = clampf(p.reflections_gain, 0.0f, 3.16f);
        late_gain = clampf(p.late_gain, 0.0f, 10.0f);
        tone_hf_a = 1.0f - std::exp(-2.0f * kPi * clampf(p.hf_reference, 1000.0f, 20000.0f) / sr);
        tone_lf_a = 1.0f - std::exp(-2.0f * kPi * clampf(p.lf_reference, 20.0f, 1000.0f) / sr);
        early_lp_a = 1.0f - std::exp(-2.0f * kPi * 9000.0f / sr);
        auto pan = [](float x, float& l, float& r) { x = clampf(x, -1.0f, 1.0f); l = x > 0 ? 1.0f - x : 1.0f; r = x < 0 ? 1.0f + x : 1.0f; };
        pan(p.reflections_pan[0], refl_pan_l, refl_pan_r);
        pan(p.late_pan[0], late_pan_l, late_pan_r);
    }

    void process(float* x, uint32_t frames) {
        nue_reverb_params np;
        if (box.get(np, seen)) design(np);
        const float sr = float(rate);
        const uint32_t out_ch = ch;
        const float in_norm = 1.0f / std::sqrt(float(kLines));
        const float glide = 0.9995f;
        static const uint32_t kOutRow[7] = {1, 2, 3, 4, 5, 6, 7};      // Hadamard rows used as output taps
        const uint32_t kInjectRow = 7;                                   // injection pattern: orthogonal to the first six output rows
        for (uint32_t f = 0; f < frames; ++f) {
            float* s = x + size_t(f) * out_ch;
            // input: all channels except the LFE, summed to mono
            float in = 0;
            uint32_t n = 0;
            for (uint32_t c = 0; c < out_ch; ++c) if (int(c) != lfe_index) { in += s[c]; ++n; }
            in = n ? in / float(n) : 0.0f;
            pre.push(in);

            // ---- early reflections (taps of the pre-delay line), alternating between the two sides
            float early_l = 0, early_r = 0;
            const float e0 = early_delay_s * sr;
            for (uint32_t t = 0; t < kEarlyTaps; ++t) {
                const float d = std::max(1.0f, e0 + kEarlyMs[t] * 0.001f * sr);
                const float v = pre.read(d) * kEarlyGain[t];
                if (t & 1) early_r += v; else early_l += v;
            }
            early_l = early_lp_l.lp(early_l, early_lp_a) * refl_gain * refl_pan_l * 0.5f;
            early_r = early_lp_r.lp(early_r, early_lp_a) * refl_gain * refl_pan_r * 0.5f;

            // ---- late field input: delayed, diffused
            late_pre.push(pre.read(std::max(1.0f, e0)));
            float v = late_pre.read(std::max(1.0f, late_delay_samples + 1.0f));
            for (uint32_t i = 0; i < kAllpass; ++i) {
                // Schroeder allpass in delay-line form: w = x + g d ; y = d - g w
                const uint32_t dl = std::max<uint32_t>(uint32_t(kAllpassMs[i] * 0.001f * sr), 1);
                const float d = ap[i].read_i(dl);
                const float w = v + ap_g * d;
                ap[i].push(w);
                v = d - ap_g * w;
            }

            // ---- FDN
            float out_lines[kLines];
            for (uint32_t i = 0; i < kLines; ++i) {
                len_cur[i] = glide * len_cur[i] + (1.0f - glide) * len_tgt[i];
                lfo_ph[i] += lfo_inc[i];
                if (lfo_ph[i] > 2.0f * kPi) lfo_ph[i] -= 2.0f * kPi;
                if (mod_depth_samples > 0.0f || std::fabs(len_cur[i] - len_tgt[i]) > 0.01f) {
                    const float d = len_cur[i] + mod_depth_samples * std::sin(lfo_ph[i]);
                    out_lines[i] = line[i].read(std::max(d, 2.0f));
                } else {
                    out_lines[i] = line[i].read_i(uint32_t(len_tgt[i]));
                }
            }
            // feedback matrix: the 8x8 Hadamard matrix / sqrt(8) (orthogonal: keeps the energy, mixes every line into every other)
            float mixed[kLines];
            for (uint32_t i = 0; i < kLines; ++i) mixed[i] = out_lines[i];
            for (uint32_t len = 1; len < kLines; len <<= 1)
                for (uint32_t i = 0; i < kLines; i += len << 1)
                    for (uint32_t j = i; j < i + len; ++j) { const float u = mixed[j], v2 = mixed[j + len]; mixed[j] = u + v2; mixed[j + len] = u - v2; }
            for (uint32_t i = 0; i < kLines; ++i) {
                float fb = mixed[i] * 0.35355339f;
                fb = damp[i].lp(fb, 1.0f - damp_a[i]);                             // one-pole lowpass with DC gain 1
                line[i].push(fb * g[i] + v * in_norm * hadamard(kInjectRow, i));
            }
            float late_out[7];
            for (uint32_t o = 0; o < 7; ++o) {
                float acc = 0;
                for (uint32_t i = 0; i < kLines; ++i) acc += hadamard(kOutRow[o], i) * out_lines[i];
                late_out[o] = acc * 0.35355339f;
            }

            // echo: the first late output fed back through a long delay, mixed in at echo_depth
            float echo_v = 0;
            if (echo_depth > 0.0f) {
                echo_v = echo.read(echo_len) * echo_depth * 0.5f;
                echo.push(late_out[0] * 0.5f + echo_v * 0.4f);
            } else {
                echo.push(0.0f);
            }

            // ---- mix to the output channels
            uint32_t slot = 0;
            for (uint32_t c = 0; c < out_ch; ++c) {
                if (int(c) == lfe_index) { s[c] = 0.0f; continue; }
                float l = late_out[slot % 7] * late_gain;
                if (out_ch == 2) l *= (c == 0 ? late_pan_l : late_pan_r);
                float e = out_ch == 1 ? 0.5f * (early_l + early_r) : ((slot & 1) == 0 ? early_l : early_r);
                if (out_ch > 2 && c >= 2) e *= 0.6f;
                s[c] = l + e + (slot == 0 ? echo_v : 0.0f);
                ++slot;
            }
        }
        apply_tone_and_master(x, frames);
    }

    // per-channel shelves + master gain
    std::vector<OnePole> hf_state, lf_state;
    void apply_tone_and_master(float* x, uint32_t frames) {
        if (hf_state.size() != ch) { hf_state.assign(ch, OnePole()); lf_state.assign(ch, OnePole()); }
        const bool shelf_hf = gain_hf < 0.999f, shelf_lf = gain_lf < 0.999f;
        for (uint32_t f = 0; f < frames; ++f) {
            float* s = x + size_t(f) * ch;
            for (uint32_t c = 0; c < ch; ++c) {
                float y = s[c];
                if (shelf_hf) { const float lp = hf_state[c].lp(y, tone_hf_a); y = lp + gain_hf * (y - lp); }
                if (shelf_lf) { const float lp = lf_state[c].lp(y, tone_lf_a); y += (gain_lf - 1.0f) * lp; }
                s[c] = nuea::flush_denormal(y * master);
            }
        }
    }
};

extern "C" {

NUE_API void NUE_CALL nue_reverb_params_default(nue_reverb_params* p) {
    std::memset(p, 0, sizeof(*p));
    p->density = 1.0f; p->diffusion = 1.0f; p->gain = 0.32f; p->gain_hf = 0.89f; p->gain_lf = 1.0f;
    p->decay_time = 1.49f; p->decay_hf_ratio = 0.83f; p->decay_lf_ratio = 1.0f;
    p->reflections_gain = 0.05f; p->reflections_delay = 0.007f;
    p->late_gain = 1.26f; p->late_delay = 0.011f;
    p->echo_time = 0.25f; p->echo_depth = 0.0f; p->modulation_time = 0.25f; p->modulation_depth = 0.0f;
    p->air_absorption_gain_hf = 0.994f; p->hf_reference = 5000.0f; p->lf_reference = 250.0f;
    p->room_rolloff_factor = 0.0f; p->decay_hf_limit = NUE_TRUE;
}

NUE_API nue_result NUE_CALL nue_reverb_create(uint32_t channels, nue_channel_mask mask, uint32_t rate, nue_reverb* out) {
    if (!out || channels == 0 || channels > NUE_AUDIO_MAX_CHANNELS || rate < 8000 || rate > 384000) return NUE_ERR_INVALID_ARGUMENT;
    if (!mask) mask = nue_channel_mask_default(channels);
    if (mask && nue_channel_mask_count(mask) != channels) return NUE_ERR_INVALID_ARGUMENT;
    auto* r = new (std::nothrow) nue_reverb_s();
    if (!r) return NUE_ERR_OUT_OF_MEMORY;
    r->init(channels, mask, rate);
    r->hf_state.assign(channels, OnePole());
    r->lf_state.assign(channels, OnePole());
    nue_reverb_params def;
    nue_reverb_params_default(&def);
    r->box.set(def);
    r->seen = 2;
    *out = r;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_reverb_destroy(nue_reverb r) { delete r; }

NUE_API void NUE_CALL nue_reverb_reset(nue_reverb r) {
    if (!r) return;
    r->reset();
    for (auto& s : r->hf_state) s.z = 0;
    for (auto& s : r->lf_state) s.z = 0;
}

NUE_API nue_result NUE_CALL nue_reverb_set_params(nue_reverb r, const nue_reverb_params* p) {
    if (!r || !p) return NUE_ERR_INVALID_ARGUMENT;
    r->box.set(*p);
    return NUE_OK;
}

NUE_API void NUE_CALL nue_reverb_process(nue_reverb r, float* samples, uint32_t frames) {
    if (r && samples && frames) r->process(samples, frames);
}

NUE_API void NUE_CALL nue_reverb_dsp(void* h, float* samples, uint32_t frames, uint32_t channels, uint32_t) {
    nue_reverb r = static_cast<nue_reverb>(h);
    if (r && channels == r->ch) r->process(samples, frames);
}

}  // extern "C"
