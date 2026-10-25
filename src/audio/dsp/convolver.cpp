// Uniformly partitioned FFT convolution (overlap-save) for impulse-response reverbs, with our own radix-2 FFT.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

namespace {

class Fft {
public:
    explicit Fft(uint32_t n) : n_(n), cs_(n / 2), sn_(n / 2), rev_(n) {
        for (uint32_t i = 0; i < n / 2; ++i) {
            cs_[i] = float(std::cos(2.0 * nuea::kPiD * i / n));
            sn_[i] = float(std::sin(2.0 * nuea::kPiD * i / n));
        }
        uint32_t bits = 0;
        while ((1u << bits) < n) ++bits;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t r = 0;
            for (uint32_t b = 0; b < bits; ++b) if (i & (1u << b)) r |= 1u << (bits - 1 - b);
            rev_[i] = r;
        }
    }
    uint32_t size() const { return n_; }
    // In-place complex FFT on split arrays. inverse = conjugate twiddles, no scaling (the caller scales by 1/N).
    void run(float* re, float* im, bool inverse) const {
        for (uint32_t i = 0; i < n_; ++i) {
            const uint32_t j = rev_[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        for (uint32_t len = 2; len <= n_; len <<= 1) {
            const uint32_t half = len >> 1, step = n_ / len;
            for (uint32_t base = 0; base < n_; base += len) {
                for (uint32_t k = 0; k < half; ++k) {
                    const float wr = cs_[k * step], wi = inverse ? sn_[k * step] : -sn_[k * step];
                    const uint32_t a = base + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                }
            }
        }
    }
private:
    uint32_t n_;
    std::vector<float> cs_, sn_;
    std::vector<uint32_t> rev_;
};

}  // namespace

struct nue_convolver_s {
    uint32_t ch = 2, rate = 48000, B = 512, N = 1024, P = 1, ir_ch = 1;
    int lfe_index = -1;
    Fft* fft = nullptr;
    // IR spectra: [ir_ch][P][re N | im N]
    std::vector<float> H;
    // input spectra ring: [P][re N | im N]
    std::vector<float> X;
    uint32_t xpos = 0;
    std::vector<float> in_prev, in_cur;          // B samples each (mono sum)
    uint32_t in_fill = 0;
    std::vector<float> out_q[2];                 // finished output blocks per IR channel, consumed sample by sample
    uint32_t out_pos = 0;
    std::vector<float> tr, ti, acc_r, acc_i;     // scratch

    ~nue_convolver_s() { delete fft; }

    void reset() {
        std::fill(X.begin(), X.end(), 0.0f);
        std::fill(in_prev.begin(), in_prev.end(), 0.0f);
        std::fill(in_cur.begin(), in_cur.end(), 0.0f);
        for (auto& q : out_q) std::fill(q.begin(), q.end(), 0.0f);
        in_fill = 0; xpos = 0; out_pos = 0;
    }

    void run_block() {
        // 1. spectrum of [prev | cur]
        for (uint32_t i = 0; i < B; ++i) { tr[i] = in_prev[i]; tr[B + i] = in_cur[i]; }
        std::fill(ti.begin(), ti.end(), 0.0f);
        fft->run(tr.data(), ti.data(), false);
        xpos = (xpos + P - 1) % P;                               // newest spectrum at xpos, older ones follow
        float* xr = &X[size_t(xpos) * 2 * N];
        std::copy(tr.begin(), tr.end(), xr);
        std::copy(ti.begin(), ti.end(), xr + N);
        // 2. multiply-accumulate against every IR partition, per IR channel
        for (uint32_t c = 0; c < ir_ch; ++c) {
            std::fill(acc_r.begin(), acc_r.end(), 0.0f);
            std::fill(acc_i.begin(), acc_i.end(), 0.0f);
            for (uint32_t p = 0; p < P; ++p) {
                const float* hx = &H[(size_t(c) * P + p) * 2 * N];
                const float* xs = &X[size_t((xpos + p) % P) * 2 * N];
                for (uint32_t k = 0; k < N; ++k) {
                    const float ar = xs[k], ai = xs[N + k], br = hx[k], bi = hx[N + k];
                    acc_r[k] += ar * br - ai * bi;
                    acc_i[k] += ar * bi + ai * br;
                }
            }
            fft->run(acc_r.data(), acc_i.data(), true);
            const float scale = 1.0f / float(N);
            for (uint32_t i = 0; i < B; ++i) out_q[c][i] = acc_r[B + i] * scale;
        }
        std::copy(in_cur.begin(), in_cur.end(), in_prev.begin());
    }

    void process(float* x, uint32_t frames) {
        for (uint32_t f = 0; f < frames; ++f) {
            float* s = x + size_t(f) * ch;
            float in = 0;
            uint32_t n = 0;
            for (uint32_t c = 0; c < ch; ++c) if (int(c) != lfe_index) { in += s[c]; ++n; }
            in_cur[in_fill] = n ? in / float(n) : 0.0f;
            // output of the previous block (latency = B)
            uint32_t slot = 0;
            for (uint32_t c = 0; c < ch; ++c) {
                if (int(c) == lfe_index) { s[c] = 0.0f; continue; }
                s[c] = out_q[ir_ch > 1 ? (slot & 1) : 0][in_fill];
                ++slot;
            }
            if (++in_fill == B) {
                // the block just finished is processed now; its result is read during the NEXT block
                // (the queue still holds the previous result for the samples read so far in this block: all of them were read)
                run_block();
                in_fill = 0;
            }
        }
    }
};

extern "C" {

NUE_API nue_result NUE_CALL nue_convolver_create(uint32_t channels, uint32_t rate, const float* ir, uint32_t ir_frames, uint32_t ir_channels,
                                                 uint32_t ir_rate, uint32_t block, nue_convolver* out) {
    if (!out || !ir || !ir_frames || channels == 0 || channels > NUE_AUDIO_MAX_CHANNELS || (ir_channels != 1 && ir_channels != 2) || rate < 8000 || ir_rate < 8000) return NUE_ERR_INVALID_ARGUMENT;
    if (block == 0) block = 512;
    if (block < 64 || block > 8192 || (block & (block - 1))) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<float> resampled;
    const float* src = ir;
    uint32_t frames = ir_frames;
    if (ir_rate != rate) {
        const uint32_t want = uint32_t(std::ceil(double(ir_frames) * rate / ir_rate)) + 1;
        resampled.assign(size_t(want) * ir_channels, 0.0f);
        frames = nue_resample_buffer(ir, ir_frames, ir_channels, ir_rate, rate, NUE_RESAMPLE_HIGH, resampled.data(), want);
        if (!frames) return NUE_ERR_FAILED;
        src = resampled.data();
    }
    auto* c = new (std::nothrow) nue_convolver_s();
    if (!c) return NUE_ERR_OUT_OF_MEMORY;
    c->ch = channels; c->rate = rate; c->B = block; c->N = block * 2; c->ir_ch = ir_channels;
    c->P = (frames + block - 1) / block;
    c->lfe_index = -1;
    const nue_channel_mask m = nue_channel_mask_default(channels);
    if (m & NUE_SPK_LFE) c->lfe_index = int(nue_channel_mask_count(m & (NUE_SPK_LFE - 1)));
    c->fft = new Fft(c->N);
    c->H.assign(size_t(ir_channels) * c->P * 2 * c->N, 0.0f);
    c->X.assign(size_t(c->P) * 2 * c->N, 0.0f);
    c->in_prev.assign(block, 0.0f); c->in_cur.assign(block, 0.0f);
    for (auto& q : c->out_q) q.assign(block, 0.0f);
    c->tr.assign(c->N, 0.0f); c->ti.assign(c->N, 0.0f); c->acc_r.assign(c->N, 0.0f); c->acc_i.assign(c->N, 0.0f);
    for (uint32_t ic = 0; ic < ir_channels; ++ic) {
        for (uint32_t p = 0; p < c->P; ++p) {
            float* hr = &c->H[(size_t(ic) * c->P + p) * 2 * c->N];
            float* hi = hr + c->N;
            for (uint32_t i = 0; i < block; ++i) {
                const uint32_t idx = p * block + i;
                hr[i] = idx < frames ? src[size_t(idx) * ir_channels + ic] : 0.0f;
            }
            c->fft->run(hr, hi, false);
        }
    }
    *out = c;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_convolver_destroy(nue_convolver c) { delete c; }
NUE_API void NUE_CALL nue_convolver_reset(nue_convolver c) { if (c) c->reset(); }
NUE_API uint32_t NUE_CALL nue_convolver_latency_frames(nue_convolver c) { return c ? c->B : 0; }
NUE_API void NUE_CALL nue_convolver_process(nue_convolver c, float* x, uint32_t frames) { if (c && x) c->process(x, frames); }
NUE_API void NUE_CALL nue_convolver_dsp(void* h, float* x, uint32_t frames, uint32_t channels, uint32_t) {
    nue_convolver c = static_cast<nue_convolver>(h);
    if (c && channels == c->ch) c->process(x, frames);
}

NUE_API uint32_t NUE_CALL nue_ir_generate_room(float decay_time, float damping, uint32_t rate, uint32_t seed, float* out, uint32_t capacity) {
    if (!out || decay_time <= 0.05f || rate < 8000) return 0;
    const uint32_t frames = std::min<uint32_t>(capacity, uint32_t(decay_time * 1.2f * float(rate)));
    uint32_t s = seed * 2654435761u + 12345u;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return float(int32_t(s)) * (1.0f / 2147483648.0f); };
    const float k = std::log(1000.0f) / (decay_time * float(rate));            // -60 dB (amplitude 1/1000) after decay_time
    float lp[2] = {0, 0};
    for (uint32_t i = 0; i < frames; ++i) {
        const float env = std::exp(-k * float(i));
        const float a = nuea::clampf(1.0f - damping * (0.2f + 0.8f * (float(i) / float(frames))), 0.02f, 1.0f);       // darker with time
        for (uint32_t c = 0; c < 2; ++c) {
            lp[c] += a * (rnd() - lp[c]);
            out[size_t(i) * 2 + c] = lp[c] * env * 0.3f;
        }
    }
    // direct sound + a few early reflections
    if (frames > 1) out[0] += 1.0f;
    return frames;
}

}  // extern "C"
