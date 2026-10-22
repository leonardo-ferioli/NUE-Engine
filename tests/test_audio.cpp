// Audio devices and streams on the null backend's virtual clock, plus format / channel / rate conversion maths.
#include "internal/audio_convert.h"
#include "test_env.h"
#include "nue_test.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using nue_test::Engine;

static nue_audio_stream open_stream(nue_audio_format fmt, uint32_t ch, uint32_t rate, nue_audio_callback cb = nullptr, void* ud = nullptr,
                                    nue_audio_stream_flags flags = 0) {
    nue_audio_stream_desc d;
    NUE_STRUCT_INIT(&d);
    d.spec = {fmt, ch, rate};
    d.callback = cb;
    d.userdata = ud;
    d.flags = flags;
    d.name = "test";
    d.latency_ms = 10;
    nue_audio_stream s = nullptr;
    nue_result r = nue_audio_stream_create(&d, &s);
    if (NUE_FAILED(r)) std::printf("  stream create failed: %s (%s)\n", nue_result_name(r), nue_get_last_error_message());
    return s;
}

TEST_CASE(device_enumeration) {
    Engine e;
    uint32_t n = 0;
    nue_audio_device_info infos[4];
    for (auto& i : infos) NUE_STRUCT_INIT(&i);
    CHECK_EQ(nue_audio_list_devices(NUE_FALSE, infos, 4, &n), NUE_OK);
    CHECK_EQ(n, 2u);
    CHECK(infos[0].is_default && !infos[1].is_default);
    CHECK_EQ(nue_audio_list_devices(NUE_TRUE, infos, 4, &n), NUE_OK);
    CHECK_EQ(n, 0u);
    nue_audio_device_info def;
    NUE_STRUCT_INIT(&def);
    CHECK_EQ(nue_audio_get_device_info(0, &def), NUE_OK);
    CHECK_EQ(def.id, 1u);
    CHECK_EQ(nue_audio_get_device_info(77, &def), NUE_ERR_NOT_FOUND);
    CHECK_EQ(nue_audio_bytes_per_sample(NUE_AUDIO_S16), 2u);
    nue_audio_spec sp = {NUE_AUDIO_F32, 6, 48000};
    CHECK_EQ(nue_audio_frame_size(&sp), 24u);
}

TEST_CASE(invalid_stream_formats_are_rejected) {
    Engine e;
    nue_audio_stream_desc d;
    NUE_STRUCT_INIT(&d);
    nue_audio_stream s = nullptr;
    d.spec = {NUE_AUDIO_S16, 9, 48000};
    CHECK_EQ(nue_audio_stream_create(&d, &s), NUE_ERR_INVALID_ARGUMENT);
    d.spec = {NUE_AUDIO_S16, 2, 100};
    CHECK_EQ(nue_audio_stream_create(&d, &s), NUE_ERR_INVALID_ARGUMENT);
    d.spec = {NUE_AUDIO_U8, 2, 48000};
    CHECK_EQ(nue_audio_stream_create(&d, &s), NUE_ERR_INVALID_ARGUMENT);
    d.spec = {NUE_AUDIO_S16, 2, 48000};
    d.flags = NUE_AUDIO_CAPTURE;
    CHECK_EQ(nue_audio_stream_create(&d, &s), NUE_ERR_UNSUPPORTED);
    CHECK(s == nullptr);
}

TEST_CASE(push_stream_plays_exactly_what_was_pushed) {
    Engine e;
    nue_audio_stream s = open_stream(NUE_AUDIO_S16, 2, 48000);
    CHECK(s != nullptr);
    CHECK_EQ(nue_null_audio_set_capture(s, 48000), NUE_OK);
    std::vector<int16_t> pcm(2 * 2400);   // 50 ms of a ramp
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = int16_t(i % 30000 + 1);
    uint32_t accepted = 0;
    CHECK_EQ(nue_audio_stream_push(s, pcm.data(), 2400, &accepted), NUE_OK);
    CHECK_EQ(accepted, 2400u);
    CHECK_EQ(nue_audio_stream_queued_frames(s), 2400u);

    nue_null_advance_time(100000000);   // 100 ms: the 50 ms of data plays, then silence (underrun)
    CHECK_EQ(nue_audio_stream_queued_frames(s), 0u);
    std::vector<int16_t> played(2 * 4800);
    uint32_t got = 0;
    CHECK_EQ(nue_null_audio_read_played(s, played.data(), 4800, &got), NUE_OK);
    CHECK_EQ(got, 4800u);
    CHECK(std::memcmp(played.data(), pcm.data(), pcm.size() * sizeof(int16_t)) == 0);   // bit exact
    bool tail_silent = true;
    for (size_t i = pcm.size(); i < played.size(); ++i) tail_silent &= played[i] == 0;
    CHECK(tail_silent);

    nue_audio_stream_stats st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_audio_stream_get_stats(s, &st), NUE_OK);
    CHECK_EQ(st.frames_played, 2400u);
    CHECK(st.underruns > 0);
    uint64_t pos = 0; int64_t t = 0;
    nue_audio_stream_get_position(s, &pos, &t);
    CHECK_EQ(pos, 2400u);
    nue_audio_stream_destroy(s);
}

TEST_CASE(queue_limits_flush_and_blocking_push) {
    Engine e;
    nue_audio_stream_desc d;
    NUE_STRUCT_INIT(&d);
    d.spec = {NUE_AUDIO_F32, 1, 8000};
    d.queue_frames = 1000;
    nue_audio_stream s = nullptr;
    CHECK_EQ(nue_audio_stream_create(&d, &s), NUE_OK);
    std::vector<float> data(3000, 0.25f);
    uint32_t accepted = 0;
    CHECK_EQ(nue_audio_stream_push(s, data.data(), 3000, &accepted), NUE_OK_PARTIAL);
    CHECK_EQ(accepted, 1000u);
    CHECK_EQ(nue_audio_stream_free_frames(s), 0u);
    CHECK_EQ(nue_audio_stream_flush(s), NUE_OK);
    CHECK_EQ(nue_audio_stream_queued_frames(s), 0u);
    CHECK_EQ(nue_audio_stream_free_frames(s), 1000u);
    nue_audio_stream_stats st;
    NUE_STRUCT_INIT(&st);
    nue_audio_stream_get_stats(s, &st);
    CHECK_EQ(st.dropped_frames, 2000u + 1000u);   // 2000 refused + 1000 flushed

    // blocking push: the virtual device drains while the call waits, so 3000 frames get through a 1000 frame queue
    CHECK_EQ(nue_audio_stream_push_wait(s, data.data(), 3000, 2000000000ll, &accepted), NUE_OK);
    CHECK_EQ(accepted, 3000u);
    // stopped streams do not drain: a blocking push must not hang
    nue_audio_stream_stop(s);
    nue_audio_stream_flush(s);
    CHECK_EQ(nue_audio_stream_push_wait(s, data.data(), 3000, -1, &accepted), NUE_OK_PARTIAL);
    CHECK_EQ(accepted, 1000u);
    nue_audio_stream_destroy(s);
}

TEST_CASE(callback_stream_runs_on_the_clock_and_honours_volume_and_pause) {
    Engine e;
    struct Ctx { uint64_t calls = 0, frames = 0; } c;
    nue_audio_stream s = open_stream(NUE_AUDIO_F32, 2, 48000, [](void* u, void* out, uint32_t frames) {
        Ctx* c = static_cast<Ctx*>(u);
        ++c->calls; c->frames += frames;
        float* p = static_cast<float*>(out);
        for (uint32_t i = 0; i < frames * 2; ++i) p[i] = 1.0f;
    }, &c);
    nue_null_audio_set_capture(s, 4800);
    nue_null_advance_time(20000000);   // 20 ms
    CHECK_EQ(c.frames, 960u);
    CHECK(c.calls >= 2);
    nue_audio_stream_set_volume(s, 0.5f);
    nue_null_advance_time(10000000);
    std::vector<float> played(2 * 4800);
    uint32_t got = 0;
    nue_null_audio_read_played(s, played.data(), 4800, &got);   // oldest first
    CHECK_EQ(got, 1440u);
    CHECK_NEAR(played[2 * 100], 1.0, 1e-6);
    CHECK_NEAR(played[2 * (got - 1)], 0.5, 1e-6);
    nue_audio_stream_stats st;
    NUE_STRUCT_INIT(&st);
    nue_audio_stream_get_stats(s, &st);
    CHECK_NEAR(st.peak_level, 0.5, 1e-6);

    // stop: the callback is not called
    nue_audio_stream_stop(s);
    uint64_t before = c.frames;
    nue_null_advance_time(50000000);
    CHECK_EQ(c.frames, before);   // a stopped stream's callback is not called; the device plays silence
    nue_audio_stream_start(s);

    // the app goes to the background: streams pause by themselves and come back
    nue_null_set_app_state(NUE_APP_STATE_BACKGROUND);
    uint64_t at_pause = c.frames;
    nue_null_advance_time(50000000);
    CHECK_EQ(c.frames, at_pause);
    nue_null_set_app_state(NUE_APP_STATE_FOREGROUND);
    nue_null_advance_time(10000000);
    CHECK(c.frames > at_pause);
    // pushing to a callback stream is a state error
    float dummy[2] = {0, 0};
    CHECK_EQ(nue_audio_stream_push(s, dummy, 1, nullptr), NUE_ERR_BAD_STATE);
    nue_audio_stream_destroy(s);
}

TEST_CASE(keep_running_in_background_flag) {
    Engine e;
    static uint64_t frames;
    frames = 0;
    nue_audio_stream s = open_stream(NUE_AUDIO_S16, 1, 22050, [](void*, void* out, uint32_t n) { frames += n; std::memset(out, 0, n * 2); }, nullptr, NUE_AUDIO_KEEP_RUNNING_IN_BACKGROUND);
    nue_null_set_app_state(NUE_APP_STATE_BACKGROUND);
    nue_null_advance_time(100000000);
    CHECK(frames > 2000);
    nue_audio_stream_destroy(s);
}

TEST_CASE(latency_query_and_change) {
    Engine e;
    nue_audio_stream s = open_stream(NUE_AUDIO_S16, 2, 48000);
    nue_audio_latency l;
    NUE_STRUCT_INIT(&l);
    CHECK_EQ(nue_audio_stream_get_latency(s, &l), NUE_OK);
    uint32_t before = l.device_buffer_frames;
    CHECK(before > 0);
    CHECK(l.device_latency_ns > 0);
    nue_result r = nue_audio_stream_set_latency(s, 40);
    CHECK(NUE_SUCCEEDED(r));
    nue_audio_stream_get_latency(s, &l);
    CHECK(l.device_buffer_frames > before);
    nue_audio_spec app, dev;
    nue_audio_stream_get_spec(s, &app, &dev);
    CHECK_EQ(app.sample_rate, 48000u);
    CHECK_EQ(dev.channels, 2u);
    nue_audio_stream_destroy(s);
    CHECK_EQ(nue_audio_stream_start(s), NUE_ERR_INVALID_HANDLE);   // destroyed handle is detected
}

TEST_CASE(shutdown_closes_open_streams) {
    {
        Engine e;
        open_stream(NUE_AUDIO_S16, 2, 48000);
        open_stream(NUE_AUDIO_F32, 2, 44100);
    }
    CHECK(!nue_is_initialized());
}

TEST_CASE(convert_format_channels_and_rate) {
    // S16 mono -> F32 stereo, no rate change
    int16_t mono[4] = {0, 16384, -16384, 32767};
    nue_audio_spec a = {NUE_AUDIO_S16, 1, 48000}, b = {NUE_AUDIO_F32, 2, 48000};
    float st[8];
    CHECK_EQ(nue_audio_convert(&a, mono, 4, &b, st, 4), 4u);
    CHECK_NEAR(st[2], 0.5 * 0.70710678, 1e-5);   // equal-power split
    CHECK_NEAR(st[2], st[3], 1e-9);
    // F32 -> S16 clips instead of wrapping
    float loud[3] = {2.0f, -2.0f, 0.5f};
    nue_audio_spec f1 = {NUE_AUDIO_F32, 1, 8000}, s1 = {NUE_AUDIO_S16, 1, 8000};
    int16_t out[3];
    nue_audio_convert(&f1, loud, 3, &s1, out, 3);
    CHECK_EQ(out[0], 32767);
    CHECK_EQ(out[1], -32768);
    CHECK_EQ(out[2], 16384);
    // 5.1 -> stereo keeps the front pair and folds the centre, drops the LFE
    float surround[6] = {0.1f, 0.2f, 0.4f, 1.0f, 0.0f, 0.0f};
    nue_audio_spec s51 = {NUE_AUDIO_F32, 6, 48000}, s2 = {NUE_AUDIO_F32, 2, 48000};
    float down[2];
    nue_audio_convert(&s51, surround, 1, &s2, down, 1);
    CHECK_NEAR(down[0], (0.1 + 0.70710678 * 0.4) * 0.5, 1e-5);
    CHECK_NEAR(down[1], (0.2 + 0.70710678 * 0.4) * 0.5, 1e-5);
    // 44.1 kHz -> 48 kHz: frame count scales and a sine keeps its shape
    std::vector<float> sine(4410);
    for (size_t i = 0; i < sine.size(); ++i) sine[i] = std::sin(2.0f * 3.14159265f * 440.0f * float(i) / 44100.0f);
    nue_audio_spec r1 = {NUE_AUDIO_F32, 1, 44100}, r2 = {NUE_AUDIO_F32, 1, 48000};
    std::vector<float> up(6000);
    uint32_t n = nue_audio_convert(&r1, sine.data(), 4410, &r2, up.data(), 6000);
    CHECK(n >= 4790 && n <= 4800);
    double max_err = 0;
    for (uint32_t i = 1; i + 2 < n; ++i) max_err = std::fmax(max_err, std::fabs(up[i] - std::sin(2.0f * 3.14159265f * 440.0f * float(i) / 48000.0f)));
    CHECK(max_err < 0.01);   // linear interpolation of a 440 Hz tone at these rates
    CHECK_EQ(nue_audio_convert(&r1, sine.data(), 10, &r2, up.data(), 6000) > 0, true);
    nue_audio_spec bad = {NUE_AUDIO_F32, 0, 48000};
    CHECK_EQ(nue_audio_convert(&bad, sine.data(), 10, &r2, up.data(), 6000), 0u);
}

TEST_CASE(streaming_resampler_has_no_block_boundary_artifacts) {
    // pushing the input in odd-sized pieces must give the same output as one big block
    const uint32_t channels = 2, total_in = 20000;
    std::vector<float> in(size_t(total_in) * channels);
    for (uint32_t i = 0; i < total_in; ++i) { in[i * 2] = std::sin(float(i) * 0.01f); in[i * 2 + 1] = std::cos(float(i) * 0.013f); }
    const double step = 44100.0 / 48000.0;
    nuei::Resampler whole, pieces;
    whole.reset(channels);
    pieces.reset(channels);
    whole.push(in.data(), total_in);
    std::vector<float> a(size_t(18000) * channels), b(size_t(18000) * channels);
    uint32_t na = whole.produce(18000, step, a.data());
    uint32_t nb = 0, fed = 0;
    while (nb < na) {
        uint32_t want = std::min<uint32_t>(257, na - nb);
        uint32_t need = pieces.input_needed(want, step);
        need = std::min(need, total_in - fed);
        pieces.push(in.data() + size_t(fed) * channels, need);
        fed += need;
        uint32_t made = pieces.produce(want, step, b.data() + size_t(nb) * channels);
        if (made == 0) break;
        nb += made;
    }
    CHECK_EQ(nb, na);
    double max_diff = 0;
    for (size_t i = 0; i < size_t(na) * channels; ++i) max_diff = std::fmax(max_diff, std::fabs(a[i] - b[i]));
    CHECK(max_diff < 1e-6);
}

TEST_MAIN_BEGIN
    RUN(device_enumeration);
    RUN(invalid_stream_formats_are_rejected);
    RUN(push_stream_plays_exactly_what_was_pushed);
    RUN(queue_limits_flush_and_blocking_push);
    RUN(callback_stream_runs_on_the_clock_and_honours_volume_and_pause);
    RUN(keep_running_in_background_flag);
    RUN(latency_query_and_change);
    RUN(shutdown_closes_open_streams);
    RUN(convert_format_channels_and_rate);
    RUN(streaming_resampler_has_no_block_boundary_artifacts);
TEST_MAIN_END
