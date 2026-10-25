// Channel layout conversion by speaker mask and the one-call HQ converter.
#include "nue/audio_dsp.h"
#include "dsp/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

namespace {

const float kR2 = 0.70710678f;

// Index of a speaker bit within a mask (channels are interleaved in ascending bit order).
int speaker_index(nue_channel_mask mask, nue_channel_mask bit) {
    if (!(mask & bit)) return -1;
    return int(nue_channel_mask_count(mask & (bit - 1)));
}

struct Route { nue_channel_mask dst; float gain; };

}  // namespace

struct nue_chanmix_s {
    uint32_t src_ch = 0, dst_ch = 0;
    std::vector<float> m;       // dst_ch x src_ch
};

extern "C" {

NUE_API uint32_t NUE_CALL nue_channel_mask_count(nue_channel_mask mask) {
    uint32_t n = 0;
    while (mask) { mask &= mask - 1; ++n; }
    return n;
}

NUE_API nue_channel_mask NUE_CALL nue_channel_mask_default(uint32_t channels) {
    switch (channels) {
    case 1: return NUE_MASK_MONO;
    case 2: return NUE_MASK_STEREO;
    case 3: return NUE_SPK_FL | NUE_SPK_FR | NUE_SPK_FC;
    case 4: return NUE_MASK_QUAD;
    case 5: return NUE_MASK_5_0;
    case 6: return NUE_MASK_5_1;
    case 7: return NUE_MASK_6_1;
    case 8: return NUE_MASK_7_1;
    default: return 0;
    }
}

NUE_API nue_result NUE_CALL nue_chanmix_create(nue_channel_mask src, nue_channel_mask dst, nue_chanmix_flags flags, nue_chanmix* out) {
    if (!out || !src || !dst) return NUE_ERR_INVALID_ARGUMENT;
    auto* x = new (std::nothrow) nue_chanmix_s();
    if (!x) return NUE_ERR_OUT_OF_MEMORY;
    x->src_ch = nue_channel_mask_count(src);
    x->dst_ch = nue_channel_mask_count(dst);
    x->m.assign(size_t(x->src_ch) * x->dst_ch, 0.0f);
    auto add = [&](nue_channel_mask s_bit, nue_channel_mask d_bit, float g) {
        const int si = speaker_index(src, s_bit), di = speaker_index(dst, d_bit);
        if (si >= 0 && di >= 0) x->m[size_t(di) * x->src_ch + si] += g;
    };
    const bool dst_mono = (dst & (NUE_SPK_FL | NUE_SPK_FR)) == 0;
    // Each source speaker: direct if the destination has it, otherwise the first applicable fallback below.
    for (nue_channel_mask bit = 1; bit && bit <= NUE_SPK_TBR; bit <<= 1) {
        if (!(src & bit)) continue;
        if (dst & bit) { add(bit, bit, 1.0f); continue; }
        std::vector<Route> routes;
        const bool left = bit == NUE_SPK_FL || bit == NUE_SPK_BL || bit == NUE_SPK_FLC || bit == NUE_SPK_SL || bit == NUE_SPK_TFL || bit == NUE_SPK_TBL;
        const nue_channel_mask front = left ? NUE_SPK_FL : NUE_SPK_FR;
        switch (bit) {
        case NUE_SPK_FC:
            if (dst & NUE_SPK_FLC && dst & NUE_SPK_FRC) routes = {{NUE_SPK_FLC, kR2}, {NUE_SPK_FRC, kR2}};
            else routes = {{NUE_SPK_FL, kR2}, {NUE_SPK_FR, kR2}};
            break;
        case NUE_SPK_FL: case NUE_SPK_FR:
            if (dst_mono) routes = {{NUE_SPK_FC, 1.0f}};
            else routes = {{nue_channel_mask((dst & NUE_SPK_FC) ? NUE_SPK_FC : 0u), kR2}};
            break;
        case NUE_SPK_LFE:
            if (flags & NUE_CHANMIX_INCLUDE_LFE) routes = {{NUE_SPK_FL, 0.3162f}, {NUE_SPK_FR, 0.3162f}, {NUE_SPK_FC, 0.3162f * kR2 * 2}};
            break;
        case NUE_SPK_BL: case NUE_SPK_BR: case NUE_SPK_SL: case NUE_SPK_SR: {
            const bool back = bit == NUE_SPK_BL || bit == NUE_SPK_BR;
            const nue_channel_mask sib_a = back ? (left ? NUE_SPK_SL : NUE_SPK_SR) : (left ? NUE_SPK_BL : NUE_SPK_BR);
            if (dst & sib_a) routes = {{sib_a, 1.0f}};
            else if (dst & NUE_SPK_BC && !dst_mono) routes = {{NUE_SPK_BC, kR2}};
            else if (!dst_mono) routes = {{front, kR2}};
            else routes = {{NUE_SPK_FC, kR2}};
            break;
        }
        case NUE_SPK_BC:
            if ((dst & NUE_SPK_BL) && (dst & NUE_SPK_BR)) routes = {{NUE_SPK_BL, kR2}, {NUE_SPK_BR, kR2}};
            else if ((dst & NUE_SPK_SL) && (dst & NUE_SPK_SR)) routes = {{NUE_SPK_SL, kR2}, {NUE_SPK_SR, kR2}};
            else if (!dst_mono) routes = {{NUE_SPK_FL, 0.5f}, {NUE_SPK_FR, 0.5f}};
            else routes = {{NUE_SPK_FC, 0.5f}};
            break;
        case NUE_SPK_FLC: case NUE_SPK_FRC:
            routes = {{dst_mono ? NUE_SPK_FC : front, 1.0f}};
            break;
        default:                                    // height speakers: fold onto the ear-level front
            if (dst_mono) routes = {{NUE_SPK_FC, kR2}};
            else if (bit == NUE_SPK_TFC) routes = {{nue_channel_mask((dst & NUE_SPK_FC) ? NUE_SPK_FC : NUE_SPK_FL), 1.0f}, {nue_channel_mask((dst & NUE_SPK_FC) ? 0u : NUE_SPK_FR), 1.0f}};
            else routes = {{front, kR2}};
            break;
        }
        for (const Route& r : routes) if (r.dst) add(bit, r.dst, r.gain);
    }
    // Up mix: rear speakers of the destination that nothing feeds (stereo source, 4/5.1/7.1 destination).
    if (flags & NUE_CHANMIX_SURROUND_FILL) {
        const bool src_front_pair = (src & NUE_SPK_FL) && (src & NUE_SPK_FR);
        const nue_channel_mask rears[4] = {NUE_SPK_BL, NUE_SPK_BR, NUE_SPK_SL, NUE_SPK_SR};
        if (src_front_pair && !(src & (NUE_SPK_BL | NUE_SPK_BR | NUE_SPK_SL | NUE_SPK_SR | NUE_SPK_BC))) {
            const int l = speaker_index(src, NUE_SPK_FL), r = speaker_index(src, NUE_SPK_FR);
            for (nue_channel_mask bit : rears) {
                const int di = speaker_index(dst, bit);
                if (di < 0) continue;
                const bool is_left = bit == NUE_SPK_BL || bit == NUE_SPK_SL;
                // (L - R) / 2 for the left side, (R - L) / 2 for the right: ambience without the centre content
                x->m[size_t(di) * x->src_ch + l] += is_left ? 0.5f : -0.5f;
                x->m[size_t(di) * x->src_ch + r] += is_left ? -0.5f : 0.5f;
            }
        }
    }
    // Headroom: a down mix can add up to more than full scale.
    if ((flags & NUE_CHANMIX_NORMALIZE) || dst_mono) {
        float worst = 0;
        for (uint32_t d = 0; d < x->dst_ch; ++d) {
            float sum = 0;
            for (uint32_t s = 0; s < x->src_ch; ++s) sum += std::fabs(x->m[size_t(d) * x->src_ch + s]);
            worst = std::max(worst, sum);
        }
        if (worst > 1.0f) for (float& v : x->m) v /= worst;
    }
    *out = x;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_chanmix_destroy(nue_chanmix m) { delete m; }
NUE_API uint32_t NUE_CALL nue_chanmix_src_channels(nue_chanmix m) { return m ? m->src_ch : 0; }
NUE_API uint32_t NUE_CALL nue_chanmix_dst_channels(nue_chanmix m) { return m ? m->dst_ch : 0; }

NUE_API nue_result NUE_CALL nue_chanmix_get_matrix(nue_chanmix m, float* out, uint32_t capacity) {
    if (!m || !out) return NUE_ERR_INVALID_ARGUMENT;
    if (capacity < m->m.size()) return NUE_ERR_BUFFER_TOO_SMALL;
    std::copy(m->m.begin(), m->m.end(), out);
    return NUE_OK;
}

NUE_API void NUE_CALL nue_chanmix_process(nue_chanmix m, const float* in, uint32_t frames, float* out) {
    if (!m || !in || !out) return;
    const uint32_t sc = m->src_ch, dc = m->dst_ch;
    for (uint32_t f = 0; f < frames; ++f) {
        const float* s = in + size_t(f) * sc;
        float* d = out + size_t(f) * dc;
        for (uint32_t o = 0; o < dc; ++o) {
            const float* row = &m->m[size_t(o) * sc];
            float acc = 0;
            for (uint32_t i = 0; i < sc; ++i) acc += row[i] * s[i];
            d[o] = acc;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------- convert_hq

static uint32_t bps(nue_audio_format f) { return f == NUE_AUDIO_S16 ? 2 : f == NUE_AUDIO_F32 ? 4 : f == NUE_AUDIO_U8 ? 1 : 0; }

static void to_f32(nue_audio_format f, const void* src, size_t n, float* dst) {
    switch (f) {
    case NUE_AUDIO_S16: { const int16_t* p = static_cast<const int16_t*>(src); for (size_t i = 0; i < n; ++i) dst[i] = float(p[i]) * (1.0f / 32768.0f); break; }
    case NUE_AUDIO_U8: { const uint8_t* p = static_cast<const uint8_t*>(src); for (size_t i = 0; i < n; ++i) dst[i] = (float(p[i]) - 128.0f) * (1.0f / 128.0f); break; }
    default: std::memcpy(dst, src, n * sizeof(float)); break;
    }
}

static void from_f32(nue_audio_format f, const float* src, size_t n, void* dst) {
    switch (f) {
    case NUE_AUDIO_S16: {
        int16_t* p = static_cast<int16_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            const float v = src[i] * 32768.0f;
            const float r = v >= 0 ? std::floor(v + 0.5f) : std::ceil(v - 0.5f);
            p[i] = int16_t(r < -32768.0f ? -32768 : (r > 32767.0f ? 32767 : int32_t(r)));
        }
        break;
    }
    case NUE_AUDIO_U8: {
        uint8_t* p = static_cast<uint8_t*>(dst);
        for (size_t i = 0; i < n; ++i) {
            const float v = src[i] * 128.0f + 128.0f;
            const float r = std::floor(v + 0.5f);
            p[i] = uint8_t(r < 0 ? 0 : (r > 255 ? 255 : int32_t(r)));
        }
        break;
    }
    default: std::memcpy(dst, src, n * sizeof(float)); break;
    }
}

NUE_API uint32_t NUE_CALL nue_audio_convert_hq(const nue_audio_spec* ss, nue_channel_mask sm, const void* src, uint32_t src_frames,
                                               const nue_audio_spec* ds, nue_channel_mask dm, void* dst, uint32_t dst_cap,
                                               nue_resample_quality quality, nue_chanmix_flags flags) {
    if (!ss || !ds || !src || !dst || !src_frames || !dst_cap) return 0;
    if (!ss->channels || !ds->channels || ss->channels > NUE_AUDIO_MAX_CHANNELS || ds->channels > NUE_AUDIO_MAX_CHANNELS) return 0;
    if (!bps(ss->format) || !bps(ds->format) || !ss->sample_rate || !ds->sample_rate) return 0;
    if (!sm) sm = nue_channel_mask_default(ss->channels);
    if (!dm) dm = nue_channel_mask_default(ds->channels);
    if (!sm || !dm || nue_channel_mask_count(sm) != ss->channels || nue_channel_mask_count(dm) != ds->channels) return 0;

    std::vector<float> a(size_t(src_frames) * ss->channels);
    to_f32(ss->format, src, a.size(), a.data());
    // mix first when going down (fewer channels to resample), after the resampler when going up
    nue_chanmix mix = nullptr;
    if (sm != dm && NUE_FAILED(nue_chanmix_create(sm, dm, flags, &mix))) return 0;
    const bool down = ds->channels <= ss->channels;
    std::vector<float> b;
    if (mix && down) { b.resize(size_t(src_frames) * ds->channels); nue_chanmix_process(mix, a.data(), src_frames, b.data()); a.swap(b); }
    uint32_t frames = src_frames;
    const uint32_t work_ch = down ? ds->channels : ss->channels;
    if (ss->sample_rate != ds->sample_rate) {
        const uint32_t want = uint32_t(std::floor(double(src_frames) * ds->sample_rate / ss->sample_rate + 0.5));
        b.assign(size_t(std::max<uint32_t>(want, 1)) * work_ch, 0.0f);
        frames = nue_resample_buffer(a.data(), src_frames, work_ch, ss->sample_rate, ds->sample_rate, quality, b.data(), want);
        a.swap(b);
    }
    if (mix && !down) { b.resize(size_t(frames) * ds->channels); nue_chanmix_process(mix, a.data(), frames, b.data()); a.swap(b); }
    nue_chanmix_destroy(mix);
    frames = std::min(frames, dst_cap);
    from_f32(ds->format, a.data(), size_t(frames) * ds->channels, dst);
    return frames;
}

}  // extern "C"
