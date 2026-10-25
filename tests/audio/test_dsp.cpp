// DSP: resampler, channel mixer and HQ converter (against the engine's cheap converter), dynamics, loudness, reverb, convolution.
#include "test_env.h"
#include "nue_test.h"

#include "nue/audio_dsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static const double kPi = 3.14159265358979323846;

static std::vector<float> sine(double freq, double rate, size_t frames, double amp = 0.5, double phase = 0.0, uint32_t ch = 1) {
    std::vector<float> v(frames * ch);
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < ch; ++c) v[i * ch + c] = float(amp * std::sin(2.0 * kPi * freq * double(i) / rate + phase));
    return v;
}

// Least squares fit of a sine of known frequency (amplitude and phase free), ignoring `skip` frames at both ends.
struct SineFit { double a, b, n, err_pow, sig_pow; };
static SineFit fit_sine(const std::vector<float>& y, double freq, double rate, size_t skip) {
    double cc = 0, ss = 0, cs = 0, yc = 0, ys = 0, n = 0;
    for (size_t i = skip; i + skip < y.size(); ++i) {
        const double w = 2.0 * kPi * freq * double(i) / rate, c = std::cos(w), s = std::sin(w);
        cc += c * c; ss += s * s; cs += c * s; yc += y[i] * c; ys += y[i] * s; n += 1;
    }
    const double det = cc * ss - cs * cs;
    SineFit f{(yc * ss - ys * cs) / det, (ys * cc - yc * cs) / det, n, 0, 0};
    for (size_t i = skip; i + skip < y.size(); ++i) {
        const double w = 2.0 * kPi * freq * double(i) / rate;
        const double d = y[i] - (f.a * std::cos(w) + f.b * std::sin(w));
        f.err_pow += d * d; f.sig_pow += double(y[i]) * y[i];
    }
    return f;
}

// SNR of `y` against the best fitting sine of that frequency, in dB.
static double snr_db(const std::vector<float>& y, double freq, double rate, size_t skip) {
    const SineFit f = fit_sine(y, freq, rate, skip);
    return 10.0 * std::log10(f.sig_pow / std::max(f.err_pow, 1e-30));
}

static double amp_at(const std::vector<float>& y, double freq, double rate, size_t skip) {
    const SineFit f = fit_sine(y, freq, rate, skip);
    return std::sqrt(f.a * f.a + f.b * f.b);
}

static uint32_t hash_f32(const std::vector<float>& v) {
    uint32_t h = 2166136261u;
    for (float f : v) { uint32_t u; std::memcpy(&u, &f, 4); for (int i = 0; i < 4; ++i) h = (h ^ ((u >> (8 * i)) & 0xFF)) * 16777619u; }
    return h;
}

// ------------------------------------------------------------------------------------------------------------ resampler

TEST_CASE(resampler_accuracy_by_quality_and_against_the_linear_converter) {
    const double in_rate = 48000, out_rate = 44100;
    const size_t n = 48000;
    const std::vector<float> in = sine(1000.0, in_rate, n);
    const double floor_db[4] = {50, 70, 90, 100};
    double prev = 0;
    for (int q = 0; q < 4; ++q) {
        std::vector<float> out(size_t(out_rate * 1.01));
        const uint32_t got = nue_resample_buffer(in.data(), uint32_t(n), 1, in_rate, out_rate, nue_resample_quality(q), out.data(), uint32_t(out.size()));
        CHECK_EQ(got, 44100u);
        out.resize(got);
        const double snr = snr_db(out, 1000.0, out_rate, 600);
        std::printf("  quality %d: SNR %.1f dB\n", q, snr);
        CHECK(snr > floor_db[q]);
        CHECK(snr > prev);
        prev = snr;
    }
    // the engine converter (linear interpolation)
    nue_audio_spec ss = {NUE_AUDIO_F32, 1, 48000}, ds = {NUE_AUDIO_F32, 1, 44100};
    std::vector<float> lin(44100);
    const uint32_t lg = nue_audio_convert(&ss, in.data(), uint32_t(n), &ds, lin.data(), 44100);
    lin.resize(lg);
    const double lin_snr = snr_db(lin, 1000.0, out_rate, 600);
    std::printf("  linear converter: SNR %.1f dB\n", lin_snr);
    CHECK(prev > lin_snr + 40.0);
}

TEST_CASE(resampler_passband_stopband_and_alias_rejection) {
    // 96000 -> 48000: a tone at 0.8 of the output Nyquist must pass, a tone just above the output Nyquist must be gone
    const size_t n = 96000;
    for (int q = 1; q < 4; ++q) {
        const double pass_f = 0.75 * 24000.0, stop_f = 30000.0;
        std::vector<float> out(48500);
        auto run = [&](double f) {
            auto in = sine(f, 96000, n, 0.5);
            const uint32_t got = nue_resample_buffer(in.data(), uint32_t(n), 1, 96000, 48000, nue_resample_quality(q), out.data(), uint32_t(out.size()));
            std::vector<float> o(out.begin(), out.begin() + got);
            double e = 0;
            for (size_t i = 2000; i + 2000 < o.size(); ++i) e += double(o[i]) * o[i];
            return std::sqrt(2.0 * e / double(o.size() - 4000));
        };
        const double pass = run(pass_f), stop = run(stop_f);
        std::printf("  quality %d: passband %.4f (want 0.5), stopband %.2e\n", q, pass, stop);
        CHECK_NEAR(pass, 0.5, 0.01);
        CHECK(stop < 0.5 * std::pow(10.0, -(q == 1 ? 70.0 : q == 2 ? 85.0 : 100.0) / 20.0));
    }
}

TEST_CASE(resampler_streaming_is_independent_of_chunking_and_survives_rate_changes) {
    const size_t n = 20000;
    auto in = sine(440.0, 44100, n, 0.5, 0.0, 2);
    std::vector<float> whole(size_t(n * 2 * 1.2) + 64), parts(whole.size());
    nue_resampler r1 = nullptr, r2 = nullptr;
    CHECK_EQ(nue_resampler_create(2, 44100, 48000, NUE_RESAMPLE_HIGH, &r1), NUE_OK);
    CHECK_EQ(nue_resampler_create(2, 44100, 48000, NUE_RESAMPLE_HIGH, &r2), NUE_OK);
    uint32_t used = 0, made_whole = 0;
    nue_resampler_process(r1, in.data(), uint32_t(n), &used, whole.data(), uint32_t(whole.size() / 2), &made_whole);
    CHECK_EQ(used, uint32_t(n));
    // same input in odd-sized chunks
    size_t pos = 0, outpos = 0;
    uint32_t chunk = 7;
    while (pos < n) {
        const uint32_t take = uint32_t(std::min<size_t>(chunk, n - pos));
        uint32_t u = 0, m = 0;
        nue_resampler_process(r2, &in[pos * 2], take, &u, &parts[outpos * 2], uint32_t((parts.size() - outpos * 2) / 2), &m);
        CHECK_EQ(u, take);
        pos += take; outpos += m;
        chunk = chunk * 3 % 509 + 1;
    }
    CHECK_EQ(uint32_t(outpos), made_whole);
    CHECK(std::memcmp(whole.data(), parts.data(), size_t(made_whole) * 2 * sizeof(float)) == 0);
    CHECK(nue_resampler_latency_frames(r1) > 30);
    nue_resampler_destroy(r1);

    // ratio glide: no discontinuity larger than the signal's own slope while the ratio moves 1.0 -> 1.5 and back
    nue_resampler r3 = nullptr;
    CHECK_EQ(nue_resampler_create(1, 48000, 48000, NUE_RESAMPLE_MEDIUM, &r3), NUE_OK);
    auto tone = sine(500.0, 48000, 48000, 0.5);
    std::vector<float> out;
    size_t ip = 0;
    for (int blk = 0; blk < 200; ++blk) {
        const double ratio = 1.0 + 0.5 * std::sin(blk * 0.05) * std::sin(blk * 0.05);
        nue_resampler_set_rates(r3, 48000.0 * ratio, 48000.0);
        float tmp[300];
        uint32_t u = 0, m = 0;
        nue_resampler_process(r3, &tone[ip], 240, &u, tmp, 240, &m);
        ip += u;
        out.insert(out.end(), tmp, tmp + m);
        if (ip + 400 > tone.size()) break;
    }
    double max_jump = 0;
    for (size_t i = 1; i < out.size(); ++i) max_jump = std::max(max_jump, double(std::fabs(out[i] - out[i - 1])));
    // 500 Hz * 1.5 at 48 kHz, amplitude 0.5: max slope ~ 0.5 * 2*pi*750/48000 = 0.049
    CHECK(max_jump < 0.06);
    nue_resampler_destroy(r3);
    nue_resampler_destroy(r2);
}

// ------------------------------------------------------------------------------------------------------------ channel mixer

static std::vector<float> matrix(nue_channel_mask s, nue_channel_mask d, nue_chanmix_flags f, uint32_t* sc = nullptr, uint32_t* dc = nullptr) {
    nue_chanmix m = nullptr;
    CHECK_EQ(nue_chanmix_create(s, d, f, &m), NUE_OK);
    std::vector<float> mat(size_t(nue_chanmix_src_channels(m)) * nue_chanmix_dst_channels(m));
    nue_chanmix_get_matrix(m, mat.data(), uint32_t(mat.size()));
    if (sc) *sc = nue_chanmix_src_channels(m);
    if (dc) *dc = nue_chanmix_dst_channels(m);
    nue_chanmix_destroy(m);
    return mat;
}

TEST_CASE(channel_mixer_matrices) {
    const float r2 = 0.70710678f;
    auto m = matrix(NUE_MASK_STEREO, NUE_MASK_MONO, 0);                      // stereo -> mono: average (normalised)
    CHECK_NEAR(m[0], 0.5, 1e-6); CHECK_NEAR(m[1], 0.5, 1e-6);
    m = matrix(NUE_MASK_MONO, NUE_MASK_STEREO, 0);                           // mono -> stereo: constant power
    CHECK_NEAR(m[0], r2, 1e-6); CHECK_NEAR(m[1], r2, 1e-6);
    uint32_t sc, dc;
    m = matrix(NUE_MASK_5_1, NUE_MASK_STEREO, 0, &sc, &dc);                   // 5.1 -> stereo, BS.775 gains: FL FR FC LFE BL BR
    CHECK_EQ(sc, 6u); CHECK_EQ(dc, 2u);
    CHECK_NEAR(m[0 * 6 + 0], 1.0, 1e-6); CHECK_NEAR(m[0 * 6 + 1], 0.0, 1e-6); CHECK_NEAR(m[0 * 6 + 2], r2, 1e-6);
    CHECK_NEAR(m[0 * 6 + 3], 0.0, 1e-6); CHECK_NEAR(m[0 * 6 + 4], r2, 1e-6); CHECK_NEAR(m[0 * 6 + 5], 0.0, 1e-6);
    CHECK_NEAR(m[1 * 6 + 1], 1.0, 1e-6); CHECK_NEAR(m[1 * 6 + 5], r2, 1e-6);
    m = matrix(NUE_MASK_5_1, NUE_MASK_STEREO, NUE_CHANMIX_NORMALIZE);          // normalised: no output can exceed full scale
    float row0 = 0;
    for (int i = 0; i < 6; ++i) row0 += std::fabs(m[i]);
    CHECK_NEAR(row0, 1.0, 1e-5);
    m = matrix(NUE_MASK_5_1, NUE_MASK_STEREO, NUE_CHANMIX_INCLUDE_LFE);
    CHECK(m[3] > 0.3f && m[3] < 0.33f);
    m = matrix(NUE_MASK_7_1, NUE_MASK_5_1, 0, &sc, &dc);                       // 7.1 -> 5.1: sides fold into the rears
    CHECK_NEAR(m[4 * 8 + 6], 1.0, 1e-6); CHECK_NEAR(m[5 * 8 + 7], 1.0, 1e-6); CHECK_NEAR(m[4 * 8 + 4], 1.0, 1e-6);
    m = matrix(NUE_MASK_STEREO, NUE_MASK_5_1, 0, &sc, &dc);                    // stereo -> 5.1: fronts only
    CHECK_NEAR(m[0 * 2 + 0], 1.0, 1e-6); CHECK_NEAR(m[1 * 2 + 1], 1.0, 1e-6);
    for (int i = 2; i < 6; ++i) { CHECK_NEAR(m[i * 2], 0.0, 1e-6); CHECK_NEAR(m[i * 2 + 1], 0.0, 1e-6); }
    m = matrix(NUE_MASK_STEREO, NUE_MASK_5_1, NUE_CHANMIX_SURROUND_FILL);      // with fill: (L-R)/2 on the left rear, (R-L)/2 on the right rear
    CHECK_NEAR(m[4 * 2 + 0], 0.5, 1e-6); CHECK_NEAR(m[4 * 2 + 1], -0.5, 1e-6);
    CHECK_NEAR(m[5 * 2 + 0], -0.5, 1e-6); CHECK_NEAR(m[5 * 2 + 1], 0.5, 1e-6);
    m = matrix(NUE_MASK_QUAD, NUE_MASK_STEREO, 0);                              // quad -> stereo: rears onto the fronts at -3 dB
    CHECK_NEAR(m[0], 1.0, 1e-6); CHECK_NEAR(m[2], r2, 1e-6); CHECK_NEAR(m[4 + 3], r2, 1e-6);
    m = matrix(NUE_MASK_7_1, NUE_MASK_MONO, 0, &sc, &dc);                       // anything -> mono stays within full scale
    float sum = 0;
    for (float v : m) sum += std::fabs(v);
    CHECK(sum <= 1.0001f);
    nue_chanmix bad = nullptr;
    CHECK_EQ(nue_chanmix_create(0, NUE_MASK_STEREO, 0, &bad), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_channel_mask_count(NUE_MASK_7_1), 8u);
    CHECK_EQ(nue_channel_mask_default(6), nue_channel_mask(NUE_MASK_5_1));
}

TEST_CASE(convert_hq_agrees_with_the_engine_converter_where_they_overlap) {
    // same rate, format conversion only: identical results
    std::vector<int16_t> pcm(2000);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t((i * 4099) % 60000 - 30000);
    nue_audio_spec s16 = {NUE_AUDIO_S16, 2, 44100}, f32 = {NUE_AUDIO_F32, 2, 44100};
    std::vector<float> a(2000), b(2000);
    CHECK_EQ(nue_audio_convert(&s16, pcm.data(), 1000, &f32, a.data(), 1000), 1000u);
    CHECK_EQ(nue_audio_convert_hq(&s16, 0, pcm.data(), 1000, &f32, 0, b.data(), 1000, NUE_RESAMPLE_HIGH, 0), 1000u);
    CHECK(a == b);
    // F32 -> S16 rounding agrees as well
    std::vector<int16_t> r1(2000), r2(2000);
    nue_audio_convert(&f32, a.data(), 1000, &s16, r1.data(), 1000);
    nue_audio_convert_hq(&f32, 0, a.data(), 1000, &s16, 0, r2.data(), 1000, NUE_RESAMPLE_HIGH, 0);
    CHECK(r1 == pcm);
    CHECK(r2 == pcm);
    // mono -> stereo at the same rate: both use constant power
    nue_audio_spec mono = {NUE_AUDIO_F32, 1, 44100};
    std::vector<float> st1(2000), st2(2000), m(1000);
    for (size_t i = 0; i < m.size(); ++i) m[i] = float(std::sin(double(i) * 0.1));
    nue_audio_convert(&mono, m.data(), 1000, &f32, st1.data(), 1000);
    nue_audio_convert_hq(&mono, 0, m.data(), 1000, &f32, 0, st2.data(), 1000, NUE_RESAMPLE_HIGH, 0);
    double err = 0;
    for (size_t i = 0; i < 2000; ++i) err = std::max(err, double(std::fabs(st1[i] - st2[i])));
    CHECK(err < 1e-6);
    // 48 kHz 5.1 S16 -> 44.1 kHz stereo F32: the frame count is the exact ratio and the tone survives with the BS.775 gain
    nue_audio_spec s51 = {NUE_AUDIO_S16, 6, 48000}, dst = {NUE_AUDIO_F32, 2, 44100};
    std::vector<int16_t> in(size_t(48000) * 6, 0);
    for (size_t i = 0; i < 48000; ++i) in[i * 6 + 0] = int16_t(8000.0 * std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0));      // FL only
    std::vector<float> out(size_t(44100) * 2 + 8);
    const uint32_t got = nue_audio_convert_hq(&s51, 0, in.data(), 48000, &dst, 0, out.data(), 44100, NUE_RESAMPLE_HIGH, 0);
    CHECK_EQ(got, 44100u);
    std::vector<float> left(got), right(got);
    for (uint32_t i = 0; i < got; ++i) { left[i] = out[i * 2]; right[i] = out[i * 2 + 1]; }
    CHECK_NEAR(amp_at(left, 1000.0, 44100, 1000), 8000.0 / 32768.0, 0.002);
    CHECK(amp_at(right, 1000.0, 44100, 1000) < 1e-5);
    CHECK_EQ(nue_audio_convert_hq(&s51, 0, in.data(), 48000, &dst, NUE_MASK_QUAD, out.data(), 100, NUE_RESAMPLE_HIGH, 0), 0u);     // mask / count mismatch
}

// ------------------------------------------------------------------------------------------------------------ dynamics

static double rms_db(const float* x, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; ++i) e += double(x[i]) * x[i];
    return 10.0 * std::log10(e / double(n) + 1e-30);
}

TEST_CASE(compressor_static_curve_attack_and_release) {
    nue_compressor_desc d;
    nue_compressor_desc_default(&d, 1, 48000);
    d.threshold_db = -18; d.ratio = 3; d.knee_db = 0; d.attack_ms = 2; d.release_ms = 100;
    nue_compressor c = nullptr;
    CHECK_EQ(nue_compressor_create(&d, &c), NUE_OK);
    // a sine peaking at -6 dBFS is 12 dB over: output 12 * (1 - 1/3) = 8 dB lower
    auto x = sine(1000.0, 48000, 48000, std::pow(10.0, -6.0 / 20.0));
    nue_compressor_process(c, x.data(), uint32_t(x.size()));
    double peak = 0;
    for (size_t i = 24000; i < x.size(); ++i) peak = std::max(peak, double(std::fabs(x[i])));
    CHECK_NEAR(20.0 * std::log10(peak), -6.0 - 8.0, 0.4);
    CHECK_NEAR(nue_compressor_gain_reduction_db(c), -8.0, 0.4);
    // release: after the loud part a quiet signal (below threshold) comes back to unity with the release time constant
    std::vector<float> quiet(48000, 0.0f);
    for (size_t i = 0; i < quiet.size(); ++i) quiet[i] = float(0.01 * std::sin(2.0 * kPi * 1000.0 * double(i) / 48000.0));
    std::vector<float> ref = quiet;
    nue_compressor_process(c, quiet.data(), uint32_t(quiet.size()));
    // ratio of gains at 1 release constant (100 ms) into the quiet part: remaining reduction ~ 8 dB * exp(-1)
    double g100 = 0, w = 0;
    for (size_t i = 4700; i < 4900; ++i) { g100 += std::fabs(quiet[i]); w += std::fabs(ref[i]); }
    const double red_db = -20.0 * std::log10(g100 / w);
    CHECK_NEAR(red_db, 8.0 * std::exp(-1.0), 1.2);
    for (size_t i = 40000; i < 48000; ++i) { CHECK(std::fabs(quiet[i] - ref[i]) < 1e-4); }
    nue_compressor_destroy(c);

    // below the threshold nothing changes, bit for bit
    nue_compressor_desc_default(&d, 2, 48000);
    CHECK_EQ(nue_compressor_create(&d, &c), NUE_OK);
    auto lowlevel = sine(300.0, 48000, 4800, 0.05, 0.0, 2);
    auto copy = lowlevel;
    nue_compressor_process(c, lowlevel.data(), 4800);
    CHECK(lowlevel == copy);
    nue_compressor_destroy(c);
}

TEST_CASE(limiter_never_exceeds_the_ceiling_and_is_transparent_below_it) {
    const double ceiling_db = -3.0, ceil_lin = std::pow(10.0, ceiling_db / 20.0);
    nue_limiter_desc d;
    nue_limiter_desc_default(&d, 2, 48000);
    d.ceiling_db = float(ceiling_db); d.lookahead_ms = 5; d.release_ms = 100;
    nue_limiter l = nullptr;
    CHECK_EQ(nue_limiter_create(&d, &l), NUE_OK);
    const uint32_t lat = nue_limiter_latency_frames(l);
    CHECK(lat >= 200 && lat <= 260);
    // loud noise bursts with peaks of 4.0 around quiet sine
    std::vector<float> x(48000 * 2);
    uint32_t s = 1;
    for (size_t i = 0; i < 48000; ++i) {
        const bool burst = (i / 4000) % 3 == 1;
        for (int c = 0; c < 2; ++c) {
            s = s * 1664525u + 1013904223u;
            const float noise = float(int32_t(s)) * (1.0f / 2147483648.0f);
            x[i * 2 + c] = burst ? 4.0f * noise : 0.2f * float(std::sin(0.05 * double(i) + c));
        }
    }
    auto y = x;
    nue_limiter_process(l, y.data(), 48000);
    double peak = 0;
    for (float v : y) peak = std::max(peak, double(std::fabs(v)));
    std::printf("  limiter peak %.5f (ceiling %.5f)\n", peak, ceil_lin);
    CHECK(peak <= ceil_lin * 1.0001);
    CHECK(peak > ceil_lin * 0.9);                       // it does reach the ceiling, it is not just muting
    CHECK(nue_limiter_gain_reduction_db(l) < -3.0f);
    nue_limiter_destroy(l);

    // signal under the ceiling comes out delayed by the latency and otherwise untouched
    CHECK_EQ(nue_limiter_create(&d, &l), NUE_OK);
    auto quiet = sine(440.0, 48000, 9600, 0.3, 0.0, 2);
    auto ref = quiet;
    nue_limiter_process(l, quiet.data(), 9600);
    bool same = true;
    for (size_t i = size_t(lat) * 2; i < quiet.size(); ++i) if (quiet[i] != ref[i - size_t(lat) * 2]) same = false;
    CHECK(same);
    nue_limiter_destroy(l);

    // true peak mode keeps the reconstructed signal under the ceiling: use a tone at fs/4 whose samples sit 45 degrees off the crest
    nue_limiter_desc_default(&d, 1, 48000);
    d.ceiling_db = -1.0f; d.flags = NUE_LIMITER_TRUE_PEAK;
    CHECK_EQ(nue_limiter_create(&d, &l), NUE_OK);
    auto t = sine(12000.0, 48000, 24000, 1.2, kPi / 4.0);
    nue_limiter_process(l, t.data(), 24000);
    std::vector<float> up(24000 * 4 + 64);
    const uint32_t got = nue_resample_buffer(t.data() + 6000, 12000, 1, 48000, 192000, NUE_RESAMPLE_HIGH, up.data(), 48000);
    double tp = 0;
    for (uint32_t i = 600; i + 600 < got; ++i) tp = std::max(tp, double(std::fabs(up[i])));
    std::printf("  true-peak mode: reconstructed peak %.1f dBFS (ceiling -1.0)\n", 20.0 * std::log10(tp));
    CHECK(20.0 * std::log10(tp) < -1.0 + 0.3);
    nue_limiter_destroy(l);
}

TEST_CASE(dynamic_range_modes_squeeze_the_range) {
    // 1 s at -40 dBFS then 1 s at -8 dBFS (rms): a TV mode narrows the 32 dB difference a lot, speakers much less
    auto make = [] {
        std::vector<float> x;
        auto a = sine(500.0, 48000, 48000, std::pow(10.0, -43.0 / 20.0)), b = sine(500.0, 48000, 48000, std::pow(10.0, -11.0 / 20.0));
        x.insert(x.end(), a.begin(), a.end());
        x.insert(x.end(), b.begin(), b.end());
        return x;
    };
    const double in_range = 32.0;
    double range[5];
    for (int mode = NUE_DRC_OFF; mode <= NUE_DRC_NIGHT; ++mode) {
        nue_drc d = nullptr;
        CHECK_EQ(nue_drc_create(1, 48000, nue_drc_mode(mode), &d), NUE_OK);
        auto x = make();
        for (size_t o = 0; o < x.size(); o += 480) nue_drc_process(d, &x[o], uint32_t(std::min<size_t>(480, x.size() - o)));
        range[mode] = rms_db(&x[48000 + 24000], 20000) - rms_db(&x[24000 + 480], 20000);
        std::printf("  mode %d: range %.1f dB (input %.0f)\n", mode, range[mode], in_range);
        nue_drc_destroy(d);
    }
    CHECK_NEAR(range[NUE_DRC_OFF], in_range, 0.5);
    CHECK(range[NUE_DRC_SPEAKERS] < range[NUE_DRC_OFF] - 3.0);
    CHECK(range[NUE_DRC_TV] < range[NUE_DRC_SPEAKERS] - 6.0);
    CHECK(range[NUE_DRC_NIGHT] < range[NUE_DRC_TV]);
    nue_drc d = nullptr;
    CHECK_EQ(nue_drc_create(2, 48000, NUE_DRC_TV, &d), NUE_OK);
    CHECK_EQ(nue_drc_set_mode(d, NUE_DRC_NIGHT), NUE_OK);
    CHECK(nue_drc_latency_frames(d) > 0);
    nue_drc_destroy(d);
}

// ------------------------------------------------------------------------------------------------------------ loudness

TEST_CASE(loudness_meter_follows_ebu_r128) {
    // stereo 1 kHz sine, peak -20 dBFS on both channels: mean square 0.005 per channel -> 10 log10(2 * 0.005) = -20.0 LUFS
    const double amp = std::pow(10.0, -20.0 / 20.0);
    auto x = sine(1000.0, 48000, 48000 * 12, amp, 0.0, 2);
    nue_loudness_result r;
    NUE_STRUCT_INIT(&r);
    CHECK_EQ(nue_loudness_analyze(x.data(), 48000 * 12, 2, 0, 48000, &r), NUE_OK);
    const double expect = 10.0 * std::log10(2.0 * amp * amp / 2.0);          // the K-weighting gain at 1 kHz (+0.69 dB) cancels the -0.691 offset
    std::printf("  integrated %.2f LUFS (expect %.2f), range %.2f LU, peaks %.4f / %.4f\n", r.integrated_lufs, expect, r.range_lu, r.sample_peak, r.true_peak);
    CHECK_NEAR(r.integrated_lufs, expect, 0.05);
    CHECK_NEAR(r.sample_peak, amp, 1e-4);
    CHECK(r.range_lu < 0.5);
    // +6.02 dB doubles the amplitude
    auto y = x;
    for (float& v : y) v *= 2.0f;
    nue_loudness_result r2;
    NUE_STRUCT_INIT(&r2);
    nue_loudness_analyze(y.data(), 48000 * 12, 2, 0, 48000, &r2);
    CHECK_NEAR(r2.integrated_lufs - r.integrated_lufs, 6.0206, 0.02);
    // true peak: a sine at fs/4 sampled 45 degrees off the crest has a sample peak 3 dB below the real peak
    auto t = sine(12000.0, 48000, 48000 * 2, 0.8, kPi / 4.0);
    nue_loudness_result r3;
    NUE_STRUCT_INIT(&r3);
    nue_loudness_analyze(t.data(), 48000 * 2, 1, 0, 48000, &r3);
    std::printf("  fs/4 tone: sample peak %.3f true peak %.3f\n", r3.sample_peak, r3.true_peak);
    CHECK_NEAR(r3.sample_peak, 0.8 * std::sin(kPi / 4.0), 0.01);
    CHECK(r3.true_peak > 0.77 && r3.true_peak < 0.83);
    // normalisation gain: to -23 LUFS, limited by a -1 dBTP ceiling
    const float g = nue_loudness_normalization_gain(-30.0, 0.1, -23.0, -1.0);
    CHECK_NEAR(20.0 * std::log10(g), 7.0, 1e-3);
    const float gl = nue_loudness_normalization_gain(-30.0, 0.5, -23.0, -1.0);        // peak at -6 dB: only 5 dB of room
    CHECK_NEAR(20.0 * std::log10(gl), 5.0, 0.05);
    CHECK_EQ(nue_loudness_normalization_gain(-INFINITY, 0.5, -23.0, -1.0), 1.0f);
    // streaming meter: momentary appears after 400 ms, silence reads -inf
    nue_loudness m = nullptr;
    CHECK_EQ(nue_loudness_create(2, 0, 48000, NUE_LOUDNESS_ALL, &m), NUE_OK);
    CHECK(std::isinf(nue_loudness_momentary(m)));
    nue_loudness_add(m, x.data(), 48000);
    CHECK_NEAR(nue_loudness_momentary(m), expect, 0.2);
    nue_loudness_reset(m);
    CHECK(std::isinf(nue_loudness_momentary(m)));
    nue_loudness_destroy(m);
}

// ------------------------------------------------------------------------------------------------------------ reverb

// T60 from a response by Schroeder backward integration: twice the time between -5 and -35 dB.
static double t60_of(const std::vector<float>& h, double rate) {
    std::vector<double> edc(h.size());
    double acc = 0;
    for (size_t i = h.size(); i-- > 0;) { acc += double(h[i]) * h[i]; edc[i] = acc; }
    const double e0 = edc[0];
    double t5 = -1, t35 = -1;
    for (size_t i = 0; i < edc.size(); ++i) {
        const double db = 10.0 * std::log10(edc[i] / e0 + 1e-30);
        if (t5 < 0 && db <= -5.0) t5 = double(i) / rate;
        if (t35 < 0 && db <= -35.0) { t35 = double(i) / rate; break; }
    }
    return t5 < 0 || t35 < 0 ? -1.0 : 2.0 * (t35 - t5);
}

static std::vector<float> impulse_response(nue_reverb r, uint32_t channels, size_t frames, uint32_t out_channel) {
    std::vector<float> buf(frames * channels, 0.0f);
    for (uint32_t c = 0; c < channels; ++c) buf[c] = 1.0f;
    for (size_t o = 0; o < frames; o += 256) nue_reverb_process(r, &buf[o * channels], uint32_t(std::min<size_t>(256, frames - o)));
    std::vector<float> h(frames);
    for (size_t i = 0; i < frames; ++i) h[i] = buf[i * channels + out_channel];
    return h;
}

TEST_CASE(reverb_decay_time_matches_the_parameter) {
    for (float rt : {0.5f, 1.5f, 3.0f}) {
        nue_reverb r = nullptr;
        CHECK_EQ(nue_reverb_create(1, 0, 48000, &r), NUE_OK);
        nue_reverb_params p;
        nue_reverb_params_default(&p);
        p.decay_time = rt; p.decay_hf_ratio = 1.0f; p.gain = 1.0f; p.gain_hf = 1.0f; p.late_gain = 1.0f; p.reflections_gain = 0.0f;
        nue_reverb_set_params(r, &p);
        auto h = impulse_response(r, 1, size_t(48000 * (rt * 1.6 + 1.0)), 0);
        const double t60 = t60_of(h, 48000);
        std::printf("  decay_time %.2f -> measured T60 %.2f s\n", rt, t60);
        CHECK(t60 > rt * 0.95 && t60 < rt * 1.05);
        nue_reverb_destroy(r);
    }
}

TEST_CASE(reverb_high_frequency_ratio_and_stability) {
    auto tail_hf_share = [](float ratio) {
        nue_reverb r = nullptr;
        nue_reverb_create(1, 0, 48000, &r);
        nue_reverb_params p;
        nue_reverb_params_default(&p);
        p.decay_time = 2.0f; p.decay_hf_ratio = ratio; p.gain = 1.0f; p.gain_hf = 1.0f; p.late_gain = 1.0f; p.reflections_gain = 0.0f; p.hf_reference = 4000.0f;
        nue_reverb_set_params(r, &p);
        auto h = impulse_response(r, 1, 48000 * 3, 0);
        nue_reverb_destroy(r);
        // share of the energy above ~6 kHz (first difference ~ high-pass) in the window 0.8 .. 1.4 s
        double hi = 0, all = 0;
        for (size_t i = size_t(0.8 * 48000); i < size_t(1.4 * 48000); ++i) { const double d = double(h[i]) - h[i - 1]; hi += d * d; all += double(h[i]) * h[i]; }
        return hi / std::max(all, 1e-30);
    };
    const double flat = tail_hf_share(1.0f), dark = tail_hf_share(0.3f);
    std::printf("  tail high-frequency share: ratio 1.0 -> %.3f, ratio 0.3 -> %.3f\n", flat, dark);
    CHECK(dark < flat * 0.6);

    // stability: loud noise, then silence for 20 s with a long decay: finite, and below -60 dB of the input at the end
    nue_reverb r = nullptr;
    CHECK_EQ(nue_reverb_create(2, 0, 44100, &r), NUE_OK);
    nue_reverb_params p;
    nue_reverb_params_default(&p);
    p.decay_time = 8.0f; p.modulation_depth = 1.0f; p.echo_depth = 1.0f; p.gain = 1.0f; p.gain_hf = 1.0f;
    nue_reverb_set_params(r, &p);
    std::vector<float> blk(512 * 2);
    uint32_t s = 7;
    bool finite = true;
    double last_e = 0;
    for (int b = 0; b < 44100 * 30 / 512; ++b) {
        for (float& v : blk) { s = s * 1664525u + 1013904223u; v = b < 40 ? float(int32_t(s)) * (1.0f / 2147483648.0f) : 0.0f; }
        nue_reverb_process(r, blk.data(), 512);
        for (float v : blk) if (!std::isfinite(v)) finite = false;
        if (b > 44100 * 29 / 512) { last_e = 0; for (float v : blk) last_e += double(v) * v; }
        if (b == 60) { p.decay_time = 2.0f; nue_reverb_set_params(r, &p); }          // parameter change while running
    }
    CHECK(finite);
    CHECK(10.0 * std::log10(last_e / 1024.0 + 1e-30) < -60.0);
    nue_reverb_destroy(r);
}

TEST_CASE(reverb_surround_stereo_image_and_determinism) {
    // 5.1: the LFE stays silent, front L/R tails are decorrelated
    nue_reverb r = nullptr;
    CHECK_EQ(nue_reverb_create(6, NUE_MASK_5_1, 48000, &r), NUE_OK);
    std::vector<float> buf(size_t(48000) * 6, 0.0f);
    for (int c = 0; c < 6; ++c) buf[c] = 1.0f;
    for (size_t o = 0; o < 48000; o += 256) nue_reverb_process(r, &buf[o * 6], 256 <= 48000 - o ? 256 : uint32_t(48000 - o));
    double lfe = 0, e0 = 0, e1 = 0, cross = 0;
    for (size_t i = 0; i < 48000; ++i) {
        lfe += std::fabs(buf[i * 6 + 3]);
        const double a = buf[i * 6 + 0], b = buf[i * 6 + 1];
        e0 += a * a; e1 += b * b; cross += a * b;
    }
    CHECK_EQ(lfe, 0.0);
    CHECK(e0 > 0 && e1 > 0);
    CHECK(std::fabs(cross) / std::sqrt(e0 * e1) < 0.5);
    nue_reverb_destroy(r);

    auto run = []() {
        nue_reverb r = nullptr;
        nue_reverb_create(2, 0, 48000, &r);
        nue_i3dl2_reverb pre;
        nue_i3dl2_preset_by_name("Concert Hall", &pre);
        nue_reverb_params p;
        nue_reverb_params_from_i3dl2(&p, &pre);
        nue_reverb_set_params(r, &p);
        std::vector<float> out(48000 * 2, 0.0f);
        for (int i = 0; i < 2000; ++i) out[size_t(i) * 2] = float(std::sin(i * 0.3)) * 0.5f;
        for (size_t o = 0; o < 48000; o += 480) nue_reverb_process(r, &out[o * 2], 480);
        nue_reverb_destroy(r);
        return out;
    };
    const auto a = run(), b = run();
    CHECK_EQ(hash_f32(a), hash_f32(b));
    CHECK(hash_f32(a) != hash_f32(std::vector<float>(a.size(), 0.0f)));
}

TEST_CASE(i3dl2_presets_and_unit_conversions) {
    CHECK_EQ(NUE_I3DL2_PRESET_COUNT, 30);
    CHECK_STR(nue_i3dl2_preset_name(0), "Default");
    CHECK_STR(nue_i3dl2_preset_name(29), "Plate");
    CHECK(nue_i3dl2_preset_name(30) == nullptr);
    nue_i3dl2_reverb hall;
    CHECK_EQ(nue_i3dl2_preset_by_name("concert_hall", &hall), NUE_OK);
    CHECK_EQ(hall.room, -1000);
    CHECK_EQ(hall.room_hf, -500);
    CHECK_NEAR(hall.decay_time, 3.92, 1e-6);
    CHECK_EQ(hall.reflections, -1230);
    CHECK_EQ(nue_i3dl2_preset_by_name("nonexistent", &hall), NUE_ERR_NOT_FOUND);
    // every preset converts to a valid, in-range parameter set
    for (uint32_t i = 0; i < NUE_I3DL2_PRESET_COUNT; ++i) {
        nue_i3dl2_reverb v;
        CHECK_EQ(nue_i3dl2_preset(i, &v), NUE_OK);
        nue_reverb_params p;
        nue_reverb_params_from_i3dl2(&p, &v);
        CHECK(p.gain > 0 && p.gain <= 1.0f);
        CHECK(p.late_gain > 0 && p.late_gain <= 10.0f);
        CHECK(p.decay_time >= 0.1f && p.decay_time <= 20.0f);
        CHECK(p.diffusion >= 0 && p.diffusion <= 1.0f);
        nue_reverb r = nullptr;
        CHECK_EQ(nue_reverb_create(2, 0, 48000, &r), NUE_OK);
        nue_reverb_set_params(r, &p);
        std::vector<float> x(2400 * 2, 0.0f);
        x[0] = 1.0f; x[1] = 1.0f;
        nue_reverb_process(r, x.data(), 2400);
        for (float f : x) CHECK(std::isfinite(f));
        nue_reverb_destroy(r);
    }
    CHECK_NEAR(nue_millibel_to_gain(-600), 0.5012, 1e-3);
    CHECK_NEAR(nue_millibel_to_gain(0), 1.0, 1e-6);
    nue_eax_reverb e;
    std::memset(&e, 0, sizeof(e));
    e.room = -1000; e.room_hf = -100; e.room_lf = 0; e.decay_time = 1.49f; e.decay_hf_ratio = 0.83f; e.decay_lf_ratio = 1.0f; e.reflections = -2602;
    e.reflections_delay = 0.007f; e.reverb = 200; e.reverb_delay = 0.011f; e.echo_time = 0.25f; e.modulation_time = 0.25f; e.air_absorption_hf = -5.0f;
    e.hf_reference = 5000; e.lf_reference = 250; e.diffusion = 100; e.density = 100; e.flags = 0x20;
    nue_reverb_params p;
    nue_reverb_params_from_eax(&p, &e);
    nue_i3dl2_reverb def;
    nue_i3dl2_preset(0, &def);
    nue_reverb_params q;
    nue_reverb_params_from_i3dl2(&q, &def);
    CHECK_NEAR(p.gain, q.gain, 1e-6);
    CHECK_NEAR(p.late_gain, q.late_gain, 1e-6);
    CHECK_NEAR(p.air_absorption_gain_hf, 0.994, 1e-3);
    CHECK(p.decay_hf_limit);
}

TEST_CASE(reverb_runs_as_a_mixer_bus_effect_deterministically) {
    nue_test::Engine engine;
    auto render = []() {
        nue_mixer_desc md;
        NUE_STRUCT_INIT(&md);
        md.flags = NUE_MIXER_MANUAL; md.sample_rate = 48000; md.channels = 2; md.block_frames = 256;
        nue_mixer mx = nullptr;
        nue_mixer_create(&md, &mx);
        nue_reverb rv = nullptr;
        nue_reverb_create(2, 0, 48000, &rv);
        nue_bus_id bus = 0;
        nue_bus_create(mx, 0, &bus);
        nue_effect_desc fx;
        NUE_STRUCT_INIT(&fx);
        fx.kind = NUE_EFFECT_CALLBACK; fx.callback = nue_reverb_dsp; fx.userdata = rv; fx.wet_dry = 1.0f;
        nue_bus_set_effect(mx, bus, 0, &fx);
        std::vector<int16_t> click(400);
        for (size_t i = 0; i < click.size(); ++i) click[i] = int16_t(12000.0 * std::sin(double(i) * 0.4) * (1.0 - double(i) / 400.0));
        nue_buffer_desc bd;
        NUE_STRUCT_INIT(&bd);
        bd.spec = {NUE_AUDIO_S16, 1, 48000}; bd.data = click.data(); bd.frame_count = 400; bd.copy_data = NUE_TRUE;
        nue_buffer_id buf = 0;
        nue_buffer_create(mx, &bd, &buf);
        nue_voice_desc vd;
        NUE_STRUCT_INIT(&vd);
        vd.kind = NUE_VOICE_STATIC; vd.buffer = buf; vd.bus = bus; vd.flags = NUE_VOICE_START_PLAYING | NUE_VOICE_AUTO_RELEASE;
        nue_voice_id v = 0;
        nue_voice_create(mx, &vd, &v);
        std::vector<float> out(48000 * 2);
        nue_mixer_render(mx, out.data(), 48000);
        nue_reverb_destroy(rv);
        nue_mixer_destroy(mx);
        return out;
    };
    const auto a = render(), b = render();
    CHECK_EQ(hash_f32(a), hash_f32(b));
    double tail = 0;
    for (size_t i = 24000 * 2; i < a.size(); ++i) tail += std::fabs(a[i]);
    CHECK(tail > 0.01);                                 // a tail exists half a second after the 8 ms click ended
}

// ------------------------------------------------------------------------------------------------------------ convolution

TEST_CASE(convolver_matches_direct_convolution) {
    // a delayed, scaled impulse is a delay and a gain
    {
        std::vector<float> ir(1000, 0.0f);
        ir[100] = 0.5f;
        nue_convolver c = nullptr;
        CHECK_EQ(nue_convolver_create(1, 48000, ir.data(), 1000, 1, 48000, 256, &c), NUE_OK);
        CHECK_EQ(nue_convolver_latency_frames(c), 256u);
        std::vector<float> x(4096, 0.0f);
        for (size_t i = 0; i < x.size(); ++i) x[i] = float(std::sin(double(i) * 0.05));
        auto y = x;
        for (size_t o = 0; o < y.size(); o += 100) nue_convolver_process(c, &y[o], uint32_t(std::min<size_t>(100, y.size() - o)));
        double err = 0;
        for (size_t i = 356; i < x.size(); ++i) err = std::max(err, double(std::fabs(y[i] - 0.5f * x[i - 356])));
        CHECK(err < 1e-5);
        nue_convolver_destroy(c);
    }
    // random 3000 tap IR, several partitions, compared with the O(n*m) sum in double
    {
        std::vector<float> ir(3000);
        uint32_t s = 99;
        for (float& v : ir) { s = s * 1664525u + 1013904223u; v = float(int32_t(s)) * (1.0f / 2147483648.0f) * 0.1f; }
        nue_convolver c = nullptr;
        CHECK_EQ(nue_convolver_create(1, 48000, ir.data(), 3000, 1, 48000, 512, &c), NUE_OK);
        std::vector<float> x(8000);
        for (float& v : x) { s = s * 1664525u + 1013904223u; v = float(int32_t(s)) * (1.0f / 2147483648.0f); }
        auto y = x;
        nue_convolver_process(c, y.data(), 8000);
        double err = 0;
        for (size_t i = 512; i < 8000; ++i) {
            double acc = 0;
            for (size_t k = 0; k < 3000 && k + 512 <= i; ++k) acc += double(ir[k]) * x[i - 512 - k];
            err = std::max(err, std::fabs(acc - y[i]));
        }
        std::printf("  3000 tap IR: max error %.2e\n", err);
        CHECK(err < 1e-4);
        nue_convolver_reset(c);
        nue_convolver_destroy(c);
    }
    // an IR at another sample rate is resampled; a room IR from the generator has the requested decay
    {
        std::vector<float> room(48000 * 2 * 2);
        const uint32_t frames = nue_ir_generate_room(1.0f, 0.5f, 44100, 5, room.data(), 48000 * 2);
        CHECK(frames > 40000);
        nue_convolver c = nullptr;
        CHECK_EQ(nue_convolver_create(2, 48000, room.data(), frames, 2, 44100, 512, &c), NUE_OK);
        std::vector<float> x(48000 * 2 * 2, 0.0f);
        x[0] = x[1] = 1.0f;
        for (size_t o = 0; o < 48000; o += 480) nue_convolver_process(c, &x[o * 2], 480);
        std::vector<float> left(48000);
        for (size_t i = 0; i < 48000; ++i) left[i] = x[i * 2];
        const double t60 = t60_of(left, 48000);
        std::printf("  generated room IR (decay 1.0 s) through the convolver: T60 %.2f\n", t60);
        CHECK(t60 > 0.7 && t60 < 1.3);
        nue_convolver_destroy(c);
    }
    nue_convolver bad = nullptr;
    float one = 1.0f;
    CHECK_EQ(nue_convolver_create(1, 48000, &one, 1, 1, 48000, 100, &bad), NUE_ERR_INVALID_ARGUMENT);    // block size must be a power of two
}

TEST_MAIN_BEGIN
    RUN(resampler_accuracy_by_quality_and_against_the_linear_converter);
    RUN(resampler_passband_stopband_and_alias_rejection);
    RUN(resampler_streaming_is_independent_of_chunking_and_survives_rate_changes);
    RUN(channel_mixer_matrices);
    RUN(convert_hq_agrees_with_the_engine_converter_where_they_overlap);
    RUN(compressor_static_curve_attack_and_release);
    RUN(limiter_never_exceeds_the_ceiling_and_is_transparent_below_it);
    RUN(dynamic_range_modes_squeeze_the_range);
    RUN(loudness_meter_follows_ebu_r128);
    RUN(reverb_decay_time_matches_the_parameter);
    RUN(reverb_high_frequency_ratio_and_stability);
    RUN(reverb_surround_stereo_image_and_determinism);
    RUN(i3dl2_presets_and_unit_conversions);
    RUN(reverb_runs_as_a_mixer_bus_effect_deterministically);
    RUN(convolver_matches_direct_convolution);
TEST_MAIN_END
