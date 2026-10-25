// Loudness measurement (EBU R128 / ITU-R BS.1770) through libebur128 (MIT, src/audio/third_party/ebur128) and normalisation helpers.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include "ebur128.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

struct nue_loudness_s {
    ebur128_state* st = nullptr;
    uint32_t ch = 0;
    nue_loudness_modes modes = 0;
    std::vector<int> map;                    // channel type per interleaved channel (kept to rebuild the state on reset)
};

static int to_modes(nue_loudness_modes m) {
    int r = EBUR128_MODE_M;                                   // momentary is the basis of everything
    if (m & NUE_LOUDNESS_SHORT_TERM) r |= EBUR128_MODE_S;
    if (m & NUE_LOUDNESS_INTEGRATED) r |= EBUR128_MODE_I;
    if (m & NUE_LOUDNESS_RANGE) r |= EBUR128_MODE_LRA;
    if (m & NUE_LOUDNESS_SAMPLE_PEAK) r |= EBUR128_MODE_SAMPLE_PEAK;
    if (m & NUE_LOUDNESS_TRUE_PEAK) r |= EBUR128_MODE_TRUE_PEAK;
    return r;
}

static double minus_inf() { return -std::numeric_limits<double>::infinity(); }

extern "C" {

NUE_API nue_result NUE_CALL nue_loudness_create(uint32_t channels, nue_channel_mask mask, uint32_t rate, nue_loudness_modes modes, nue_loudness* out) {
    if (!out || channels == 0 || channels > 32 || rate < 8000) return NUE_ERR_INVALID_ARGUMENT;
    if (!mask) mask = nue_channel_mask_default(channels);
    if (mask && nue_channel_mask_count(mask) != channels) return NUE_ERR_INVALID_ARGUMENT;
    auto* m = new (std::nothrow) nue_loudness_s();
    if (!m) return NUE_ERR_OUT_OF_MEMORY;
    m->ch = channels; m->modes = modes ? modes : NUE_LOUDNESS_ALL;
    m->st = ebur128_init(channels, rate, to_modes(m->modes));
    if (!m->st) { delete m; return NUE_ERR_OUT_OF_MEMORY; }
    m->map.assign(channels, EBUR128_UNUSED);
    if (channels == 1) {
        m->map[0] = EBUR128_DUAL_MONO;
    } else if (mask) {
        uint32_t idx = 0;
        for (nue_channel_mask bit = 1; bit && bit <= NUE_SPK_TBR; bit <<= 1) {
            if (!(mask & bit)) continue;
            int type = EBUR128_UNUSED;
            switch (bit) {
            case NUE_SPK_FL: case NUE_SPK_FLC: type = EBUR128_LEFT; break;
            case NUE_SPK_FR: case NUE_SPK_FRC: type = EBUR128_RIGHT; break;
            case NUE_SPK_FC: type = EBUR128_CENTER; break;
            case NUE_SPK_LFE: type = EBUR128_UNUSED; break;
            case NUE_SPK_BL: case NUE_SPK_SL: case NUE_SPK_TBL: type = EBUR128_LEFT_SURROUND; break;
            case NUE_SPK_BR: case NUE_SPK_SR: case NUE_SPK_TBR: type = EBUR128_RIGHT_SURROUND; break;
            case NUE_SPK_BC: type = EBUR128_DUAL_MONO; break;               // counted on both surround channels' weight: approximated by dual mono
            case NUE_SPK_TFL: type = EBUR128_LEFT; break;
            case NUE_SPK_TFR: type = EBUR128_RIGHT; break;
            case NUE_SPK_TFC: type = EBUR128_CENTER; break;
            default: type = EBUR128_UNUSED; break;
            }
            m->map[idx++] = type;
        }
    } else {
        for (uint32_t i = 0; i < channels; ++i) m->map[i] = i == 3 ? EBUR128_UNUSED : (i == 0 ? EBUR128_LEFT : i == 1 ? EBUR128_RIGHT : i == 2 ? EBUR128_CENTER : (i & 1) ? EBUR128_LEFT_SURROUND : EBUR128_RIGHT_SURROUND);
    }
    for (uint32_t i = 0; i < channels; ++i) ebur128_set_channel(m->st, i, m->map[i]);
    *out = m;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_loudness_destroy(nue_loudness m) {
    if (!m) return;
    ebur128_destroy(&m->st);
    delete m;
}

NUE_API void NUE_CALL nue_loudness_reset(nue_loudness m) {
    if (!m) return;
    // ebur128 has no reset: re-initialising with the same parameters is the documented way (change_parameters with the same values reports NO_CHANGE)
    const unsigned ch = m->ch;
    const unsigned long rate = m->st->samplerate;
    ebur128_destroy(&m->st);
    m->st = ebur128_init(ch, rate, to_modes(m->modes));
    if (m->st) for (unsigned i = 0; i < ch; ++i) ebur128_set_channel(m->st, i, m->map[i]);
}

NUE_API nue_result NUE_CALL nue_loudness_add(nue_loudness m, const float* x, uint32_t frames) {
    if (!m || !m->st || (!x && frames)) return NUE_ERR_INVALID_ARGUMENT;
    return ebur128_add_frames_float(m->st, x, frames) == EBUR128_SUCCESS ? NUE_OK : NUE_ERR_OUT_OF_MEMORY;
}

NUE_API double NUE_CALL nue_loudness_momentary(nue_loudness m) { double v = minus_inf(); if (m && m->st) ebur128_loudness_momentary(m->st, &v); return v; }
NUE_API double NUE_CALL nue_loudness_short_term(nue_loudness m) { double v = minus_inf(); if (m && m->st && (m->modes & NUE_LOUDNESS_SHORT_TERM)) ebur128_loudness_shortterm(m->st, &v); return v; }
NUE_API double NUE_CALL nue_loudness_integrated(nue_loudness m) { double v = minus_inf(); if (m && m->st && (m->modes & NUE_LOUDNESS_INTEGRATED)) ebur128_loudness_global(m->st, &v); return v; }
NUE_API double NUE_CALL nue_loudness_range(nue_loudness m) { double v = 0; if (m && m->st && (m->modes & NUE_LOUDNESS_RANGE)) ebur128_loudness_range(m->st, &v); return v; }

NUE_API double NUE_CALL nue_loudness_sample_peak(nue_loudness m) {
    double best = 0;
    if (!m || !m->st || !(m->modes & NUE_LOUDNESS_SAMPLE_PEAK)) return 0;
    for (uint32_t c = 0; c < m->ch; ++c) { double p = 0; if (ebur128_sample_peak(m->st, c, &p) == EBUR128_SUCCESS) best = std::max(best, p); }
    return best;
}

NUE_API double NUE_CALL nue_loudness_true_peak(nue_loudness m) {
    double best = 0;
    if (!m || !m->st || !(m->modes & NUE_LOUDNESS_TRUE_PEAK)) return 0;
    for (uint32_t c = 0; c < m->ch; ++c) { double p = 0; if (ebur128_true_peak(m->st, c, &p) == EBUR128_SUCCESS) best = std::max(best, p); }
    return best;
}

NUE_API nue_result NUE_CALL nue_loudness_analyze(const float* x, uint32_t frames, uint32_t channels, nue_channel_mask mask, uint32_t rate, nue_loudness_result* out) {
    if (!out || !x) return NUE_ERR_INVALID_ARGUMENT;
    nue_loudness m = nullptr;
    nue_result r = nue_loudness_create(channels, mask, rate, NUE_LOUDNESS_INTEGRATED | NUE_LOUDNESS_RANGE | NUE_LOUDNESS_SAMPLE_PEAK | NUE_LOUDNESS_TRUE_PEAK, &m);
    if (NUE_FAILED(r)) return r;
    const uint32_t chunk = 48000;
    for (uint32_t f = 0; f < frames; f += chunk) nue_loudness_add(m, x + size_t(f) * channels, std::min(chunk, frames - f));
    const uint32_t want = out->struct_size ? out->struct_size : uint32_t(sizeof(*out));
    nue_loudness_result res;
    res.struct_size = want;
    res.integrated_lufs = nue_loudness_integrated(m);
    res.range_lu = nue_loudness_range(m);
    res.sample_peak = nue_loudness_sample_peak(m);
    res.true_peak = nue_loudness_true_peak(m);
    std::memcpy(out, &res, std::min<size_t>(want, sizeof(res)));
    nue_loudness_destroy(m);
    return NUE_OK;
}

NUE_API float NUE_CALL nue_loudness_normalization_gain(double integrated_lufs, double true_peak, double target_lufs, double ceiling_dbtp) {
    if (!std::isfinite(integrated_lufs)) return 1.0f;                       // silence or too short: leave it alone
    double gain_db = target_lufs - integrated_lufs;
    if (true_peak > 0.0) {
        const double peak_db = 20.0 * std::log10(true_peak);
        if (peak_db + gain_db > ceiling_dbtp) gain_db = ceiling_dbtp - peak_db;
    }
    return float(std::pow(10.0, gain_db / 20.0));
}

}  // extern "C"
