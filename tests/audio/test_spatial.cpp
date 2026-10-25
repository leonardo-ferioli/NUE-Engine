// Spatial rendering: scene maths, panning on several layouts, binaural model, HRTF set, ambisonics, backend hook.
#include "nue_test.h"

#include "nue/audio_spatial.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

static const double kPi = 3.14159265358979323846;

static nue_spatial_world world() { nue_spatial_world w; nue_spatial_world_default(&w); return w; }
static nue_spatial_source_params src_at(float x, float y, float z) { nue_spatial_source_params p; nue_spatial_source_params_default(&p); p.position = {x, y, z}; return p; }

TEST_CASE(scene_maths_distance_cone_doppler_and_listener_pose) {
    nue_spatial_world w = world();
    nue_spatial_source_params p = src_at(5, 0, 0);
    nue_spatial_eval e;
    CHECK_EQ(nue_spatial_evaluate(&w, &p, &e), NUE_OK);
    CHECK_NEAR(e.distance, 5.0, 1e-5);
    CHECK_NEAR(e.azimuth_deg, 90.0, 1e-3);
    CHECK_NEAR(e.elevation_deg, 0.0, 1e-3);
    CHECK_NEAR(e.distance_gain, 1.0 / 5.0, 1e-5);                     // inverse: min / (min + rolloff (d - min))
    p.model = NUE_ATTEN_LINEAR; p.max_distance = 9;
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.distance_gain, 1.0 - 4.0 / 8.0, 1e-5);
    p.model = NUE_ATTEN_EXPONENTIAL; p.rolloff = 2;
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.distance_gain, std::pow(5.0, -2.0), 1e-5);
    p.model = NUE_ATTEN_NONE;
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_EQ(e.distance_gain, 1.0f);
    // inside the minimum distance: full gain; elevation
    p = src_at(0, 1, 1);
    p.min_distance = 10;
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_EQ(e.distance_gain, 1.0f);
    CHECK_NEAR(e.elevation_deg, 45.0, 1e-3);
    // listener turned to face +x: a source at +x is now straight ahead; left-handed: up x forward = right
    w.listener.forward = {1, 0, 0};
    p = src_at(5, 0, 0);
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.azimuth_deg, 0.0, 1e-3);
    p = src_at(0, 0, -5);                                             // the listener's right is now -z
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.azimuth_deg, 90.0, 1e-3);
    // cone: 90 degree inner, 180 outer, outer gain 0.1; the source points along +x
    w = world();
    p = src_at(0, 0, 5);
    p.cone_direction = {1, 0, 0};
    p.cone_inner_deg = 90; p.cone_outer_deg = 180; p.cone_outer_gain = 0.1f;
    nue_spatial_evaluate(&w, &p, &e);                                  // the listener is at 90 degrees off the axis (full angle 180): outer
    CHECK_NEAR(e.cone_gain, 0.1, 1e-4);
    p.position = {5, 0, 0};                                            // on the axis behind the listener's position: the listener is on the axis
    p.cone_direction = {-1, 0, 0};
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.cone_gain, 1.0, 1e-5);
    p.position = {5, 0, 5};                                            // 45 degrees off the axis: full angle 90 = inner edge
    p.cone_direction = {-1, 0, 0};
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.cone_gain, 1.0, 1e-3);
    // Doppler: a source approaching at 34.3 m/s raises the pitch by 343 / (343 - 34.3), receding lowers it
    p = src_at(0, 0, 100);
    p.velocity = {0, 0, -34.3f};
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.doppler_ratio, 343.0 / (343.0 - 34.3), 1e-4);
    p.velocity = {0, 0, 34.3f};
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.doppler_ratio, 343.0 / (343.0 + 34.3), 1e-4);
    w.listener.velocity = {0, 0, 34.3f};                               // listener running toward the (receding) source at the same speed
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(e.doppler_ratio, (343.0 + 34.3) / (343.0 + 34.3), 1e-4);
    w.doppler_factor = 0;
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_EQ(e.doppler_ratio, 1.0f);
    // occlusion gain
    p = src_at(0, 0, 1);
    p.occlusion = 0.5f;
    nue_spatial_evaluate(&w, &p, &e);
    CHECK_NEAR(20.0 * std::log10(e.occlusion_gain), -20.0, 1e-3);
}

static std::vector<float> render_const(nue_spatial s, const nue_spatial_world& w, const nue_spatial_source_params& p, uint32_t blocks, uint32_t frames, float level = 1.0f) {
    nue_spatial_source src = nullptr;
    nue_spatial_source_create(s, &src);
    const uint32_t ch = nue_spatial_output_channels(s);
    std::vector<float> all;
    std::vector<float> in(frames, level), out(size_t(frames) * ch);
    for (uint32_t b = 0; b < blocks; ++b) {
        CHECK_EQ(nue_spatial_begin_block(s, &w, out.data(), frames), NUE_OK);
        CHECK_EQ(nue_spatial_render_source(src, &p, in.data()), NUE_OK);
        CHECK_EQ(nue_spatial_end_block(s), NUE_OK);
        all.insert(all.end(), out.begin(), out.end());
    }
    nue_spatial_source_destroy(src);
    return all;
}

static std::vector<double> channel_power(const std::vector<float>& x, uint32_t ch, size_t from) {
    std::vector<double> p(ch, 0.0);
    const size_t frames = x.size() / ch;
    for (size_t f = from; f < frames; ++f)
        for (uint32_t c = 0; c < ch; ++c) p[c] += double(x[f * ch + c]) * x[f * ch + c];
    for (double& v : p) v /= double(frames - from);
    return p;
}

static nue_spatial make(nue_spatial_mode m, nue_channel_mask mask, uint32_t order = 1) {
    nue_spatial_desc d;
    NUE_STRUCT_INIT(&d);
    d.mode = m; d.mask = mask; d.sample_rate = 48000; d.ambisonic_order = order;
    nue_spatial s = nullptr;
    CHECK_EQ(nue_spatial_create(&d, &s), NUE_OK);
    return s;
}

TEST_CASE(panning_is_constant_power_and_lands_on_the_right_speakers) {
    nue_spatial_world w = world();
    // stereo
    nue_spatial st = make(NUE_SPATIAL_PAN, 0);
    for (float az = -180; az <= 180; az += 15) {
        nue_spatial_source_params p = src_at(std::sin(az * float(kPi) / 180.0f), 0, std::cos(az * float(kPi) / 180.0f));
        auto out = render_const(st, w, p, 2, 64);
        auto pw = channel_power(out, 2, 64);
        CHECK_NEAR(pw[0] + pw[1], 1.0, 1e-3);
        if (az > 10 && az < 170) CHECK(pw[1] > pw[0]);
        if (az < -10 && az > -170) CHECK(pw[0] > pw[1]);
    }
    auto hard = channel_power(render_const(st, w, src_at(1, 0, 0), 2, 64), 2, 64);
    CHECK(hard[0] < 1e-6 && hard[1] > 0.99);
    nue_spatial_destroy(st);

    // 5.1 : FL FR FC LFE BL BR
    nue_spatial s51 = make(NUE_SPATIAL_PAN, NUE_MASK_5_1);
    auto front = channel_power(render_const(s51, w, src_at(0, 0, 3), 2, 64, 3.0f), 6, 64);   // inverse model: gain 1/3 at 3 units
    CHECK(front[2] > 0.99);
    for (int c : {0, 1, 3, 4, 5}) CHECK(front[c] < 1e-6);
    auto right = channel_power(render_const(s51, w, src_at(std::sin(0.3f), 0, std::cos(0.3f)), 2, 64), 6, 64);      // 17 degrees: between FC and FR
    CHECK(right[2] > 0 && right[1] > 0);
    CHECK(right[0] < 1e-6 && right[4] < 1e-6 && right[5] < 1e-6);
    for (float az = 0; az < 360; az += 10) {
        const float a = az * float(kPi) / 180.0f;
        auto pw = channel_power(render_const(s51, w, src_at(std::sin(a) * 4, 0, std::cos(a) * 4), 2, 64, 4.0f), 6, 64);
        double total = 0;
        for (double v : pw) total += v;
        // distance gain at 4 units is 0.25 and the input level is 4: unity again
        CHECK_NEAR(total, 1.0, 5e-3);
        CHECK(pw[3] == 0.0);                                                  // LFE is never panned to
    }
    // spread fills the speakers
    nue_spatial_source_params wide = src_at(0, 0, 3);
    wide.spread_deg = 180;
    auto spread = channel_power(render_const(s51, w, wide, 2, 64, 3.0f), 6, 64);
    for (int c : {0, 1, 2, 4, 5}) CHECK(spread[c] > 0.1);
    nue_spatial_destroy(s51);

    // 7.1 : a source at 90 degrees goes to the side speaker
    nue_spatial s71 = make(NUE_SPATIAL_PAN, NUE_MASK_7_1);
    auto side = channel_power(render_const(s71, w, src_at(2, 0, 0), 2, 64, 2.0f), 8, 64);
    CHECK(side[7] > 0.99);
    nue_spatial_destroy(s71);

    // gains glide between blocks: a source jumping from hard left to hard right does not click
    nue_spatial st2 = make(NUE_SPATIAL_PAN, 0);
    nue_spatial_source src = nullptr;
    nue_spatial_source_create(st2, &src);
    std::vector<float> in(256, 1.0f), out(512);
    nue_spatial_source_params pl = src_at(-1, 0, 0), pr = src_at(1, 0, 0);
    nue_spatial_begin_block(st2, &w, out.data(), 256); nue_spatial_render_source(src, &pl, in.data()); nue_spatial_end_block(st2);
    nue_spatial_begin_block(st2, &w, out.data(), 256); nue_spatial_render_source(src, &pr, in.data()); nue_spatial_end_block(st2);
    double max_step = 0;
    for (size_t f = 1; f < 256; ++f) max_step = std::max(max_step, double(std::fabs(out[f * 2 + 1] - out[(f - 1) * 2 + 1])));
    CHECK(max_step < 0.01);
    nue_spatial_source_destroy(src);
    nue_spatial_destroy(st2);
}

// time lag (in samples, + = b later than a) maximising the cross-correlation
static int best_lag(const std::vector<float>& x, uint32_t ch, int max_lag) {
    double best = -1e30;
    int bl = 0;
    const size_t frames = x.size() / ch;
    for (int lag = -max_lag; lag <= max_lag; ++lag) {
        double c = 0;
        for (size_t f = 200; f + 200 < frames; ++f) c += double(x[f * ch]) * x[(f + lag) * ch + 1];
        if (c > best) { best = c; bl = lag; }
    }
    return bl;
}

TEST_CASE(binaural_model_has_interaural_delay_and_head_shadow) {
    nue_spatial_world w = world();
    nue_spatial b = make(NUE_SPATIAL_BINAURAL, 0);
    nue_spatial_source_params p = src_at(1, 0, 0);                           // hard right
    // broadband noise
    nue_spatial_source src = nullptr;
    nue_spatial_source_create(b, &src);
    std::vector<float> all;
    uint32_t seed = 3;
    for (int blk = 0; blk < 40; ++blk) {
        std::vector<float> in(256), out(512);
        for (float& v : in) { seed = seed * 1664525u + 1013904223u; v = float(int32_t(seed)) * (1.0f / 2147483648.0f) * 0.5f; }
        nue_spatial_begin_block(b, &w, out.data(), 256);
        nue_spatial_render_source(src, &p, in.data());
        nue_spatial_end_block(b);
        all.insert(all.end(), out.begin(), out.end());
    }
    // for a right source the right ear (channel 1) leads: the left ear signal is the right one delayed by ~0.66 ms = 31 samples
    std::vector<float> swapped(all.size());
    for (size_t f = 0; f < all.size() / 2; ++f) { swapped[f * 2] = all[f * 2 + 1]; swapped[f * 2 + 1] = all[f * 2]; }      // a = right, b = left
    const int lag = best_lag(swapped, 2, 60);
    std::printf("  interaural delay: %d samples (%.2f ms)\n", lag, lag / 48.0);
    CHECK(lag >= 28 && lag <= 34);
    const auto pw = channel_power(all, 2, 1000);
    CHECK(pw[0] < pw[1] * 0.5);                                              // head shadow: the far ear is quieter
    // straight ahead: no delay, equal levels
    nue_spatial_source_params fr = src_at(0, 0, 1);
    nue_spatial b2 = make(NUE_SPATIAL_BINAURAL, 0);
    nue_spatial_source src2 = nullptr;
    nue_spatial_source_create(b2, &src2);
    std::vector<float> front;
    for (int blk = 0; blk < 20; ++blk) {
        std::vector<float> in(256), out(512);
        for (float& v : in) { seed = seed * 1664525u + 1013904223u; v = float(int32_t(seed)) * (1.0f / 2147483648.0f) * 0.5f; }
        nue_spatial_begin_block(b2, &w, out.data(), 256);
        nue_spatial_render_source(src2, &fr, in.data());
        nue_spatial_end_block(b2);
        front.insert(front.end(), out.begin(), out.end());
    }
    CHECK_EQ(best_lag(front, 2, 40), 0);
    const auto fp = channel_power(front, 2, 500);
    CHECK_NEAR(fp[0] / fp[1], 1.0, 0.02);
    CHECK_STR(nue_spatial_active_renderer(b2), "binaural");
    nue_spatial_source_destroy(src); nue_spatial_source_destroy(src2);
    nue_spatial_destroy(b); nue_spatial_destroy(b2);
    nue_spatial_desc d;
    NUE_STRUCT_INIT(&d);
    d.mode = NUE_SPATIAL_BINAURAL; d.mask = NUE_MASK_5_1; d.sample_rate = 48000;
    nue_spatial bad = nullptr;
    CHECK_EQ(nue_spatial_create(&d, &bad), NUE_ERR_INVALID_ARGUMENT);        // binaural needs two channels
}

TEST_CASE(measured_hrtf_set_replaces_the_head_model) {
    nue_spatial_world w = world();
    nue_spatial b = make(NUE_SPATIAL_BINAURAL, 0);
    // two directions: right (90, 0) = left ear delta at 3, right ear delta at 0; left (-90, 0) = mirrored
    const uint32_t n = 8;
    float az[2] = {90, -90}, el[2] = {0, 0};
    std::vector<float> irs(2 * 2 * n, 0.0f);
    irs[(0 * 2 + 0) * n + 3] = 0.5f;      // dir 0, left ear
    irs[(0 * 2 + 1) * n + 0] = 1.0f;      // dir 0, right ear
    irs[(1 * 2 + 0) * n + 0] = 1.0f;
    irs[(1 * 2 + 1) * n + 3] = 0.5f;
    CHECK_EQ(nue_spatial_set_hrtf(b, az, el, irs.data(), 2, n, 48000), NUE_OK);
    nue_spatial_source src = nullptr;
    nue_spatial_source_create(b, &src);
    std::vector<float> in(64, 0.0f), out(128);
    in[10] = 1.0f;
    nue_spatial_source_params p = src_at(1, 0, 0.1f);
    p.min_distance = 10;
    nue_spatial_begin_block(b, &w, out.data(), 64);
    nue_spatial_render_source(src, &p, in.data());
    nue_spatial_end_block(b);
    CHECK_NEAR(out[10 * 2 + 1], 1.0, 1e-6);                                  // right ear: immediate
    CHECK_NEAR(out[13 * 2 + 0], 0.5, 1e-6);                                  // left ear: 3 samples later, attenuated
    CHECK_STR(nue_spatial_active_renderer(b), "hrtf");
    nue_spatial_source_destroy(src);
    nue_spatial_destroy(b);
}

TEST_CASE(ambisonic_encode_decode_puts_energy_toward_the_source) {
    nue_spatial_world w = world();
    for (uint32_t order = 1; order <= 3; ++order) {
        nue_spatial a = make(NUE_SPATIAL_AMBISONIC, NUE_MASK_5_1, order);
        auto front = channel_power(render_const(a, w, src_at(0, 0, 2), 2, 64, 2.0f), 6, 64);
        auto rear = channel_power(render_const(a, w, src_at(0, 0, -2), 2, 64, 2.0f), 6, 64);
        auto right = channel_power(render_const(a, w, src_at(2, 0, 0), 2, 64, 2.0f), 6, 64);
        std::printf("  order %u: front FC %.3f vs BL %.3f | rear BL %.3f vs FC %.3f | right FR %.3f vs FL %.3f\n", order, front[2], front[4], rear[4], rear[2], right[1], right[0]);
        CHECK(front[2] > front[4] * 2.0);
        CHECK(rear[4] + rear[5] > (rear[2] + rear[0] + rear[1]) * 0.5);
        CHECK(right[1] > right[0] * 2.0);
        CHECK_EQ(front[3], 0.0);
        double total = 0;
        for (double v : front) total += v;
        CHECK_NEAR(total, 1.0, 0.05);                                        // normalised to unit power straight ahead
        nue_spatial_destroy(a);
    }
}

struct BackendProbe { int calls = 0; uint32_t last_count = 0; float az = 0; bool fail = false; int destroyed = 0; };

TEST_CASE(backend_hook_receives_evaluated_sources_and_failures_fall_back) {
    BackendProbe probe;
    nue_spatial_backend_ops ops;
    NUE_STRUCT_INIT(&ops);
    ops.name = "test-backend";
    ops.userdata = &probe;
    ops.render = [](void* ud, const nue_spatial_world*, const nue_spatial_backend_source* s, uint32_t count, uint32_t frames, float* out) -> nue_result {
        BackendProbe* p = static_cast<BackendProbe*>(ud);
        ++p->calls; p->last_count = count; p->az = count ? s[0].eval.azimuth_deg : 0;
        if (p->fail) return NUE_ERR_DEVICE_LOST;
        for (uint32_t f = 0; f < frames; ++f) out[f * 2] += 0.25f * s[0].mono[f];
        return NUE_OK;
    };
    ops.destroy = [](void* ud) { ++static_cast<BackendProbe*>(ud)->destroyed; };
    nue_spatial_desc d;
    NUE_STRUCT_INIT(&d);
    d.mode = NUE_SPATIAL_PAN; d.sample_rate = 48000; d.backend = &ops;
    nue_spatial s = nullptr;
    CHECK_EQ(nue_spatial_create(&d, &s), NUE_OK);
    nue_spatial_world w = world();
    auto out = render_const(s, w, src_at(1, 0, 0), 1, 32);
    CHECK_EQ(probe.calls, 1);
    CHECK_EQ(probe.last_count, 1u);
    CHECK_NEAR(probe.az, 90.0, 1e-3);
    CHECK_NEAR(out[0], 0.25, 1e-6);
    CHECK_STR(nue_spatial_active_renderer(s), "test-backend");
    probe.fail = true;                                                       // the backend fails: the built-in panner renders instead
    out = render_const(s, w, src_at(1, 0, 0), 1, 32);
    CHECK_NEAR(out[1], 1.0, 1e-6);
    CHECK_NEAR(out[0], 0.0, 1e-6);
    CHECK_STR(nue_spatial_active_renderer(s), "pan");
    nue_spatial_destroy(s);
    CHECK_EQ(probe.destroyed, 1);
    // block protocol errors
    nue_spatial s2 = make(NUE_SPATIAL_PAN, 0);
    CHECK_EQ(nue_spatial_end_block(s2), NUE_ERR_BAD_STATE);
    nue_spatial_destroy(s2);
}

TEST_MAIN_BEGIN
    RUN(scene_maths_distance_cone_doppler_and_listener_pose);
    RUN(panning_is_constant_power_and_lands_on_the_right_speakers);
    RUN(binaural_model_has_interaural_delay_and_head_shadow);
    RUN(measured_hrtf_set_replaces_the_head_model);
    RUN(ambisonic_encode_decode_puts_energy_toward_the_source);
    RUN(backend_hook_receives_evaluated_sources_and_failures_fall_back);
TEST_MAIN_END
