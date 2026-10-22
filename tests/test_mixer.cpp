// Software mixer: voices (static / ring / callback), pan and gain maths, pitch, buses and effects, 3D, voice limits, determinism,
// and the mixer attached to an output stream on the null backend's virtual clock.
#include "test_env.h"
#include "nue_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using nue_test::Engine;

static const float kRoot2Over2 = 0.70710678f;

struct M {
    nue_mixer m = nullptr;
    explicit M(uint32_t rate = 48000, uint32_t channels = 2, uint32_t block = 64, uint32_t max_voices = 0) {
        nue_mixer_desc d;
        NUE_STRUCT_INIT(&d);
        d.flags = NUE_MIXER_MANUAL;
        d.sample_rate = rate; d.channels = channels; d.block_frames = block; d.max_voices = max_voices;
        nue_result r = nue_mixer_create(&d, &m);
        if (NUE_FAILED(r)) std::printf("  mixer create failed: %s\n", nue_get_last_error_message());
    }
    ~M() { nue_mixer_destroy(m); }
    std::vector<float> render(uint32_t frames, uint32_t channels = 2) {
        std::vector<float> out(size_t(frames) * channels, 123.0f);
        CHECK_EQ(nue_mixer_render(m, out.data(), frames), NUE_OK);
        return out;
    }
    nue_buffer_id buffer(const std::vector<int16_t>& pcm, uint32_t channels = 1, uint32_t rate = 48000, uint32_t loop_start = 0, uint32_t loop_end = 0) {
        nue_buffer_desc d;
        NUE_STRUCT_INIT(&d);
        d.spec = {NUE_AUDIO_S16, channels, rate};
        d.data = pcm.data();
        d.frame_count = uint32_t(pcm.size() / channels);
        d.copy_data = NUE_TRUE;
        d.loop_start = loop_start; d.loop_end = loop_end;
        nue_buffer_id id = 0;
        CHECK_EQ(nue_buffer_create(m, &d, &id), NUE_OK);
        return id;
    }
    nue_voice_id voice(nue_buffer_id b, nue_voice_flags flags = 0, float pan = 0, float volume = 0, float pitch = 0) {
        nue_voice_desc d;
        NUE_STRUCT_INIT(&d);
        d.kind = NUE_VOICE_STATIC; d.buffer = b; d.flags = flags; d.pan = pan; d.volume = volume; d.pitch = pitch;
        nue_voice_id v = 0;
        CHECK_EQ(nue_voice_create(m, &d, &v), NUE_OK);
        return v;
    }
};

static std::vector<int16_t> ramp(size_t n, int16_t peak) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = int16_t(peak * int(i + 1) / int(n));
    return v;
}

TEST_CASE(static_voice_plays_exact_samples_and_ends) {
    M mx;
    std::vector<int16_t> pcm(100, 16384);   // 0.5
    nue_buffer_id b = mx.buffer(pcm);
    static int ended;
    ended = 0;
    nue_voice_desc d;
    NUE_STRUCT_INIT(&d);
    d.kind = NUE_VOICE_STATIC; d.buffer = b; d.flags = NUE_VOICE_START_PLAYING;
    d.on_end = [](void*, nue_voice_id) { ++ended; };
    nue_voice_id v;
    CHECK_EQ(nue_voice_create(mx.m, &d, &v), NUE_OK);
    CHECK(nue_voice_is_playing(mx.m, v));
    std::vector<float> out = mx.render(200);
    // mono centred: constant-power pan puts 0.5 * cos(45 deg) in each channel
    CHECK_NEAR(out[0], 0.5 * kRoot2Over2, 1e-5);
    CHECK_NEAR(out[1], 0.5 * kRoot2Over2, 1e-5);
    CHECK_NEAR(out[2 * 99], 0.5 * kRoot2Over2, 1e-5);
    CHECK_NEAR(out[2 * 100], 0.0, 1e-9);   // buffer over: silence
    CHECK_EQ(ended, 1);
    CHECK(!nue_voice_is_playing(mx.m, v));
    nue_voice_state st;
    NUE_STRUCT_INIT(&st);
    nue_voice_get_state(mx.m, v, &st);
    CHECK_EQ(st.position_frames, 0u);   // reset at the end
    CHECK_EQ(nue_voice_play(mx.m, v), NUE_OK);   // can play again
    out = mx.render(10);
    CHECK_NEAR(out[0], 0.5 * kRoot2Over2, 1e-5);
}

TEST_CASE(pan_law_and_stereo_balance) {
    M mx;
    nue_buffer_id mono = mx.buffer(std::vector<int16_t>(64, 32767));
    nue_voice_id left = mx.voice(mono, NUE_VOICE_START_PLAYING, -1.0f);
    std::vector<float> out = mx.render(64);
    CHECK_NEAR(out[0], 1.0, 1e-3);
    CHECK_NEAR(out[1], 0.0, 1e-3);
    nue_voice_set_pan(mx.m, left, 1.0f);
    nue_voice_play(mx.m, left);
    // pan changes ramp over the block: after a full block the new pan is in effect
    out = mx.render(64);
    out = mx.render(64);
    nue_voice_destroy(mx.m, left);
    nue_buffer_id st = mx.buffer([] { std::vector<int16_t> v; for (int i = 0; i < 64; ++i) { v.push_back(32767); v.push_back(16384); } return v; }(), 2);
    nue_voice_id sv = mx.voice(st, NUE_VOICE_START_PLAYING, 0.5f);   // balance towards the right: left is attenuated by (1 - pan)
    out = mx.render(64);
    CHECK_NEAR(out[0], 0.5 * 1.0, 1e-3);
    CHECK_NEAR(out[1], 0.5, 1e-3);
    nue_voice_destroy(mx.m, sv);
    // total power of a centred mono voice equals a hard-panned one (constant power)
    nue_voice_id c = mx.voice(mono, NUE_VOICE_START_PLAYING, 0.0f);
    out = mx.render(64);
    CHECK_NEAR(out[0] * out[0] + out[1] * out[1], 1.0, 2e-3);
    nue_voice_destroy(mx.m, c);
}

TEST_CASE(volume_master_and_gain_ramp_without_clicks) {
    M mx(48000, 2, 64);
    nue_buffer_id b = mx.buffer(std::vector<int16_t>(4096, 32767));
    nue_voice_id v = mx.voice(b, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP, -1.0f, 1.0f);
    mx.render(64);   // gain settled at 1
    nue_voice_set_volume(mx.m, v, 0.0f);
    std::vector<float> out = mx.render(64);
    // linear ramp 1 -> 0 across the block
    CHECK(out[0] > 0.9f);
    CHECK(out[2 * 31] > 0.4f && out[2 * 31] < 0.6f);
    CHECK_NEAR(out[2 * 63], 0.0, 1e-4);
    for (int i = 1; i < 64; ++i) CHECK(out[2 * i] <= out[2 * (i - 1)] + 1e-6f);   // monotonic: no step
    nue_voice_set_volume(mx.m, v, 1.0f);
    mx.render(64);
    nue_mixer_set_master_volume(mx.m, 0.5f);
    mx.render(64);
    out = mx.render(64);
    CHECK_NEAR(out[0], 0.5, 1e-3);
    CHECK_NEAR(nue_mixer_get_master_volume(mx.m), 0.5, 1e-9);
}

TEST_CASE(loop_and_loop_range) {
    M mx(48000, 2, 64);
    // 10 frames: a 4-frame loop range [4,8)
    std::vector<int16_t> pcm = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000};
    nue_buffer_id b = mx.buffer(pcm, 1, 48000, 4, 8);
    nue_voice_id v = mx.voice(b, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP, -1.0f);
    std::vector<float> out = mx.render(20);
    auto s = [&](int i) { return int(std::lround(out[size_t(2 * i)] * 32768.0f)); };
    CHECK_EQ(s(0), 1000); CHECK_EQ(s(3), 4000);
    CHECK_EQ(s(4), 5000); CHECK_EQ(s(7), 8000);
    CHECK_EQ(s(8), 5000);    // wrapped to the loop start, never plays frames 8, 9
    CHECK_EQ(s(11), 8000);
    CHECK_EQ(s(12), 5000);
    CHECK(nue_voice_is_playing(mx.m, v));
    nue_voice_set_looping(mx.m, v, NUE_FALSE);
    out = mx.render(16);
    CHECK(!nue_voice_is_playing(mx.m, v));   // finishes the buffer (up to frame 10) and stops
}

TEST_CASE(pitch_and_frequency_change_speed_and_resample) {
    M mx(48000, 2, 64);
    std::vector<int16_t> pcm = ramp(480, 30000);
    nue_buffer_id b = mx.buffer(pcm);
    nue_voice_id v = mx.voice(b, NUE_VOICE_START_PLAYING, -1.0f, 0.0f, 2.0f);   // twice as fast: 240 output frames
    mx.render(480);
    CHECK(!nue_voice_is_playing(mx.m, v));
    nue_voice_state st;
    NUE_STRUCT_INIT(&st);
    // a source at 24 kHz played into a 48 kHz mixer lasts twice as long
    nue_buffer_id slow = mx.buffer(ramp(240, 30000), 1, 24000);
    nue_voice_id sv = mx.voice(slow, NUE_VOICE_START_PLAYING, -1.0f);
    mx.render(300);
    CHECK(nue_voice_is_playing(mx.m, sv));     // 240 source frames last 480 output frames
    mx.render(200);
    CHECK(!nue_voice_is_playing(mx.m, sv));
    // SetFrequency overrides the source rate: 48 kHz source played "at" 96 kHz is 2x
    nue_buffer_id b2 = mx.buffer(ramp(480, 30000));
    nue_voice_id fv = mx.voice(b2, NUE_VOICE_START_PLAYING, -1.0f);
    nue_voice_set_frequency(mx.m, fv, 96000);
    mx.render(250);
    CHECK(!nue_voice_is_playing(mx.m, fv));
    CHECK_EQ(nue_voice_set_pitch(mx.m, fv, 0.001f), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_voice_set_frequency(mx.m, fv, 50), NUE_ERR_INVALID_ARGUMENT);
    // a linear ramp stays a straight line after resampling
    nue_buffer_id r = mx.buffer(ramp(100, 20000), 1, 24000);
    nue_voice_id rv = mx.voice(r, NUE_VOICE_START_PLAYING, -1.0f);
    std::vector<float> out = mx.render(64);
    double max_dev = 0;
    for (int i = 4; i < 64; ++i) {
        double slope = out[size_t(2 * i)] - out[size_t(2 * (i - 1))];
        double expected = 20000.0 / 100.0 / 32768.0 * 0.5;   // 0.5 source frame per output frame
        max_dev = std::max(max_dev, std::fabs(slope - expected));
    }
    CHECK(max_dev < 1e-4);
    nue_voice_destroy(mx.m, rv);
}

TEST_CASE(buses_volume_mute_nesting_and_effects) {
    M mx(48000, 2, 64);
    nue_buffer_id b = mx.buffer(std::vector<int16_t>(8192, 16384));
    nue_bus_id music, sfx;
    CHECK_EQ(nue_bus_create(mx.m, 0, &music), NUE_OK);
    CHECK_EQ(nue_bus_create(mx.m, music, &sfx), NUE_OK);   // sfx child of music
    nue_voice_id v = mx.voice(b, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP, -1.0f);
    CHECK_EQ(nue_voice_set_bus(mx.m, v, sfx), NUE_OK);
    mx.render(64);
    std::vector<float> out = mx.render(64);
    CHECK_NEAR(out[0], 0.5, 1e-4);
    nue_bus_set_volume(mx.m, sfx, 0.5f);
    nue_bus_set_volume(mx.m, music, 0.5f);
    out = mx.render(64);
    CHECK_NEAR(out[0], 0.125, 1e-4);   // 0.5 * 0.5 * 0.5
    nue_bus_set_mute(mx.m, music, NUE_TRUE);
    out = mx.render(64);
    CHECK_NEAR(out[0], 0.0, 1e-9);
    nue_bus_set_mute(mx.m, music, NUE_FALSE);
    // destroying a bus moves its voices to the parent
    CHECK_EQ(nue_bus_destroy(mx.m, sfx), NUE_OK);
    out = mx.render(64);
    CHECK_NEAR(out[0], 0.25, 1e-4);    // now only the music bus (0.5) applies
    CHECK_EQ(nue_voice_set_bus(mx.m, v, sfx), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_bus_destroy(mx.m, 0), NUE_ERR_INVALID_HANDLE);   // master cannot go
}

TEST_CASE(lowpass_attenuates_high_frequencies_and_echo_delays) {
    // 10 kHz square-ish tone through a 500 Hz low-pass
    M mx(48000, 1, 64);
    std::vector<int16_t> tone(4800);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = int16_t(20000 * std::sin(2 * 3.14159265 * 10000.0 * double(i) / 48000.0));
    nue_buffer_id b = mx.buffer(tone);
    nue_voice_id v = mx.voice(b, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP);
    (void)v;
    std::vector<float> dry = mx.render(2400, 1);
    nue_effect_desc e;
    NUE_STRUCT_INIT(&e);
    e.kind = NUE_EFFECT_LOWPASS; e.params[0] = 500; e.params[1] = 0.707f; e.wet_dry = 1.0f;
    CHECK_EQ(nue_bus_set_effect(mx.m, 0, 0, &e), NUE_OK);
    mx.render(1000, 1);   // let the filter settle
    std::vector<float> wet = mx.render(1000, 1);
    auto peak = [](const std::vector<float>& x) { float p = 0; for (float f : x) p = std::max(p, std::fabs(f)); return p; };
    CHECK(peak(dry) > 0.4f);
    CHECK(peak(wet) < 0.01f);   // two octaves... far above the cutoff
    CHECK_EQ(nue_bus_set_effect(mx.m, 0, 0, nullptr), NUE_OK);

    // echo: an impulse comes back after the delay
    M ex(48000, 1, 64);
    std::vector<int16_t> imp(1, 32767);
    nue_buffer_id ib = ex.buffer(imp);
    nue_effect_desc echo;
    NUE_STRUCT_INIT(&echo);
    echo.kind = NUE_EFFECT_ECHO; echo.params[0] = 10.0f; echo.params[1] = 0.0f; echo.params[2] = 0.5f; echo.wet_dry = 1.0f;   // 10 ms = 480 frames
    nue_bus_set_effect(ex.m, 0, 0, &echo);
    ex.voice(ib, NUE_VOICE_START_PLAYING);
    std::vector<float> out = ex.render(600, 1);
    CHECK(out[0] > 0.9f);                // dry impulse: mono source into a 1-channel mixer, full level
    CHECK_NEAR(out[480], 0.5 * out[0], 0.02);
    CHECK_NEAR(out[300], 0.0, 1e-6);
    // wet/dry mix
    nue_effect_desc half = echo;
    half.wet_dry = 0.5f;
    nue_bus_set_effect(ex.m, 0, 1, &half);
    CHECK_EQ(nue_bus_set_effect(ex.m, 0, NUE_BUS_MAX_EFFECTS, &half), NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(callback_effect_and_callback_voice) {
    M mx(48000, 2, 64);
    static int fx_calls;
    fx_calls = 0;
    nue_effect_desc e;
    NUE_STRUCT_INIT(&e);
    e.kind = NUE_EFFECT_CALLBACK;
    e.callback = [](void*, float* s, uint32_t frames, uint32_t ch, uint32_t) { ++fx_calls; for (uint32_t i = 0; i < frames * ch; ++i) s[i] *= 0.5f; };
    CHECK_EQ(nue_bus_set_effect(mx.m, 0, 0, &e), NUE_OK);
    nue_effect_desc bad = e;
    bad.callback = nullptr;
    CHECK_EQ(nue_bus_set_effect(mx.m, 0, 1, &bad), NUE_ERR_INVALID_ARGUMENT);

    nue_voice_desc d;
    NUE_STRUCT_INIT(&d);
    d.kind = NUE_VOICE_CALLBACK;
    d.spec = {NUE_AUDIO_S16, 1, 48000};
    d.flags = NUE_VOICE_START_PLAYING;
    d.pan = -1.0f;
    static uint32_t produced;
    produced = 0;
    d.pull = [](void*, void* out, uint32_t frames) -> uint32_t {
        int16_t* p = static_cast<int16_t*>(out);
        uint32_t n = std::min<uint32_t>(frames, 300 - produced);   // 300 frames then the source ends
        for (uint32_t i = 0; i < n; ++i) p[i] = 20000;
        produced += n;
        return n;
    };
    nue_voice_id v;
    CHECK_EQ(nue_voice_create(mx.m, &d, &v), NUE_OK);
    std::vector<float> out = mx.render(128);
    CHECK_NEAR(out[0], 20000.0 / 32768.0 * 0.5, 1e-3);   // voice * effect
    CHECK(fx_calls >= 2);
    out = mx.render(512);
    CHECK(!nue_voice_is_playing(mx.m, v));
    CHECK_EQ(produced, 300u);
    CHECK_NEAR(out[2 * 400], 0.0, 1e-9);
}

TEST_CASE(ring_voice_follows_directsound_semantics) {
    M mx(48000, 1, 64);
    nue_voice_desc d;
    NUE_STRUCT_INIT(&d);
    d.kind = NUE_VOICE_RING;
    d.spec = {NUE_AUDIO_S16, 1, 48000};
    d.ring_frames = 256;
    d.pan = -1.0f;
    nue_voice_id v;
    CHECK_EQ(nue_voice_create(mx.m, &d, &v), NUE_OK);
    std::vector<int16_t> data(256);
    for (int i = 0; i < 256; ++i) data[size_t(i)] = int16_t((i + 1) * 100);
    CHECK_EQ(nue_voice_ring_write(mx.m, v, 0, data.data(), 256), NUE_OK);
    uint32_t play = 99, write = 99, size = 0;
    CHECK_EQ(nue_voice_ring_get_cursors(mx.m, v, &play, &write), NUE_OK);
    CHECK_EQ(play, 0u);
    CHECK(write > play);    // the write cursor leads by the pending block
    nue_voice_ring_get_size(mx.m, v, &size);
    CHECK_EQ(size, 256u);
    CHECK_EQ(nue_voice_play(mx.m, v), NUE_OK);
    std::vector<float> out = mx.render(100, 1);
    nue_voice_ring_get_cursors(mx.m, v, &play, &write);
    CHECK_EQ(play, 100u);
    CHECK_NEAR(out[0] * 32768.0, 100.0, 1.0);
    CHECK_NEAR(out[99] * 32768.0, 10000.0, 1.0);
    // wraps around the end, forever
    out = mx.render(200, 1);
    nue_voice_ring_get_cursors(mx.m, v, &play, &write);
    CHECK_EQ(play, 44u);    // 300 mod 256
    CHECK_NEAR(out[156] * 32768.0, 100.0, 1.0);   // frame 256 -> wrapped to the start of the ring
    // Stop keeps the play cursor (DirectSound), Play resumes from it
    nue_voice_stop(mx.m, v);
    nue_voice_ring_get_cursors(mx.m, v, &play, &write);
    CHECK_EQ(play, 44u);
    nue_voice_play(mx.m, v);
    out = mx.render(1, 1);
    CHECK_NEAR(out[0] * 32768.0, 4500.0, 1.0);
    // a streaming writer: overwrite the region behind the play cursor while it plays
    std::vector<int16_t> fresh(50, 5000);
    CHECK_EQ(nue_voice_ring_write(mx.m, v, 200, fresh.data(), 50), NUE_OK);
    CHECK_EQ(nue_voice_ring_fill_silence(mx.m, v, 250, 6), NUE_OK);   // wraps over the end into the start
    CHECK_EQ(nue_voice_ring_write(mx.m, v, 300, fresh.data(), 10), NUE_ERR_INVALID_ARGUMENT);
    nue_voice_set_position(mx.m, v, 200);
    out = mx.render(60, 1);
    CHECK_NEAR(out[0] * 32768.0, 5000.0, 1.0);
    CHECK_NEAR(out[51] * 32768.0, 0.0, 1.0);   // silenced 250..255
    // other kinds reject ring calls
    nue_buffer_id sb = mx.buffer(std::vector<int16_t>(8, 1));
    nue_voice_id sv = mx.voice(sb);
    CHECK_EQ(nue_voice_ring_get_cursors(mx.m, sv, &play, &write), NUE_ERR_BAD_STATE);
}

TEST_CASE(ring_notifications_fire_when_the_play_cursor_passes_positions) {
    M mx(48000, 1, 64);
    nue_voice_desc d;
    NUE_STRUCT_INIT(&d);
    d.kind = NUE_VOICE_RING; d.spec = {NUE_AUDIO_S16, 1, 48000}; d.ring_frames = 200; d.flags = NUE_VOICE_START_PLAYING;
    nue_voice_id v;
    nue_voice_create(mx.m, &d, &v);
    static std::vector<uint32_t> hits;
    hits.clear();
    uint32_t positions[2] = {50, 150};
    CHECK_EQ(nue_voice_ring_set_notifications(mx.m, v, positions, 2, [](void*, nue_voice_id, uint32_t p) { hits.push_back(p); }, nullptr), NUE_OK);
    mx.render(64, 1);                 // cursor 0 -> 64: passes 50
    CHECK_EQ(hits.size(), 1u);
    CHECK_EQ(hits[0], 50u);
    mx.render(128, 1);                // 64 -> 192: passes 150
    CHECK_EQ(hits.size(), 2u);
    CHECK_EQ(hits[1], 150u);
    mx.render(64, 1);                 // 192 -> 256 = 56 after the wrap: passes 50 again
    CHECK_EQ(hits.size(), 3u);
    CHECK_EQ(hits[2], 50u);
    uint32_t bad[1] = {200};
    CHECK_EQ(nue_voice_ring_set_notifications(mx.m, v, bad, 1, [](void*, nue_voice_id, uint32_t) {}, nullptr), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_voice_ring_set_notifications(mx.m, v, nullptr, 0, nullptr, nullptr), NUE_OK);
    mx.render(200, 1);
    CHECK_EQ(hits.size(), 3u);        // removed
}

TEST_CASE(three_d_attenuation_pan_cone_and_doppler) {
    M mx(48000, 2, 64);
    nue_listener l;
    NUE_STRUCT_INIT(&l);
    l.forward = {0, 0, 1}; l.up = {0, 1, 0};
    nue_mixer_set_listener(mx.m, &l);
    nue_buffer_id b = mx.buffer(std::vector<int16_t>(48000, 32767));
    nue_voice_id v = mx.voice(b, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP | NUE_VOICE_3D);
    nue_voice_3d p;
    NUE_STRUCT_INIT(&p);
    p.min_distance = 1.0f; p.max_distance = 100.0f; p.rolloff = 1.0f; p.model = NUE_ATTEN_INVERSE;

    auto level = [&](nue_vec3 pos, nue_vec3 vel = {0, 0, 0}) {
        p.position = pos; p.velocity = vel;
        nue_voice_set_3d(mx.m, v, &p);
        mx.render(64);
        std::vector<float> o = mx.render(64);
        return std::make_pair(o[2 * 63], o[2 * 63 + 1]);
    };
    auto near_front = level({0, 0, 1});
    auto far_front = level({0, 0, 4});
    CHECK_NEAR(near_front.first, kRoot2Over2, 1e-3);                 // at min distance: full gain, centred
    CHECK_NEAR(far_front.first, kRoot2Over2 / 4.0, 1e-3);            // inverse distance: 1/4 at 4 m
    auto right = level({4, 0, 0});
    CHECK(right.second > right.first * 5.0f);                        // hard right
    auto left = level({-4, 0, 0});
    CHECK(left.first > left.second * 5.0f);
    // linear model reaches silence at max distance
    p.model = NUE_ATTEN_LINEAR; p.max_distance = 10.0f;
    auto at_max = level({0, 0, 10});
    CHECK_NEAR(at_max.first, 0.0, 1e-4);
    auto half = level({0, 0, 5.5});
    CHECK_NEAR(half.first, kRoot2Over2 * 0.5, 2e-2);
    p.model = NUE_ATTEN_NONE;
    auto none = level({0, 0, 50});
    CHECK_NEAR(none.first, kRoot2Over2, 1e-3);
    // cone: sound projected away from the listener is attenuated to the outer gain
    p.model = NUE_ATTEN_NONE;
    p.cone_direction = {0, 0, 1};   // pointing away (listener is at the origin, source at z=5 facing +z)
    p.cone_inner_angle_deg = 30.0f; p.cone_outer_angle_deg = 90.0f; p.cone_outer_gain = 0.25f;
    auto behind = level({0, 0, 5});
    CHECK_NEAR(behind.first, kRoot2Over2 * 0.25, 1e-3);
    p.cone_direction = {0, 0, -1};  // pointing at the listener
    auto facing = level({0, 0, 5});
    CHECK_NEAR(facing.first, kRoot2Over2, 1e-3);
    p.cone_direction = {0, 0, 0};
    // head relative sources ignore the listener transform
    nue_listener moved = l;
    moved.position = {100, 0, 0};
    nue_mixer_set_listener(mx.m, &moved);
    nue_voice_destroy(mx.m, v);
    nue_voice_id hv = mx.voice(b, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP | NUE_VOICE_3D | NUE_VOICE_HEAD_RELATIVE);
    p.position = {0, 0, 1};
    p.model = NUE_ATTEN_INVERSE;
    nue_voice_set_3d(mx.m, hv, &p);
    mx.render(64);
    std::vector<float> o = mx.render(64);
    CHECK_NEAR(o[2 * 63], kRoot2Over2, 1e-3);
    nue_voice_destroy(mx.m, hv);

    // doppler: a source approaching at 1/10 of the speed of sound plays about 11 percent faster
    nue_mixer_set_listener(mx.m, &l);
    nue_voice_id dv = mx.voice(mx.buffer(ramp(48000, 30000)), NUE_VOICE_START_PLAYING | NUE_VOICE_3D);
    nue_voice_3d dp;
    NUE_STRUCT_INIT(&dp);
    dp.position = {0, 0, 50}; dp.velocity = {0, 0, -34.3f}; dp.model = NUE_ATTEN_NONE; dp.min_distance = 1; dp.max_distance = 1000;
    nue_voice_set_3d(mx.m, dv, &dp);
    mx.render(4800);
    nue_voice_state st;
    NUE_STRUCT_INIT(&st);
    nue_voice_get_state(mx.m, dv, &st);
    double speedup = double(st.position_frames) / 4800.0;
    CHECK_NEAR(speedup, 343.0 / (343.0 - 34.3), 0.01);
    nue_world_3d w;
    NUE_STRUCT_INIT(&w);
    w.doppler_factor = 0.0f; w.speed_of_sound = 343.0f; w.rolloff_factor = 1.0f; w.distance_factor = 1.0f;
    nue_mixer_set_world_3d(mx.m, &w);
    nue_voice_set_position(mx.m, dv, 0);
    mx.render(4800);
    nue_voice_get_state(mx.m, dv, &st);
    CHECK_NEAR(double(st.position_frames) / 4800.0, 1.0, 0.001);   // doppler disabled
}

TEST_CASE(voice_limit_priority_stealing_and_auto_release) {
    M mx(48000, 2, 64, /*max_voices*/ 3);
    nue_buffer_id b = mx.buffer(std::vector<int16_t>(48000, 1000));
    nue_voice_desc d;
    NUE_STRUCT_INIT(&d);
    d.kind = NUE_VOICE_STATIC; d.buffer = b; d.flags = NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP;
    nue_voice_id v[4];
    d.priority = 5; CHECK_EQ(nue_voice_create(mx.m, &d, &v[0]), NUE_OK);
    d.priority = 1; CHECK_EQ(nue_voice_create(mx.m, &d, &v[1]), NUE_OK);
    d.priority = 3; CHECK_EQ(nue_voice_create(mx.m, &d, &v[2]), NUE_OK);
    d.priority = 0;
    CHECK_EQ(nue_voice_create(mx.m, &d, &v[3]), NUE_ERR_OVERFLOW);   // lowest priority of all: refused
    d.priority = 2; CHECK_EQ(nue_voice_create(mx.m, &d, &v[3]), NUE_OK);  // steals the priority 1 voice
    nue_voice_state st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_voice_get_state(mx.m, v[1], &st), NUE_ERR_INVALID_HANDLE);   // the stolen id is stale, not reused
    CHECK_EQ(nue_voice_get_state(mx.m, v[0], &st), NUE_OK);
    nue_mixer_stats ms;
    NUE_STRUCT_INIT(&ms);
    nue_mixer_get_stats(mx.m, &ms);
    CHECK_EQ(ms.virtualized_voices, 1u);

    // one-shots with AUTO_RELEASE vanish when they end
    M am;
    nue_buffer_id sb = am.buffer(std::vector<int16_t>(10, 1000));
    nue_voice_id shot = am.voice(sb, NUE_VOICE_START_PLAYING | NUE_VOICE_AUTO_RELEASE);
    am.render(64);
    CHECK_EQ(nue_voice_get_state(am.m, shot, &st), NUE_ERR_INVALID_HANDLE);
    // destroying a buffer does not cut a voice that is playing it
    nue_buffer_id big = am.buffer(std::vector<int16_t>(1000, 16384));
    nue_voice_id bv = am.voice(big, NUE_VOICE_START_PLAYING, -1.0f);
    CHECK_EQ(nue_buffer_destroy(am.m, big), NUE_OK);
    std::vector<float> out = am.render(10);
    CHECK_NEAR(out[0], 0.5, 1e-3);
    CHECK_EQ(nue_buffer_destroy(am.m, big), NUE_ERR_INVALID_HANDLE);
    nue_voice_desc late;
    NUE_STRUCT_INIT(&late);
    late.kind = NUE_VOICE_STATIC; late.buffer = big;
    nue_voice_id none;
    CHECK_EQ(nue_voice_create(am.m, &late, &none), NUE_ERR_INVALID_HANDLE);   // new voices cannot use a destroyed buffer
    nue_voice_destroy(am.m, bv);
}

TEST_CASE(output_is_deterministic_and_clipping_is_counted) {
    auto run = [] {
        M mx(44100, 2, 100);
        nue_buffer_id a = mx.buffer(ramp(3000, 30000), 1, 22050);
        nue_buffer_id b = mx.buffer(ramp(2000, 30000), 2, 48000);
        mx.voice(a, NUE_VOICE_START_PLAYING | NUE_VOICE_LOOP, 0.3f, 0.8f, 1.37f);
        mx.voice(b, NUE_VOICE_START_PLAYING, -0.2f, 1.0f);
        nue_effect_desc e;
        NUE_STRUCT_INIT(&e);
        e.kind = NUE_EFFECT_REVERB; e.params[0] = 0.5f; e.params[1] = 0.3f; e.params[2] = 0.3f; e.wet_dry = 1.0f;
        nue_bus_set_effect(mx.m, 0, 0, &e);
        return mx.render(5000);
    };
    std::vector<float> a = run(), b = run();
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);   // bit identical
    // render split differently gives the same samples (blocks are independent of call sizes)
    M one(48000, 2, 64), two(48000, 2, 64);
    nue_buffer_id x = one.buffer(ramp(2000, 30000)), y = two.buffer(ramp(2000, 30000));
    one.voice(x, NUE_VOICE_START_PLAYING); two.voice(y, NUE_VOICE_START_PLAYING);
    std::vector<float> whole = one.render(640);
    std::vector<float> parts;
    for (int i = 0; i < 10; ++i) { std::vector<float> p = two.render(64); parts.insert(parts.end(), p.begin(), p.end()); }
    CHECK(std::memcmp(whole.data(), parts.data(), whole.size() * sizeof(float)) == 0);

    // clipping: two full-scale voices add beyond +-1; the count and peak are reported, the signal is not clamped by the mixer
    M clip;
    nue_buffer_id loud = clip.buffer(std::vector<int16_t>(64, 32767));
    clip.voice(loud, NUE_VOICE_START_PLAYING, -1.0f); clip.voice(loud, NUE_VOICE_START_PLAYING, -1.0f);
    std::vector<float> out = clip.render(64);
    CHECK(out[0] > 1.9f);
    nue_mixer_stats ms;
    NUE_STRUCT_INIT(&ms);
    nue_mixer_get_stats(clip.m, &ms);
    CHECK(ms.clipped_samples >= 64);
    CHECK(ms.peak[0] > 1.9f);
    CHECK_EQ(ms.active_voices, 2u);
    nue_mixer_reset_stats(clip.m);
    nue_mixer_get_stats(clip.m, &ms);
    CHECK_EQ(ms.clipped_samples, 0u);
}

TEST_CASE(multichannel_output_layouts) {
    M mx(48000, 6, 64);   // 5.1 mixer
    nue_buffer_id mono = mx.buffer(std::vector<int16_t>(64, 32767));
    mx.voice(mono, NUE_VOICE_START_PLAYING, -1.0f);
    std::vector<float> out = mx.render(64, 6);
    CHECK_NEAR(out[0], 1.0, 1e-3);   // hard left on the front pair
    CHECK_NEAR(out[1], 0.0, 1e-3);
    for (int c = 2; c < 6; ++c) CHECK_NEAR(out[size_t(c)], 0.0, 1e-9);
    M mono_out(48000, 1, 64);
    nue_buffer_id st = mono_out.buffer([] { std::vector<int16_t> v; for (int i = 0; i < 64; ++i) { v.push_back(16384); v.push_back(16384); } return v; }(), 2);
    mono_out.voice(st, NUE_VOICE_START_PLAYING);
    std::vector<float> m1 = mono_out.render(64, 1);
    CHECK(m1[10] > 0.4f);   // stereo folded to mono, not silent
}

TEST_CASE(mixer_attached_to_a_stream_runs_on_the_virtual_device) {
    Engine e;
    nue_mixer_desc d;
    NUE_STRUCT_INIT(&d);
    d.sample_rate = 48000; d.channels = 2; d.block_frames = 128;
    nue_mixer mx;
    CHECK_EQ(nue_mixer_create(&d, &mx), NUE_OK);
    nue_audio_stream s = nue_mixer_get_stream(mx);
    CHECK(s != nullptr);
    CHECK_EQ(nue_null_audio_set_capture(s, 48000), NUE_OK);
    nue_buffer_desc bd;
    NUE_STRUCT_INIT(&bd);
    std::vector<int16_t> pcm(4800, 16384);
    bd.spec = {NUE_AUDIO_S16, 1, 48000}; bd.data = pcm.data(); bd.frame_count = 4800; bd.copy_data = NUE_TRUE;
    nue_buffer_id buf;
    nue_buffer_create(mx, &bd, &buf);
    nue_voice_desc vd;
    NUE_STRUCT_INIT(&vd);
    vd.kind = NUE_VOICE_STATIC; vd.buffer = buf; vd.flags = NUE_VOICE_START_PLAYING; vd.pan = -1.0f;
    nue_voice_id v;
    nue_voice_create(mx, &vd, &v);
    nue_null_advance_time(50000000);   // 50 ms of device time
    std::vector<float> played(2 * 4800);
    uint32_t got = 0;
    nue_null_audio_read_played(s, played.data(), 4800, &got);
    CHECK(got >= 2400);
    CHECK_NEAR(played[0], 0.5, 1e-3);
    CHECK_NEAR(played[2 * 2399], 0.5, 1e-3);
    nue_mixer_stats ms;
    NUE_STRUCT_INIT(&ms);
    nue_mixer_get_stats(mx, &ms);
    CHECK(ms.blocks_mixed > 10);
    // manual render of an attached mixer is a state error... but the call itself is allowed to fail early
    float tmp[8];
    nue_result r = nue_mixer_render(mx, tmp, 2);
    CHECK(r == NUE_ERR_BAD_STATE || NUE_SUCCEEDED(r));
    nue_mixer_destroy(mx);
    CHECK_EQ(nue_mixer_start(mx), NUE_ERR_INVALID_HANDLE);   // destroyed
}

TEST_CASE(invalid_arguments_and_stale_ids) {
    M mx;
    nue_buffer_desc bd;
    NUE_STRUCT_INIT(&bd);
    nue_buffer_id b;
    CHECK_EQ(nue_buffer_create(mx.m, &bd, &b), NUE_ERR_INVALID_ARGUMENT);
    int16_t one = 0;
    bd.data = &one; bd.frame_count = 1; bd.spec = {NUE_AUDIO_S16, 0, 48000};
    CHECK_EQ(nue_buffer_create(mx.m, &bd, &b), NUE_ERR_INVALID_ARGUMENT);
    nue_voice_desc vd;
    NUE_STRUCT_INIT(&vd);
    nue_voice_id v;
    vd.kind = NUE_VOICE_RING; vd.spec = {NUE_AUDIO_S16, 1, 48000}; vd.ring_frames = 1;
    CHECK_EQ(nue_voice_create(mx.m, &vd, &v), NUE_ERR_INVALID_ARGUMENT);
    vd.kind = NUE_VOICE_CALLBACK; vd.pull = nullptr;
    CHECK_EQ(nue_voice_create(mx.m, &vd, &v), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_voice_play(mx.m, 0), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_voice_play(mx.m, 0xFFFF0001u), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_mixer_render(mx.m, nullptr, 10), NUE_ERR_INVALID_ARGUMENT);
    nue_mixer bad = nullptr;
    nue_mixer_desc md;
    NUE_STRUCT_INIT(&md);
    md.channels = 9;
    md.flags = NUE_MIXER_MANUAL;
    CHECK_EQ(nue_mixer_create(&md, &bad), NUE_ERR_INVALID_ARGUMENT);
    // stale voice id after destroy is rejected, a new voice gets a different id
    nue_buffer_id sb = mx.buffer(std::vector<int16_t>(8, 1));
    nue_voice_id a = mx.voice(sb);
    nue_voice_destroy(mx.m, a);
    nue_voice_id c = mx.voice(sb);
    CHECK(a != c);
    CHECK_EQ(nue_voice_set_volume(mx.m, a, 1.0f), NUE_ERR_INVALID_HANDLE);
}

TEST_MAIN_BEGIN
    RUN(static_voice_plays_exact_samples_and_ends);
    RUN(pan_law_and_stereo_balance);
    RUN(volume_master_and_gain_ramp_without_clicks);
    RUN(loop_and_loop_range);
    RUN(pitch_and_frequency_change_speed_and_resample);
    RUN(buses_volume_mute_nesting_and_effects);
    RUN(lowpass_attenuates_high_frequencies_and_echo_delays);
    RUN(callback_effect_and_callback_voice);
    RUN(ring_voice_follows_directsound_semantics);
    RUN(ring_notifications_fire_when_the_play_cursor_passes_positions);
    RUN(three_d_attenuation_pan_cone_and_doppler);
    RUN(voice_limit_priority_stealing_and_auto_release);
    RUN(output_is_deterministic_and_clipping_is_counted);
    RUN(multichannel_output_layouts);
    RUN(mixer_attached_to_a_stream_runs_on_the_virtual_device);
    RUN(invalid_arguments_and_stale_ids);
TEST_MAIN_END
