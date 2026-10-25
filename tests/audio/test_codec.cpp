// Audio codecs: block decoders against reference vectors, streaming decoder (WAV, raw ADPCM, seek, loops), prefetch stream, mixer hook-up.
#include "test_env.h"
#include "nue_test.h"

#include "nue/audio_codec.h"
#include "nue/mixer.h"

#include "adpcm_vectors.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static uint32_t fnv(const int16_t* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        const uint16_t u = uint16_t(s[i]);
        h = (h ^ (u & 0xFF)) * 16777619u;
        h = (h ^ (u >> 8)) * 16777619u;
    }
    return h;
}

// ------------------------------------------------------------------------------------------------------------ block decoders

TEST_CASE(ima_hand_computed_vector) {
    // step 7, index 0, code 7: diff = 0 + 7 + 3 + 1 = 11 -> 11, index 8.  code 0 at step 16: diff 2 -> 13, index 7.
    // code 8 at step 14: -(14>>3 = 1) = -1 -> 12, index 6.  code 15 at step 13: -(1 + 3 + 6 + 13 = 23) -> -11, index 6+8 = 14.
    nue_ima_state s{0, 0};
    CHECK_EQ(nue_ima_decode_nibble(&s, 7), 11);
    CHECK_EQ(s.step_index, 8);
    CHECK_EQ(nue_ima_decode_nibble(&s, 0), 13);
    CHECK_EQ(s.step_index, 7);
    CHECK_EQ(nue_ima_decode_nibble(&s, 8), 12);
    CHECK_EQ(s.step_index, 6);
    CHECK_EQ(nue_ima_decode_nibble(&s, 15), -11);
    CHECK_EQ(s.step_index, 14);
    // saturation
    nue_ima_state hi{32767, 88};
    CHECK_EQ(nue_ima_decode_nibble(&hi, 7), 32767);
    CHECK_EQ(hi.step_index, 88);
    nue_ima_state lo{-32768, 0};
    CHECK_EQ(nue_ima_decode_nibble(&lo, 15), -32768);
}

TEST_CASE(ms_adpcm_hand_computed_block) {
    // mono block, coef pair 0 (256, 0): prediction = sample1. delta 16, s1 = 1000, s2 = 900.
    uint8_t blk[7 + 2] = {0, 16, 0, 0xE8, 0x03, 0x84, 0x03, 0x12, 0xF8};   // nibbles: 1,2,F,8
    int16_t out[2 + 4];
    CHECK_EQ(nue_adpcm_ms_decode_block(blk, sizeof(blk), 1, nullptr, 0, out), 6u);
    CHECK_EQ(out[0], 900);
    CHECK_EQ(out[1], 1000);
    CHECK_EQ(out[2], 1000 + 1 * 16);          // code 1, delta 16 -> 1016; delta = 230*16>>8 = 14 -> 16
    CHECK_EQ(out[3], 1016 + 2 * 16);          // code 2
    CHECK_EQ(out[4], 1048 - 1 * 16);          // code 0xF = -1, delta still 16 (never below 16)
    CHECK_EQ(out[5], 1032 - 8 * 16);          // code 8 = -8
}

TEST_CASE(adpcm_block_vectors_match_reference) {
    std::vector<int16_t> out;
    auto ima = [&](const uint8_t* d, size_t size, uint32_t block, uint32_t ch, uint32_t hash, uint32_t frames) {
        out.clear();
        std::vector<int16_t> tmp(size_t(nue_adpcm_ima_wav_samples_per_block(block, ch)) * ch);
        for (size_t o = 0; o + block <= size; o += block) {
            const uint32_t n = nue_adpcm_ima_wav_decode_block(d + o, block, ch, tmp.data());
            out.insert(out.end(), tmp.begin(), tmp.begin() + size_t(n) * ch);
        }
        CHECK_EQ(out.size() / ch, frames);
        CHECK_EQ(fnv(out.data(), out.size()), hash);
    };
    ima(ima_mono_data, sizeof(ima_mono_data), 36, 1, ima_mono_hash, ima_mono_frames);
    ima(ima_stereo_data, sizeof(ima_stereo_data), 72, 2, ima_stereo_hash, ima_stereo_frames);
    ima(ima_quad_data, sizeof(ima_quad_data), 80, 4, ima_quad_hash, ima_quad_frames);

    auto xbox = [&](const uint8_t* d, size_t size, uint32_t ch, uint32_t hash, uint32_t frames) {
        out.clear();
        std::vector<int16_t> tmp(size_t(64) * ch);
        for (size_t o = 0; o + 36 * ch <= size; o += 36 * ch) {
            CHECK_EQ(nue_adpcm_xbox_decode_block(d + o, ch, tmp.data()), 64u);
            out.insert(out.end(), tmp.begin(), tmp.end());
        }
        CHECK_EQ(out.size() / ch, frames);
        CHECK_EQ(fnv(out.data(), out.size()), hash);
    };
    xbox(xbox_mono_data, sizeof(xbox_mono_data), 1, xbox_mono_hash, xbox_mono_frames);
    xbox(xbox_stereo_data, sizeof(xbox_stereo_data), 2, xbox_stereo_hash, xbox_stereo_frames);

    auto ms = [&](const uint8_t* d, size_t size, uint32_t block, uint32_t ch, uint32_t hash, uint32_t frames) {
        out.clear();
        std::vector<int16_t> tmp(size_t(nue_adpcm_ms_samples_per_block(block, ch)) * ch);
        for (size_t o = 0; o + block <= size; o += block) {
            const uint32_t n = nue_adpcm_ms_decode_block(d + o, block, ch, nullptr, 0, tmp.data());
            CHECK(n != 0);
            out.insert(out.end(), tmp.begin(), tmp.begin() + size_t(n) * ch);
        }
        CHECK_EQ(out.size() / ch, frames);
        CHECK_EQ(fnv(out.data(), out.size()), hash);
    };
    ms(ms_mono_data, sizeof(ms_mono_data), 64, 1, ms_mono_hash, ms_mono_frames);
    ms(ms_stereo_data, sizeof(ms_stereo_data), 128, 2, ms_stereo_hash, ms_stereo_frames);

    out.clear();
    nue_psx_state ps{0, 0, 0};
    for (size_t o = 0; o + 16 <= sizeof(psx_data); o += 16) {
        int16_t f[28];
        nue_adpcm_psx_decode_frame(&ps, psx_data + o, f);
        out.insert(out.end(), f, f + 28);
    }
    CHECK_EQ(out.size(), psx_frames);
    CHECK_EQ(fnv(out.data(), out.size()), psx_hash);

    out.clear();
    nue_dsp_state ds{0, 0};
    for (size_t o = 0; o + 8 <= sizeof(dsp_data); o += 8) {
        int16_t f[14];
        nue_adpcm_dsp_decode_frame(&ds, dsp_coefs, dsp_data + o, f);
        out.insert(out.end(), f, f + 14);
    }
    CHECK_EQ(out.size(), dsp_frames);
    CHECK_EQ(fnv(out.data(), out.size()), dsp_hash);

    out.clear();
    for (size_t o = 0; o + 2 * 0x4c <= sizeof(xas_data); o += 2 * 0x4c) {
        std::vector<int16_t> tmp(256);
        CHECK_EQ(nue_adpcm_ea_xas_decode_block(xas_data + o, 2, tmp.data()), 128u);
        out.insert(out.end(), tmp.begin(), tmp.end());
    }
    CHECK_EQ(out.size() / 2, xas_frames);
    CHECK_EQ(fnv(out.data(), out.size()), xas_hash);

    for (int v = 1; v <= 2; ++v) {
        out.clear();
        nue_ea_xa_state es{0, 0};
        const uint8_t* d = v == 1 ? eaxa1_data : eaxa2_data;
        const size_t n = v == 1 ? sizeof(eaxa1_data) : sizeof(eaxa2_data);
        for (size_t o = 0; o + 15 <= n; o += 15) {
            int16_t f[28];
            nue_adpcm_ea_xa_decode_frame(&es, v == 1 ? NUE_EA_XA_V1 : NUE_EA_XA_V2, d + o, f);
            out.insert(out.end(), f, f + 28);
        }
        CHECK_EQ(out.size(), v == 1 ? eaxa1_frames : eaxa2_frames);
        CHECK_EQ(fnv(out.data(), out.size()), v == 1 ? eaxa1_hash : eaxa2_hash);
    }
    out.clear();
    nue_ea_xa_state l{0, 0}, r{0, 0};
    for (size_t o = 0; o + 30 <= sizeof(eaxa_st_data); o += 30) {
        int16_t a[28], b[28];
        nue_adpcm_ea_xa_decode_stereo_frame(&l, &r, eaxa_st_data + o, a, b);
        for (int i = 0; i < 28; ++i) { out.push_back(a[i]); out.push_back(b[i]); }
    }
    CHECK_EQ(out.size() / 2, eaxa_st_frames);
    CHECK_EQ(fnv(out.data(), out.size()), eaxa_st_hash);
}

TEST_CASE(dsp_nibble_sample_conversions) {
    CHECK_EQ(nue_dsp_samples_to_nibbles(14), 16u);
    CHECK_EQ(nue_dsp_samples_to_nibbles(28), 32u);
    CHECK_EQ(nue_dsp_samples_to_nibbles(15), 16u + 3u);
    CHECK_EQ(nue_dsp_nibbles_to_samples(16), 14u);
    CHECK_EQ(nue_dsp_nibbles_to_samples(19), 15u);
    CHECK_EQ(nue_dsp_nibbles_to_samples(1), 0u);
}

// ------------------------------------------------------------------------------------------------------------ streaming decoder

static std::vector<int16_t> decode_all(nue_adec d, uint32_t channels) {
    std::vector<int16_t> out;
    std::vector<int16_t> buf(size_t(300) * channels);
    for (;;) {
        uint32_t got = 0;
        nue_result r = nue_adec_read_s16(d, buf.data(), 300, &got);
        out.insert(out.end(), buf.begin(), buf.begin() + size_t(got) * channels);
        if (r == NUE_ERR_END_OF_DATA || got < 300) break;
    }
    return out;
}

static nue_adec_raw_desc raw(nue_acodec codec, uint32_t ch, uint32_t rate) {
    nue_adec_raw_desc d;
    NUE_STRUCT_INIT(&d);
    d.codec = codec; d.channels = ch; d.sample_rate = rate;
    return d;
}

TEST_CASE(raw_streams_match_block_decoders_and_seek_exactly) {
    struct Case { const char* name; nue_adec_raw_desc d; const uint8_t* data; size_t size; uint32_t hash; uint32_t frames; };
    nue_adec_dsp_channel dsp;
    std::memset(&dsp, 0, sizeof(dsp));
    std::memcpy(dsp.coef, dsp_coefs, sizeof(dsp.coef));
    Case cases[8];
    int n = 0;
    cases[n++] = {"psx", raw(NUE_ACODEC_PSX_ADPCM, 1, 22050), psx_data, sizeof(psx_data), psx_hash, psx_frames};
    cases[n] = {"dsp", raw(NUE_ACODEC_DSP_ADPCM, 1, 32000), dsp_data, sizeof(dsp_data), dsp_hash, dsp_frames};
    cases[n].d.dsp = &dsp; ++n;
    cases[n++] = {"xas", raw(NUE_ACODEC_EA_XAS, 2, 44100), xas_data, sizeof(xas_data), xas_hash, xas_frames};
    cases[n++] = {"xbox", raw(NUE_ACODEC_IMA_XBOX, 2, 44100), xbox_stereo_data, sizeof(xbox_stereo_data), xbox_stereo_hash, xbox_stereo_frames};
    cases[n] = {"ima", raw(NUE_ACODEC_IMA_WAV, 2, 44100), ima_stereo_data, sizeof(ima_stereo_data), ima_stereo_hash, ima_stereo_frames};
    cases[n].d.block_size = 72; ++n;
    cases[n] = {"ms", raw(NUE_ACODEC_MS_ADPCM, 2, 44100), ms_stereo_data, sizeof(ms_stereo_data), ms_stereo_hash, ms_stereo_frames};
    cases[n].d.block_size = 128; ++n;
    cases[n++] = {"eaxa", raw(NUE_ACODEC_EA_XA, 1, 22050), eaxa1_data, sizeof(eaxa1_data), eaxa1_hash, eaxa1_frames};
    for (int i = 0; i < n; ++i) {
        Case& c = cases[i];
        std::printf("  codec %s\n", c.name);
        nue_adec dec = nullptr;
        CHECK_EQ(nue_adec_open_raw_memory(c.data, c.size, &c.d, &dec), NUE_OK);
        if (!dec) continue;
        nue_adec_info info;
        NUE_STRUCT_INIT(&info);
        CHECK_EQ(nue_adec_get_info(dec, &info), NUE_OK);
        CHECK_EQ(info.frame_count, c.frames);
        CHECK_EQ(info.spec.channels, c.d.channels);
        std::vector<int16_t> all = decode_all(dec, c.d.channels);
        CHECK_EQ(all.size() / c.d.channels, c.frames);
        CHECK_EQ(fnv(all.data(), all.size()), c.hash);
        // seek to positions all over the stream (forward, backward, block boundaries, last frame) and compare with the sequential decode
        const uint32_t ch = c.d.channels;
        const uint64_t spots[] = {0, 1, 27, 28, 29, 100, 255, 256, 257, c.frames / 2, c.frames - 2, 3, c.frames - 1, 5000};
        for (uint64_t at : spots) {
            if (at >= c.frames) continue;
            CHECK_EQ(nue_adec_seek(dec, at), NUE_OK);
            CHECK_EQ(nue_adec_tell(dec), at);
            std::vector<int16_t> got(size_t(50) * ch);
            uint32_t g = 0;
            nue_adec_read_s16(dec, got.data(), 50, &g);
            const uint32_t want = uint32_t(std::min<uint64_t>(50, c.frames - at));
            CHECK_EQ(g, want);
            CHECK(std::memcmp(got.data(), all.data() + size_t(at) * ch, size_t(g) * ch * 2) == 0);
        }
        nue_adec_destroy(dec);
    }
}

TEST_CASE(raw_interleaved_channels_and_partial_reads) {
    // two PSX channels interleaved every 0x10 bytes (one frame): decoded channels equal the two mono decodes
    const size_t frames = sizeof(psx_data) / 16;
    std::vector<uint8_t> left, right;
    for (size_t f = 0; f + 1 < frames; f += 2) {
        left.insert(left.end(), psx_data + f * 16, psx_data + f * 16 + 16);
        right.insert(right.end(), psx_data + (f + 1) * 16, psx_data + (f + 1) * 16 + 16);
    }
    std::vector<uint8_t> stereo;
    for (size_t f = 0; f < left.size() / 16; ++f) {
        stereo.insert(stereo.end(), left.begin() + f * 16, left.begin() + f * 16 + 16);
        stereo.insert(stereo.end(), right.begin() + f * 16, right.begin() + f * 16 + 16);
    }
    nue_adec_raw_desc d = raw(NUE_ACODEC_PSX_ADPCM, 2, 32000);
    d.interleave = 16;
    nue_adec st = nullptr, l = nullptr, r = nullptr;
    CHECK_EQ(nue_adec_open_raw_memory(stereo.data(), stereo.size(), &d, &st), NUE_OK);
    nue_adec_raw_desc m = raw(NUE_ACODEC_PSX_ADPCM, 1, 32000);
    CHECK_EQ(nue_adec_open_raw_memory(left.data(), left.size(), &m, &l), NUE_OK);
    CHECK_EQ(nue_adec_open_raw_memory(right.data(), right.size(), &m, &r), NUE_OK);
    if (!st || !l || !r) return;
    auto a = decode_all(st, 2), b = decode_all(l, 1), c = decode_all(r, 1);
    CHECK_EQ(a.size(), b.size() * 2);
    bool same = a.size() == b.size() * 2;
    for (size_t i = 0; same && i < b.size(); ++i) same = a[2 * i] == b[i] && a[2 * i + 1] == c[i];
    CHECK(same);
    // separate channel areas (interleave 0): left bytes then right bytes
    std::vector<uint8_t> planar = left;
    planar.insert(planar.end(), right.begin(), right.end());
    nue_adec p = nullptr;
    nue_adec_raw_desc pd = raw(NUE_ACODEC_PSX_ADPCM, 2, 32000);
    CHECK_EQ(nue_adec_open_raw_memory(planar.data(), planar.size(), &pd, &p), NUE_OK);
    auto pa = decode_all(p, 2);
    CHECK(pa == a);
    nue_adec_destroy(p); nue_adec_destroy(st); nue_adec_destroy(l); nue_adec_destroy(r);
}

TEST_CASE(loops_wrap_at_loop_end_and_count_down) {
    nue_adec_raw_desc d = raw(NUE_ACODEC_PSX_ADPCM, 1, 22050);
    d.loop_start = 100; d.loop_end = 300;
    nue_adec dec = nullptr;
    CHECK_EQ(nue_adec_open_raw_memory(psx_data, sizeof(psx_data), &d, &dec), NUE_OK);
    if (!dec) return;
    nue_adec_info info;
    NUE_STRUCT_INIT(&info);
    nue_adec_get_info(dec, &info);
    CHECK(info.has_loop);
    CHECK_EQ(info.loop_start, 100u);
    std::vector<int16_t> plain = decode_all(dec, 1);
    CHECK_EQ(nue_adec_seek(dec, 0), NUE_OK);
    CHECK_EQ(nue_adec_set_loop(dec, 2), NUE_OK);                   // loop twice more, then continue to the end
    std::vector<int16_t> looped = decode_all(dec, 1);
    std::vector<int16_t> expect(plain.begin(), plain.begin() + 300);
    for (int k = 0; k < 2; ++k) expect.insert(expect.end(), plain.begin() + 100, plain.begin() + 300);
    expect.insert(expect.end(), plain.begin() + 300, plain.end());
    CHECK(looped == expect);
    // infinite: a 1000 frame read crosses the loop several times and never ends
    CHECK_EQ(nue_adec_seek(dec, 0), NUE_OK);
    CHECK_EQ(nue_adec_set_loop(dec, -1), NUE_OK);
    std::vector<int16_t> big(1000);
    uint32_t got = 0;
    CHECK_EQ(nue_adec_read_s16(dec, big.data(), 1000, &got), NUE_OK);
    CHECK_EQ(got, 1000u);
    for (uint32_t i = 0; i < 700; ++i) CHECK_EQ(big[300 + i], plain[100 + (i % 200)]);
    CHECK_EQ(nue_adec_set_loop(dec, 0), NUE_OK);
    nue_adec_destroy(dec);

    // no loop information -> set_loop refuses, set_loop_points enables it
    nue_adec_raw_desc nd = raw(NUE_ACODEC_PSX_ADPCM, 1, 22050);
    CHECK_EQ(nue_adec_open_raw_memory(psx_data, sizeof(psx_data), &nd, &dec), NUE_OK);
    CHECK_EQ(nue_adec_set_loop(dec, 1), NUE_ERR_BAD_STATE);
    CHECK_EQ(nue_adec_set_loop_points(dec, 10, 20), NUE_OK);
    CHECK_EQ(nue_adec_set_loop(dec, 1), NUE_OK);
    nue_adec_destroy(dec);
}

// WAV builders ---------------------------------------------------------------------------------------------------------------

static void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x & 0xFFFF); put16(v, x >> 16); }
static void putc4(std::vector<uint8_t>& v, const char* s) { v.insert(v.end(), s, s + 4); }

static std::vector<uint8_t> make_wav(uint32_t tag, uint32_t ch, uint32_t rate, uint32_t bits, uint32_t block_align, const std::vector<uint8_t>& extra,
                                     const std::vector<uint8_t>& data, const std::vector<uint8_t>& after_data = {}) {
    std::vector<uint8_t> fmt;
    put16(fmt, tag); put16(fmt, ch); put32(fmt, rate); put32(fmt, rate * block_align / (ch ? 1 : 1)); put16(fmt, block_align); put16(fmt, bits);
    if (!extra.empty() || tag == 0x11 || tag == 0x02) { put16(fmt, uint32_t(extra.size())); fmt.insert(fmt.end(), extra.begin(), extra.end()); }
    std::vector<uint8_t> w;
    putc4(w, "RIFF"); put32(w, 0); putc4(w, "WAVE");
    putc4(w, "fmt "); put32(w, uint32_t(fmt.size())); w.insert(w.end(), fmt.begin(), fmt.end());
    if (fmt.size() & 1) w.push_back(0);
    putc4(w, "data"); put32(w, uint32_t(data.size())); w.insert(w.end(), data.begin(), data.end());
    if (data.size() & 1) w.push_back(0);
    w.insert(w.end(), after_data.begin(), after_data.end());
    const uint32_t riff = uint32_t(w.size() - 8);
    std::memcpy(&w[4], &riff, 4);
    return w;
}

TEST_CASE(wav_pcm_formats_decode_exactly) {
    std::vector<int16_t> pcm;
    for (int i = 0; i < 1000; ++i) pcm.push_back(int16_t((i * 977) % 60000 - 30000));
    std::vector<uint8_t> data(pcm.size() * 2);
    std::memcpy(data.data(), pcm.data(), data.size());
    auto wav = make_wav(1, 2, 44100, 16, 4, {}, data);
    nue_adec dec = nullptr;
    CHECK_EQ(nue_adec_open_memory(wav.data(), wav.size(), &dec), NUE_OK);
    if (!dec) return;
    nue_adec_info info;
    NUE_STRUCT_INIT(&info);
    nue_adec_get_info(dec, &info);
    CHECK_EQ(info.codec, NUE_ACODEC_PCM_S16LE);
    CHECK_EQ(info.spec.channels, 2u);
    CHECK_EQ(info.spec.sample_rate, 44100u);
    CHECK_EQ(info.frame_count, 500u);
    CHECK(decode_all(dec, 2) == pcm);
    nue_adec_destroy(dec);

    // 8 bit unsigned and 24 bit and float
    std::vector<uint8_t> u8 = {0, 128, 255, 64};
    wav = make_wav(1, 1, 8000, 8, 1, {}, u8);
    CHECK_EQ(nue_adec_open_memory(wav.data(), wav.size(), &dec), NUE_OK);
    float f[4]; uint32_t got = 0;
    nue_adec_read_f32(dec, f, 4, &got);
    CHECK_EQ(got, 4u);
    CHECK_NEAR(f[0], -1.0, 1e-6); CHECK_NEAR(f[1], 0.0, 1e-6); CHECK_NEAR(f[2], 127.0 / 128.0, 1e-6); CHECK_NEAR(f[3], -0.5, 1e-6);
    nue_adec_destroy(dec);

    std::vector<uint8_t> s24 = {0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x40};
    wav = make_wav(1, 1, 48000, 24, 3, {}, s24);
    CHECK_EQ(nue_adec_open_memory(wav.data(), wav.size(), &dec), NUE_OK);
    nue_adec_read_f32(dec, f, 3, &got);
    CHECK_NEAR(f[0], -1.0, 1e-7); CHECK_NEAR(f[1], 8388607.0 / 8388608.0, 1e-7); CHECK_NEAR(f[2], 0.5, 1e-7);
    nue_adec_destroy(dec);

    std::vector<uint8_t> f32(8);
    const float fv[2] = {0.25f, -0.75f};
    std::memcpy(f32.data(), fv, 8);
    wav = make_wav(3, 1, 48000, 32, 4, {}, f32);
    CHECK_EQ(nue_adec_open_memory(wav.data(), wav.size(), &dec), NUE_OK);
    nue_adec_read_f32(dec, f, 2, &got);
    CHECK_EQ(f[0], 0.25f); CHECK_EQ(f[1], -0.75f);
    nue_adec_destroy(dec);
}

TEST_CASE(wav_with_adpcm_smpl_loop_and_truncated_data) {
    // IMA ADPCM mono, block 36 (65 frames), 3 blocks; plus a smpl chunk with a loop; the decoded output equals the block decoder.
    std::vector<uint8_t> extra;
    put16(extra, 65);
    std::vector<uint8_t> data(ima_mono_data, ima_mono_data + sizeof(ima_mono_data));
    // smpl layout: manufacturer, product, period, unity note, pitch fraction, smpte format, smpte offset, loop count, sampler data, then loops
    std::vector<uint8_t> chunk;
    putc4(chunk, "smpl"); put32(chunk, 36 + 24);
    for (int i = 0; i < 7; ++i) put32(chunk, 0);
    put32(chunk, 1); put32(chunk, 0);                       // count = 1, sampler data = 0
    put32(chunk, 0); put32(chunk, 0); put32(chunk, 50); put32(chunk, 120); put32(chunk, 0); put32(chunk, 0);   // id, type, start, end, frac, play count
    auto wav = make_wav(0x11, 1, 22050, 4, 36, extra, data, chunk);
    nue_adec dec = nullptr;
    CHECK_EQ(nue_adec_open_memory(wav.data(), wav.size(), &dec), NUE_OK);
    if (!dec) return;
    nue_adec_info info;
    NUE_STRUCT_INIT(&info);
    nue_adec_get_info(dec, &info);
    CHECK_EQ(info.codec, NUE_ACODEC_IMA_WAV);
    CHECK_EQ(info.frame_count, ima_mono_frames);
    CHECK(info.has_loop);
    CHECK_EQ(info.loop_start, 50u);
    CHECK_EQ(info.loop_end, 121u);                          // smpl end is inclusive
    auto all = decode_all(dec, 1);
    CHECK_EQ(fnv(all.data(), all.size()), ima_mono_hash);
    nue_adec_destroy(dec);

    // a data chunk that claims more than the file holds (streamed/truncated wav) plays what is there
    auto cut = make_wav(1, 1, 8000, 16, 2, {}, std::vector<uint8_t>(200, 0));
    cut.resize(cut.size() - 60);
    CHECK_EQ(nue_adec_open_memory(cut.data(), cut.size(), &dec), NUE_OK);
    if (dec) { CHECK_EQ(decode_all(dec, 1).size(), 70u); nue_adec_destroy(dec); }

    // garbage is refused
    uint8_t junk[64] = {1, 2, 3};
    CHECK_EQ(nue_adec_open_memory(junk, sizeof(junk), &dec), NUE_ERR_UNSUPPORTED);
}

TEST_CASE(callback_source_gives_the_same_samples_as_memory) {
    struct Ctx { std::vector<uint8_t> bytes; int reads = 0; bool closed = false; } ctx;
    ctx.bytes.assign(psx_data, psx_data + sizeof(psx_data));
    nue_adec_source s;
    NUE_STRUCT_INIT(&s);
    s.userdata = &ctx;
    s.size = ctx.bytes.size();
    s.read = [](void* ud, uint64_t off, void* dst, size_t n, size_t* got) -> nue_result {
        Ctx* c = static_cast<Ctx*>(ud);
        ++c->reads;
        n = off >= c->bytes.size() ? 0 : std::min<size_t>(n, c->bytes.size() - size_t(off));
        std::memcpy(dst, c->bytes.data() + off, n);
        *got = n;
        return NUE_OK;
    };
    s.close = [](void* ud) { static_cast<Ctx*>(ud)->closed = true; };
    nue_adec_raw_desc d = raw(NUE_ACODEC_PSX_ADPCM, 1, 22050);
    nue_adec dec = nullptr;
    CHECK_EQ(nue_adec_open_raw_source(&s, &d, &dec), NUE_OK);
    if (!dec) return;
    auto a = decode_all(dec, 1);
    CHECK_EQ(fnv(a.data(), a.size()), psx_hash);
    CHECK(ctx.reads >= 1 && ctx.reads < 10);                // the 64 KiB window means the 640 bytes cost very few reads
    nue_adec_destroy(dec);
    CHECK(ctx.closed);
}

TEST_CASE(decode_all_and_prefetch_stream) {
    nue_adec_raw_desc d = raw(NUE_ACODEC_PSX_ADPCM, 1, 22050);
    nue_adec dec = nullptr;
    CHECK_EQ(nue_adec_open_raw_memory(psx_data, sizeof(psx_data), &d, &dec), NUE_OK);
    if (!dec) return;
    int16_t* all = nullptr;
    uint64_t frames = 0;
    CHECK_EQ(nue_adec_decode_all_s16(dec, &all, &frames), NUE_OK);
    CHECK_EQ(frames, psx_frames);
    if (all) { CHECK_EQ(fnv(all, size_t(frames)), psx_hash); nue_free(all); }

    nue_adec_stream s = nullptr;
    CHECK_EQ(nue_adec_stream_create(dec, 4096, &s), NUE_OK);          // takes the decoder
    if (!s) return;
    std::vector<int16_t> got;
    for (int tries = 0; tries < 2000 && got.size() < psx_frames; ++tries) {
        int16_t blk[256];
        const uint32_t n = nue_adec_stream_pull_s16(s, blk, 256);
        if (n < 256 && nue_adec_stream_underruns(s) == 0) { got.insert(got.end(), blk, blk + n); break; }
        got.insert(got.end(), blk, blk + 256);
    }
    // the stream may have started with an underrun (silence) while the worker filled the ring: the tail must still be the sound
    CHECK(got.size() >= psx_frames);
    nue_adec_stream_destroy(s);
}

// ------------------------------------------------------------------------------------------------------------ mixer hook-up

TEST_CASE(decoder_feeds_a_callback_voice) {
    nue_test::Engine engine;
    nue_mixer_desc md;
    NUE_STRUCT_INIT(&md);
    md.flags = NUE_MIXER_MANUAL; md.sample_rate = 22050; md.channels = 1; md.block_frames = 64;
    nue_mixer mx = nullptr;
    CHECK_EQ(nue_mixer_create(&md, &mx), NUE_OK);
    nue_adec_raw_desc d = raw(NUE_ACODEC_PSX_ADPCM, 1, 22050);
    nue_adec dec = nullptr;
    CHECK_EQ(nue_adec_open_raw_memory(psx_data, sizeof(psx_data), &d, &dec), NUE_OK);
    if (!mx || !dec) return;
    nue_voice_desc vd;
    NUE_STRUCT_INIT(&vd);
    vd.kind = NUE_VOICE_CALLBACK; vd.flags = NUE_VOICE_START_PLAYING;
    vd.spec = {NUE_AUDIO_S16, 1, 22050};
    vd.pull = nue_adec_pull_s16; vd.userdata = dec;
    nue_voice_id v = 0;
    CHECK_EQ(nue_voice_create(mx, &vd, &v), NUE_OK);
    std::vector<float> out(psx_frames + 64);
    CHECK_EQ(nue_mixer_render(mx, out.data(), uint32_t(out.size())), NUE_OK);
    nue_adec_seek(dec, 0);
    auto ref = decode_all(dec, 1);
    int bad = 0;
    // the mixer's linear interpolator needs one frame of look-ahead, so the very last source frame of a callback voice is not rendered (noted in docs/REQUESTS_audio.md)
    for (size_t i = 0; i + 1 < ref.size(); ++i) if (std::fabs(out[i] - ref[i] / 32768.0f) > 1e-6f) { if (!bad) std::printf("  first mismatch at %zu: %g vs %g\n", i, out[i], ref[i] / 32768.0f); ++bad; }
    CHECK_EQ(bad, 0);
    CHECK_EQ(out[ref.size() + 10], 0.0f);                   // the voice ended: silence afterwards
    nue_voice_destroy(mx, v);
    nue_adec_destroy(dec);
    nue_mixer_destroy(mx);
}

TEST_MAIN_BEGIN
    RUN(ima_hand_computed_vector);
    RUN(ms_adpcm_hand_computed_block);
    RUN(adpcm_block_vectors_match_reference);
    RUN(dsp_nibble_sample_conversions);
    RUN(raw_streams_match_block_decoders_and_seek_exactly);
    RUN(raw_interleaved_channels_and_partial_reads);
    RUN(loops_wrap_at_loop_end_and_count_down);
    RUN(wav_pcm_formats_decode_exactly);
    RUN(wav_with_adpcm_smpl_loop_and_truncated_data);
    RUN(callback_source_gives_the_same_samples_as_memory);
    RUN(decode_all_and_prefetch_stream);
    RUN(decoder_feeds_a_callback_voice);
TEST_MAIN_END
