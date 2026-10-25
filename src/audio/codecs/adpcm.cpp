// Block decoders for the ADPCM families of PC and console games. Pure functions: no allocation, no global state.
//
//   IMA / Microsoft IMA / Xbox : the Interactive Multimedia Association algorithm (public specification, step table below).
//   Microsoft ADPCM            : the RIFF WAVE_FORMAT_ADPCM predictor/delta scheme (public specification).
//   PSX / VAG                  : Sony SPU ADPCM (five fixed filters, public format knowledge, see docs/AUDIO.md).
//   DSP                        : Nintendo GameCube / Wii DSP ADPCM (coefficients stored in the file header).
//   EA XAS / EA-XA             : Electronic Arts ADPCM variants; layout documented in docs/AUDIO.md.
// The algorithms and the edge-case behaviour (filter/shift clamps, XAS group layout, EA-XA tables) were checked against the decoders of
// vgmstream (https://github.com/vgmstream/vgmstream, ISC licence, commit d9875b1; see docs/THIRD_PARTY_AUDIO.md). The code is ours.
#include "nue/audio_codec.h"
#include "internal.hpp"

#include <climits>
#include <cstdint>
#include <cstring>

using nuea::clamp16;
using nuea::rd_le16;
using nuea::rd_le32;

namespace {

const int16_t kImaStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767};
const int8_t kImaIndex[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

inline void ima_init(nue_ima_state* s, int32_t pred, uint8_t idx_byte) {
    const int32_t idx = int8_t(idx_byte);              // headers store a signed byte; out-of-range values clamp
    s->predictor = pred;
    s->step_index = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
}

const nue_ms_adpcm_coef kMsStdCoefs[7] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
const int16_t kMsAdapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};

const int8_t kPsxCoef[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------------------------------------------- IMA

NUE_API int16_t NUE_CALL nue_ima_decode_nibble(nue_ima_state* s, uint32_t code) {
    const int32_t step = kImaStep[s->step_index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += step >> 1;
    if (code & 1) diff += step >> 2;
    if (code & 8) diff = -diff;
    int32_t p = s->predictor + diff;
    s->predictor = p < -32768 ? -32768 : (p > 32767 ? 32767 : p);
    int32_t idx = s->step_index + kImaIndex[code & 7];
    s->step_index = idx < 0 ? 0 : (idx > 88 ? 88 : idx);
    return int16_t(s->predictor);
}

NUE_API uint32_t NUE_CALL nue_adpcm_ima_wav_samples_per_block(uint32_t block_size, uint32_t channels) {
    if (channels == 0 || channels > NUE_AUDIO_MAX_CHANNELS || block_size < 4 * channels) return 0;
    const uint32_t data = block_size - 4 * channels;
    if (data % (4 * channels) != 0) return 0;               // whole 4-byte groups per channel
    return 1 + (data / channels) * 2;
}

// WAV IMA block: [header x channels][4-byte group ch0][4-byte group ch1]...; each group holds 8 nibbles (low nibble first).
NUE_API uint32_t NUE_CALL nue_adpcm_ima_wav_decode_block(const uint8_t* block, uint32_t block_size, uint32_t channels, int16_t* out) {
    const uint32_t frames = nue_adpcm_ima_wav_samples_per_block(block_size, channels);
    if (!frames || !block || !out) return 0;
    nue_ima_state st[NUE_AUDIO_MAX_CHANNELS];
    for (uint32_t c = 0; c < channels; ++c) {
        const uint8_t* h = block + 4 * c;
        ima_init(&st[c], int16_t(rd_le16(h)), h[2]);
        out[c] = int16_t(st[c].predictor);
    }
    const uint8_t* p = block + 4 * channels;
    const uint32_t groups = (block_size - 4 * channels) / (4 * channels);
    for (uint32_t g = 0; g < groups; ++g) {
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t b = 0; b < 4; ++b) {
                const uint8_t v = *p++;
                const uint32_t f = 1 + g * 8 + b * 2;
                out[f * channels + c] = nue_ima_decode_nibble(&st[c], v & 0xF);
                out[(f + 1) * channels + c] = nue_ima_decode_nibble(&st[c], v >> 4);
            }
        }
    }
    return frames;
}

// ---------------------------------------------------------------------------------------------------------------- MS ADPCM

NUE_API uint32_t NUE_CALL nue_adpcm_ms_samples_per_block(uint32_t block_size, uint32_t channels) {
    if (channels == 0 || channels > NUE_AUDIO_MAX_CHANNELS || block_size < 7 * channels) return 0;
    return 2 + (block_size - 7 * channels) * 2 / channels;
}

NUE_API uint32_t NUE_CALL nue_adpcm_ms_decode_block(const uint8_t* block, uint32_t block_size, uint32_t channels,
                                                    const nue_ms_adpcm_coef* coefs, uint32_t coef_count, int16_t* out) {
    const uint32_t frames = nue_adpcm_ms_samples_per_block(block_size, channels);
    if (!frames || !block || !out) return 0;
    if (!coefs) { coefs = kMsStdCoefs; coef_count = 7; }
    int32_t delta[NUE_AUDIO_MAX_CHANNELS], s1[NUE_AUDIO_MAX_CHANNELS], s2[NUE_AUDIO_MAX_CHANNELS];
    nue_ms_adpcm_coef k[NUE_AUDIO_MAX_CHANNELS];
    const uint8_t* p = block;
    for (uint32_t c = 0; c < channels; ++c) {
        const uint32_t idx = *p++;
        if (idx >= coef_count) return 0;
        k[c] = coefs[idx];
    }
    for (uint32_t c = 0; c < channels; ++c) { delta[c] = int16_t(rd_le16(p)); p += 2; }
    for (uint32_t c = 0; c < channels; ++c) { s1[c] = int16_t(rd_le16(p)); p += 2; }
    for (uint32_t c = 0; c < channels; ++c) { s2[c] = int16_t(rd_le16(p)); p += 2; }
    for (uint32_t c = 0; c < channels; ++c) { out[c] = int16_t(s2[c]); out[channels + c] = int16_t(s1[c]); }
    const uint32_t nibbles = (block_size - 7 * channels) * 2;       // across all channels, interleaved in time order
    for (uint32_t n = 0; n < nibbles; ++n) {
        const uint32_t c = n % channels;
        const uint8_t v = p[n >> 1];
        const int32_t code = (n & 1) ? (v & 0xF) : (v >> 4);
        const int32_t signed_code = code >= 8 ? code - 16 : code;
        int32_t pred = ((s1[c] * k[c].c1 + s2[c] * k[c].c2) >> 8) + signed_code * delta[c];
        const int16_t sample = clamp16(pred);
        out[(2 + n / channels) * channels + c] = sample;
        s2[c] = s1[c];
        s1[c] = sample;
        int32_t d = (kMsAdapt[code] * delta[c]) >> 8;
        delta[c] = d < 16 ? 16 : (d > INT32_MAX / 768 ? INT32_MAX / 768 : d);     // noise input can grow delta without bound: cap like ffmpeg
    }
    return frames;
}

// ---------------------------------------------------------------------------------------------------------------- PSX

NUE_API uint8_t NUE_CALL nue_adpcm_psx_decode_frame(nue_psx_state* st, const uint8_t* frame, int16_t* out) {
    uint32_t shift = frame[0] & 0xF;
    uint32_t filter = frame[0] >> 4;
    const uint8_t flag = frame[1];
    if (filter > 4) filter = 0;                         // only five filters exist in the SPU
    if (shift > 12) shift = 9;                          // out-of-range shifts behave like 9 (nocash PSX documentation)
    const int32_t k1 = kPsxCoef[filter][0], k2 = kPsxCoef[filter][1];
    const bool silent = !st->ignore_flags && flag >= 7;  // a frame flagged end+loop is silence; some games store garbage in the flags
    int32_t h1 = st->hist1, h2 = st->hist2;
    for (uint32_t i = 0; i < NUE_PSX_FRAME_SAMPLES; ++i) {
        int32_t s = 0;
        if (!silent) {
            const uint8_t byte = frame[2 + (i >> 1)];
            int32_t nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
            if (nib >= 8) nib -= 16;
            // (nib << (12 - shift)) + ((k1*h1 + k2*h2) >> 6), computed with 8 extra bits like the float reference code
            s = (nib * (1 << (20 - shift)) + 4 * (k1 * h1 + k2 * h2)) >> 8;
            s = s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
        }
        out[i] = int16_t(s);
        h2 = h1; h1 = s;
    }
    st->hist1 = h1; st->hist2 = h2;
    return flag;
}

// ---------------------------------------------------------------------------------------------------------------- DSP

NUE_API void NUE_CALL nue_adpcm_dsp_decode_frame(nue_dsp_state* st, const int16_t coefs[16], const uint8_t* frame, int16_t* out) {
    const uint32_t scale = 1u << (frame[0] & 0xF);
    uint32_t idx = (frame[0] >> 4) & 0xF;
    if (idx > 7) idx = 7;                               // a damaged header: the reference decoder clamps too
    const int32_t c1 = coefs[idx * 2], c2 = coefs[idx * 2 + 1];
    int32_t h1 = st->hist1, h2 = st->hist2;
    for (uint32_t i = 0; i < NUE_DSP_FRAME_SAMPLES; ++i) {
        const uint8_t byte = frame[1 + (i >> 1)];
        int32_t nib = (i & 1) ? (byte & 0xF) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        int32_t s = (nib * int32_t(scale) * 2048 + 1024 + c1 * h1 + c2 * h2) >> 11;
        s = s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
        out[i] = int16_t(s);
        h2 = h1; h1 = s;
    }
    st->hist1 = h1; st->hist2 = h2;
}

NUE_API uint32_t NUE_CALL nue_dsp_nibbles_to_samples(uint32_t nibbles) {
    const uint32_t frames = nibbles / 16, rem = nibbles % 16;
    return frames * 14 + (rem < 2 ? 0 : rem - 2);
}
NUE_API uint32_t NUE_CALL nue_dsp_samples_to_nibbles(uint32_t samples) {
    const uint32_t frames = samples / 14, rem = samples % 14;
    return frames * 16 + (rem ? rem + 2 : 0);
}

// ---------------------------------------------------------------------------------------------------------------- Xbox ADPCM

// Same layout as the RIFF IMA block (all channel headers first, then 4-byte groups per channel in turn) with a fixed 36 bytes per
// channel. The header sample is the first output and the last nibble of the block is dropped (64 frames per block, not 65).
NUE_API uint32_t NUE_CALL nue_adpcm_xbox_decode_block(const uint8_t* block, uint32_t channels, int16_t* out) {
    if (!block || !out || channels == 0 || channels > NUE_AUDIO_MAX_CHANNELS) return 0;
    const uint32_t block_size = NUE_XBOX_ADPCM_BYTES_PER_CHANNEL * channels;
    int16_t tmp[65 * NUE_AUDIO_MAX_CHANNELS];
    if (nue_adpcm_ima_wav_decode_block(block, block_size, channels, tmp) != 65) return 0;
    std::memcpy(out, tmp, size_t(NUE_XBOX_ADPCM_FRAMES_PER_BLOCK) * channels * sizeof(int16_t));
    return NUE_XBOX_ADPCM_FRAMES_PER_BLOCK;
}

// ---------------------------------------------------------------------------------------------------------------- EA XAS v1

// 0x4c bytes per channel: 4 group headers (u32 LE) + 15 rows of 4 bytes (one byte per group, high nibble first). Each group yields its
// two header samples and 30 decoded samples = 32, so 128 per block. The coefficient pairs are the four CD-XA filters; the history
// samples come from the header's upper 12 bits (with 4 bits of shift info).
NUE_API uint32_t NUE_CALL nue_adpcm_ea_xas_decode_block(const uint8_t* block, uint32_t channels, int16_t* out) {
    if (!block || !out || channels == 0 || channels > NUE_AUDIO_MAX_CHANNELS) return 0;
    static const int8_t k[4][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}};    // CD-XA K0/K1 times 64
    for (uint32_t c = 0; c < channels; ++c) {
        const uint8_t* b = block + size_t(c) * NUE_EA_XAS_BYTES_PER_CHANNEL;
        for (uint32_t g = 0; g < 4; ++g) {
            const uint32_t hdr = rd_le32(b + g * 4);
            const uint32_t fi = hdr & 0xF;
            const int32_t k1 = fi < 4 ? k[fi][0] : 0, k2 = fi < 4 ? k[fi][1] : 0;
            int32_t h2 = int16_t(hdr & 0xFFF0);
            int32_t h1 = int16_t((hdr >> 16) & 0xFFF0);
            const uint32_t shift = (hdr >> 16) & 0xF;
            int16_t* o = out + size_t(g) * 32 * channels + c;
            o[0] = int16_t(h2);
            o[channels] = int16_t(h1);
            uint32_t n = 2;
            for (uint32_t row = 0; row < 15; ++row) {
                const uint8_t byte = b[16 + row * 4 + g];
                for (uint32_t half = 0; half < 2; ++half) {
                    const uint32_t nib = half == 0 ? (byte >> 4) : (byte & 0xF);
                    int32_t s = int32_t(int16_t(nib << 12)) >> shift;
                    s = (s * 64 + h1 * k1 + h2 * k2) / 64;       // the reference computes in float and truncates toward zero
                    s = s < -32768 ? -32768 : (s > 32767 ? 32767 : s);
                    o[size_t(n++) * channels] = int16_t(s);
                    h2 = h1; h1 = s;
                }
            }
        }
    }
    return NUE_EA_XAS_FRAMES_PER_BLOCK;
}

}  // extern "C"

// ---------------------------------------------------------------------------------------------------------------- EA-XA

namespace {
const int kEaXaTable[20] = {0, 240, 460, 392, 0, 0, -208, -220, 0, 1, 3, 4, 7, 8, 10, 11, 0, -1, -3, -4};

inline int16_t ea_xa_expand(int32_t nib, int32_t shift, int32_t c1, int32_t c2, int32_t h1, int32_t h2, bool v1) {
    int32_t s = int32_t(uint32_t(nib) << 28) >> shift;      // sign extend the nibble, scale
    s = (s + c1 * h1 + c2 * h2 + (v1 ? 128 : 0)) >> 8;
    return clamp16(s);
}
}  // namespace

extern "C" {

NUE_API void NUE_CALL nue_adpcm_ea_xa_decode_frame(nue_ea_xa_state* st, nue_ea_xa_variant variant, const uint8_t* frame, int16_t* out) {
    const int idx = frame[0] >> 4;
    const int32_t c1 = kEaXaTable[idx], c2 = kEaXaTable[idx + 4];
    const int32_t shift = (frame[0] & 0xF) + 8;
    int32_t h1 = st->hist1, h2 = st->hist2;
    for (uint32_t i = 0; i < NUE_EA_XA_FRAME_SAMPLES; ++i) {
        const uint8_t byte = frame[1 + (i >> 1)];
        const int32_t nib = (i & 1) ? (byte & 0xF) : (byte >> 4);     // high nibble first
        const int16_t s = ea_xa_expand(nib, shift, c1, c2, h1, h2, variant == NUE_EA_XA_V1);
        out[i] = s;
        h2 = h1; h1 = s;
    }
    st->hist1 = h1; st->hist2 = h2;
}

NUE_API void NUE_CALL nue_adpcm_ea_xa_decode_stereo_frame(nue_ea_xa_state* left, nue_ea_xa_state* right, const uint8_t* frame,
                                                          int16_t* out_left, int16_t* out_right) {
    nue_ea_xa_state* st[2] = {left, right};
    int16_t* outs[2] = {out_left, out_right};
    for (int ch = 0; ch < 2; ++ch) {
        const bool hn = ch == 0;                             // left = high nibbles, right = low nibbles
        const int ci = hn ? frame[0] >> 4 : frame[0] & 0xF;
        const int32_t c1 = kEaXaTable[ci], c2 = kEaXaTable[ci + 4];
        const int32_t shift = (hn ? frame[1] >> 4 : frame[1] & 0xF) + 8;
        int32_t h1 = st[ch]->hist1, h2 = st[ch]->hist2;
        for (uint32_t i = 0; i < NUE_EA_XA_FRAME_SAMPLES; ++i) {
            const uint8_t byte = frame[2 + i];
            const int32_t nib = hn ? (byte >> 4) : (byte & 0xF);
            const int16_t s = ea_xa_expand(nib, shift, c1, c2, h1, h2, true);
            outs[ch][i] = s;
            h2 = h1; h1 = s;
        }
        st[ch]->hist1 = h1; st[ch]->hist2 = h2;
    }
}

}  // extern "C"
