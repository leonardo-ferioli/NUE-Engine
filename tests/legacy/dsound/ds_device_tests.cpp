// Level A tests: DirectSound semantics (nue::dsound::Device), mixing correctness, 3D, environment, engine. No window, no sound card, no game data.
// The semantics were established against the real Windows DirectSound by dsound_differential_tests (see docs/LEGACY_DSOUND.md); items marked
// "measured" below are what that comparison found.
#include <memory>

#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

namespace {

constexpr uint32_t F_VOL = dsx::buffer_flag::ctrl_volume, F_PAN = dsx::buffer_flag::ctrl_pan, F_FREQ = dsx::buffer_flag::ctrl_frequency,
                   F_3D = dsx::buffer_flag::ctrl_3d, F_NOTIFY = dsx::buffer_flag::ctrl_position_notify;

struct Rig {
    std::unique_ptr<dsx::Device> dev;
    explicit Rig(uint32_t rate = 48000, uint32_t channels = 2, uint32_t latency = 0, uint32_t block = 256) {
        dsx::Config c;
        c.block_frames = block;
        dev = dsx::make_device(c);
        dev->set_output({rate, channels, speaker::default_mask(channels)}, latency);
    }
    dsx::BufferId make(uint32_t flags, uint32_t bytes, const WaveFormat& f) {
        dsx::BufferDesc d;
        d.flags = flags;
        d.bytes = bytes;
        d.has_format = true;
        d.format = f;
        const auto r = dev->create_buffer(d);
        CHECK(r.hr == dsx::hr::ok);
        return r.id;
    }
    void write(dsx::BufferId id, uint32_t offset, const std::vector<uint8_t>& bytes) {
        dsx::Regions r;
        CHECK(dev->lock(id, offset, uint32_t(bytes.size()), 0, r) == dsx::hr::ok);
        CHECK(dev->unlock(id, r, bytes_of(bytes, 0, r.bytes1), bytes_of(bytes, r.bytes1, r.bytes2)) == dsx::hr::ok);
    }
    std::vector<float> render(uint32_t frames) {
        std::vector<float> out(size_t(frames) * dev->output().channels);
        dev->render(out.data(), frames);
        return out;
    }
};

void test_units() {
    CHECK_NEAR(dsx::millibels_to_amplitude(0), 1.0, 1e-6);
    CHECK_NEAR(dsx::millibels_to_amplitude(-600), 0.5012, 1e-3);   // -6 dB
    CHECK_NEAR(dsx::millibels_to_amplitude(-2000), 0.1, 1e-6);     // -20 dB
    CHECK(dsx::millibels_to_amplitude(-10000) == 0.0f);
    CHECK_NEAR(dsx::millibels_to_amplitude(-1234), nue_centibels_to_linear(-1234), 1e-6);   // the native unit helper agrees
    float l, r;
    dsx::pan_gains(0, l, r);
    CHECK(l == 1.0f && r == 1.0f);
    dsx::pan_gains(10000, l, r);
    CHECK(l == 0.0f && r == 1.0f);
    dsx::pan_gains(-1000, l, r);
    CHECK_NEAR(l, 1.0, 1e-6);
    CHECK_NEAR(r, 0.3162, 1e-3);
}

void test_creation_and_validation() {
    Rig rig;
    auto& d = *rig.dev;
    // Primary buffer: cooperative level and format validation.
    dsx::BufferDesc primary;
    primary.flags = dsx::buffer_flag::primary | F_VOL;
    auto p = d.create_buffer(primary);
    CHECK(p.hr == dsx::hr::ok && !p.existing_primary);
    CHECK(d.create_buffer(primary).existing_primary);   // a second request returns the same object
    CHECK(d.set_format(p.id, pcm16(44100, 2)) == dsx::hr::priority_level_needed);
    CHECK(d.set_cooperative_level(0) == dsx::hr::invalid_param);
    CHECK(d.set_cooperative_level(2) == dsx::hr::ok);
    CHECK(d.set_format(p.id, pcm16(44100, 2)) == dsx::hr::ok);
    CHECK(d.set_format(p.id, pcm16(44100, 6)) == dsx::hr::ok);
    CHECK(d.set_format(p.id, pcm16(44100, 9)) == dsx::hr::invalid_param);   // measured on Windows: 9 channels (extensible) is E_INVALIDARG
    CHECK(d.set_format(p.id, pcm16(50, 2)) == dsx::hr::invalid_param);
    WaveFormat f;
    CHECK(d.get_format(p.id, f) == dsx::hr::ok && f.channels == 6);
    // Secondary buffer validation.
    dsx::BufferDesc s;
    s.flags = 0;
    s.bytes = 4096;
    CHECK(d.create_buffer(s).hr == dsx::hr::invalid_param);                 // no format
    s.has_format = true;
    s.format = pcm16(44100, 2);
    s.flags = F_3D;
    CHECK(d.create_buffer(s).hr == dsx::hr::ok);                            // measured: Windows accepts a stereo 3D buffer (it is mixed down for 3D)
    s.format = pcm16(44100, 1);
    s.flags = F_3D | F_PAN;
    CHECK(d.create_buffer(s).hr == dsx::hr::ok);                            // and CTRL3D together with CTRLPAN
    s.flags = dsx::buffer_flag::loc_hardware;
    CHECK(d.create_buffer(s).hr == dsx::hr::unsupported);                   // no hardware voices: E_NOTIMPL, as Windows answers
    s.flags = dsx::buffer_flag::ctrl_fx;
    CHECK(d.create_buffer(s).hr == dsx::hr::unsupported);
    s.flags = 0;
    s.bytes = 2;
    CHECK(d.create_buffer(s).hr == dsx::hr::invalid_param);                 // under DSBSIZE_MIN
    s.bytes = 4001;
    s.format = pcm16(44100, 2);
    auto r = d.create_buffer(s);
    CHECK(r.hr == dsx::hr::ok);
    dsx::BufferCaps caps;
    CHECK(d.get_caps(r.id, caps) == dsx::hr::ok && caps.bytes == 4004);     // rounded UP to whole frames (measured)
    // Controls need their flag.
    int32_t v = 0;
    CHECK(d.set_volume(r.id, -100) == dsx::hr::control_unavailable);
    CHECK(d.get_pan(r.id, v) == dsx::hr::control_unavailable);
    CHECK(d.set_frequency(r.id, 22050) == dsx::hr::control_unavailable);
    auto c = rig.make(F_VOL | F_PAN | F_FREQ, 4000, pcm16(44100, 2));
    CHECK(d.set_volume(c, -10001) == dsx::hr::invalid_param);
    CHECK(d.set_volume(c, 1) == dsx::hr::invalid_param);
    CHECK(d.set_volume(c, -10000) == dsx::hr::ok);
    CHECK(d.set_pan(c, 10001) == dsx::hr::invalid_param);
    CHECK(d.set_frequency(c, 99) == dsx::hr::invalid_param);
    CHECK(d.set_frequency(c, 200001) == dsx::hr::invalid_param);
    CHECK(d.set_frequency(c, 0) == dsx::hr::ok);
    uint32_t hz = 0;
    CHECK(d.get_frequency(c, hz) == dsx::hr::ok && hz == 44100);            // 0 means the buffer's own rate
    CHECK(d.set_format(c, pcm16(44100, 2)) == dsx::hr::invalid_call);       // SetFormat is for the primary only
    // Stale handle after destroy.
    d.destroy_buffer(c);
    CHECK(d.get_volume(c, v) == dsx::hr::invalid_param);
    // The order of checks (measured): range first, control flag second.
    CHECK(d.set_volume(r.id, 1) == dsx::hr::invalid_param);                 // r has no CTRLVOLUME
    CHECK(d.set_volume(r.id, -100) == dsx::hr::control_unavailable);
    CHECK(d.set_pan(r.id, 20000) == dsx::hr::invalid_param);
    CHECK(d.set_frequency(r.id, 0) == dsx::hr::control_unavailable);
}

void test_lock_semantics() {
    Rig rig;
    auto& d = *rig.dev;
    auto b = rig.make(0, 1000 * 4, pcm16(48000, 2));
    dsx::Regions r;
    CHECK(d.lock(b, 0, 4000, 0, r) == dsx::hr::ok && r.bytes1 == 4000 && r.bytes2 == 0);
    CHECK(d.lock(b, 3000, 2000, 0, r) == dsx::hr::ok && r.offset1 == 3000 && r.bytes1 == 1000 && r.offset2 == 0 && r.bytes2 == 1000);   // wraps
    CHECK(d.lock(b, 0, 0, dsx::lock_flag::entire_buffer, r) == dsx::hr::ok && r.bytes1 == 4000);
    // measured: ENTIREBUFFER with an offset locks the whole buffer starting there, wrapping (offset 123 -> 3877 and 123 bytes)
    CHECK(d.lock(b, 123, 0, dsx::lock_flag::entire_buffer, r) == dsx::hr::ok && r.offset1 == 123 && r.bytes1 == 3877 && r.offset2 == 0 && r.bytes2 == 123);
    CHECK(d.lock(b, 4000, 4, 0, r) == dsx::hr::invalid_param);              // offset at/after the end
    CHECK(d.lock(b, 0, 4004, 0, r) == dsx::hr::invalid_param);              // longer than the buffer
    CHECK(d.lock(b, 0, 4, 0x80, r) == dsx::hr::invalid_param);              // unknown flag
    // FROMWRITECURSOR: not playing -> the write cursor equals the play cursor (the position).
    CHECK(d.set_current_position(b, 400) == dsx::hr::ok);
    CHECK(d.lock(b, 123, 8, dsx::lock_flag::from_write_cursor, r) == dsx::hr::ok && r.offset1 == 400);
    // Unlock with data moves bytes into the buffer; a short span is refused.
    std::vector<uint8_t> data(8, 0x55);
    CHECK(d.unlock(b, r, bytes_of(data, 0, 4), Bytes{}) == dsx::hr::invalid_param);
    CHECK(d.unlock(b, r, bytes_of(data, 0, 8), Bytes{}) == dsx::hr::ok);
    // Buffer lost: Lock, Play and GetCurrentPosition report it until Restore.
    d.lose_buffers();
    uint32_t status = 0;
    CHECK(d.get_status(b, status) == dsx::hr::ok && (status & dsx::status_flag::buffer_lost));
    CHECK(d.lock(b, 0, 4, 0, r) == dsx::hr::buffer_lost);
    CHECK(d.play(b, 0, 0) == dsx::hr::buffer_lost);
    CHECK(d.restore(b) == dsx::hr::ok);
    CHECK(d.lock(b, 0, 4, 0, r) == dsx::hr::ok);
}

void test_exact_playback_and_formats() {
    // Same rate as the output: samples pass through bit-exactly (ring mixed through the whole chain).
    Rig rig(48000, 2);
    auto b = rig.make(0, 64 * 4, pcm16(48000, 2));
    std::vector<int16_t> pattern(64 * 2);
    for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = int16_t(int(i) * 311 - 9000);
    rig.write(b, 0, s16_bytes(pattern));
    CHECK(rig.dev->play(b, 0, dsx::play_flag::looping) == dsx::hr::ok);
    auto out = rig.render(256);
    bool exact = true;
    for (size_t i = 0; i < 256 * 2; ++i) exact = exact && out[i] == float(pattern[i % pattern.size()]) / 32768.0f;
    CHECK(exact);   // and the ring wrapped four times

    // Mono duplicates to both speakers at unity; 8-bit unsigned has silence at 0x80; 24-bit, 32-bit int and float conversions.
    Rig r2(48000, 2);
    WaveFormat m8 = pcm16(48000, 1);
    m8.bits = 8;
    auto b8 = r2.make(0, 4, m8);
    r2.write(b8, 0, {0x80, 0xFF, 0x00, 0x80});
    r2.dev->play(b8, 0, dsx::play_flag::looping);
    auto o8 = r2.render(4);
    CHECK(o8[0] == 0.0f && o8[1] == 0.0f);
    CHECK_NEAR(o8[2], 127.0 / 128.0, 1e-6);
    CHECK(o8[2] == o8[3]);
    CHECK_NEAR(o8[4], -1.0, 1e-6);

    Rig r3(48000, 2);
    WaveFormat f24 = pcm16(48000, 1);
    f24.bits = 24;
    auto b24 = r3.make(0, 6, f24);
    r3.write(b24, 0, {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0});   // +0.5, -0.5
    r3.dev->play(b24, 0, dsx::play_flag::looping);
    auto o24 = r3.render(2);
    CHECK_NEAR(o24[0], 0.5, 1e-6);
    CHECK_NEAR(o24[2], -0.5, 1e-6);

    Rig r4(48000, 2);
    WaveFormat ff = pcm16(48000, 1);
    ff.bits = 32;
    ff.type = SampleType::ieee_float;
    auto bf = r4.make(0, 8, ff);
    const float fv[2] = {0.25f, -0.75f};
    std::vector<uint8_t> fb(8);
    std::memcpy(fb.data(), fv, 8);
    r4.write(bf, 0, fb);
    r4.dev->play(bf, 0, dsx::play_flag::looping);
    auto of = r4.render(2);
    CHECK(of[0] == 0.25f && of[2] == -0.75f);
}

void test_gain_pan_frequency() {
    Rig rig(48000, 2);
    auto b = rig.make(F_VOL | F_PAN | F_FREQ, 4800 * 4, pcm16(48000, 2));
    rig.write(b, 0, sine16(4800, 2, 1000, 48000, 0.5));
    rig.dev->play(b, 0, dsx::play_flag::looping);
    auto full = rig.render(4800);
    const double base = rms(full, 2, 0);
    CHECK_NEAR(base, 0.5 / std::sqrt(2.0), 0.01);

    Rig r2(48000, 2);
    auto b2 = r2.make(F_VOL | F_PAN, 4800 * 4, pcm16(48000, 2));
    r2.write(b2, 0, sine16(4800, 2, 1000, 48000, 0.5));
    r2.dev->set_volume(b2, -600);   // -6 dB
    r2.dev->play(b2, 0, dsx::play_flag::looping);
    auto quiet = r2.render(4800);
    CHECK_NEAR(rms(quiet, 2, 0) / base, 0.5012, 0.003);

    r2.dev->set_pan(b2, 10000);     // full right: left silent, right untouched
    r2.render(512);                 // let any ramp settle
    auto panned = r2.render(4800);
    CHECK(rms(panned, 2, 0) < 1e-6);
    CHECK_NEAR(rms(panned, 2, 1) / base, 0.5012, 0.003);

    // Frequency: a 1 kHz tone at 24 kHz played at its own rate stays 1 kHz; with SetFrequency(48000) it sounds an octave up.
    Rig r3(48000, 2);
    auto b3 = r3.make(F_FREQ, 24000 * 4, pcm16(24000, 2));
    r3.write(b3, 0, sine16(24000, 2, 1000, 24000, 0.5));
    r3.dev->play(b3, 0, dsx::play_flag::looping);
    auto native = r3.render(24000);   // 0.5 s of output
    CHECK(std::abs(zero_crossings(native, 2, 0) - 1000) <= 4);
    r3.dev->set_frequency(b3, 48000);
    r3.render(1024);
    auto octave = r3.render(24000);
    CHECK(std::abs(zero_crossings(octave, 2, 0) - 2000) <= 8);

    // Master volume through the primary buffer.
    Rig r4(48000, 2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary | F_VOL;
    auto prim = r4.dev->create_buffer(pd);
    auto b4 = r4.make(0, 4800 * 4, pcm16(48000, 2));
    r4.write(b4, 0, sine16(4800, 2, 1000, 48000, 0.5));
    r4.dev->play(b4, 0, dsx::play_flag::looping);
    r4.dev->set_volume(prim.id, -2000);
    r4.render(512);
    CHECK_NEAR(rms(r4.render(4800), 2, 0) / base, 0.1, 0.002);
}

void test_resampling_quality() {
    // 44.1 kHz game ring to a 48 kHz device (the real situation of the NFS Underground 2 default): amplitude and DC are preserved, error is small.
    Rig rig(48000, 2);
    const uint32_t n = 44100;   // exactly one second: the tone is periodic over the ring
    auto b = rig.make(0, n * 4, pcm16(44100, 2));
    rig.write(b, 0, sine16(n, 2, 1000, 44100, 0.5));
    rig.dev->play(b, 0, dsx::play_flag::looping);
    auto out = rig.render(48000);
    // The resampler's phase after its history is primed is within one output frame of the ideal one: allow that, nothing else.
    double max_err = 1e9;
    for (double shift : {-1.0, -0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75, 1.0}) {
        double err = 0;
        for (uint32_t i = 64; i < 48000 - 64; ++i) {
            const double ideal = 0.5 * std::sin(2.0 * 3.14159265358979 * 1000.0 * (double(i) + shift) / 48000.0);
            err = std::max(err, std::fabs(double(out[size_t(i) * 2]) - ideal));
        }
        max_err = std::min(max_err, err);
    }
    CHECK(max_err < 0.005);   // nue_resampler (medium): the tone is reproduced in phase and in level

    Rig dc(48000, 2);
    auto bd = dc.make(0, 2000 * 4, pcm16(22050, 2));
    std::vector<int16_t> flat(2000 * 2, 16384);
    dc.write(bd, 0, s16_bytes(flat));
    dc.dev->play(bd, 0, dsx::play_flag::looping);
    auto od = dc.render(6000);
    double worst = 0;
    for (size_t i = 200; i < 6000; ++i) worst = std::max(worst, std::fabs(double(od[i * 2]) - 0.5));
    CHECK(worst < 2e-3);   // unity gain at DC for every phase

    // Downsampling 96 kHz -> 48 kHz keeps the tone's amplitude.
    Rig ds(48000, 2);
    auto bs = ds.make(0, 96000 * 4, pcm16(96000, 2));
    ds.write(bs, 0, sine16(96000, 2, 1000, 96000, 0.5));
    ds.dev->play(bs, 0, dsx::play_flag::looping);
    CHECK_NEAR(rms(ds.render(48000), 2, 0), 0.5 / std::sqrt(2.0), 0.02);
}

void test_cursors_and_transport() {
    // Latency 480 frames (10 ms at 48 kHz), mixing block 256: exact expectations.
    Rig rig(48000, 2, 480, 256);
    auto b = rig.make(0, 48000 * 4, pcm16(48000, 2));   // 1 s ring
    uint32_t play = 1, write = 1, status = 0;
    CHECK(rig.dev->get_current_position(b, &play, &write) == dsx::hr::ok && play == 0 && write == 0);   // stopped: both cursors at the position
    CHECK(rig.dev->get_current_position(b, nullptr, nullptr) == dsx::hr::invalid_param);                // measured
    CHECK(rig.dev->play(b, 1, 0) == dsx::hr::invalid_param);   // priority needs DSBPLAY_LOCDEFER
    CHECK(rig.dev->play(b, 0, dsx::play_flag::looping) == dsx::hr::ok);
    rig.render(4800);
    // 19 blocks of 256 were mixed (4864 frames); 64 of them wait in the output queue; the device holds 480 more.
    CHECK(rig.dev->get_current_position(b, &play, &write) == dsx::hr::ok);
    CHECK(play == (4864 - 480 - 64) * 4);
    CHECK(write == (4320 + 1440) * 4);   // 30 ms ahead of what is heard (Windows' margin, measured)
    CHECK(rig.dev->get_status(b, status) == dsx::hr::ok && status == (dsx::status_flag::playing | dsx::status_flag::looping));
    // Stop keeps the position where it was heard; Play resumes there.
    CHECK(rig.dev->stop(b) == dsx::hr::ok);
    rig.dev->get_current_position(b, &play, &write);
    CHECK(play == (4864 - 480 - 64) * 4 && write == play);
    rig.render(1000);
    uint32_t again = 0;
    rig.dev->get_current_position(b, &again, nullptr);
    CHECK(again == play);   // a stopped buffer does not move
    CHECK(rig.dev->set_current_position(b, 4000) == dsx::hr::ok);
    rig.dev->get_current_position(b, &again, nullptr);
    CHECK(again == 4000);
    CHECK(rig.dev->set_current_position(b, 48000 * 4) == dsx::hr::invalid_param);
    // Play of a playing buffer only changes looping; it does not restart.
    rig.dev->play(b, 0, dsx::play_flag::looping);
    rig.render(512);
    rig.dev->play(b, 0, 0);
    rig.dev->get_status(b, status);
    CHECK((status & dsx::status_flag::playing) && !(status & dsx::status_flag::looping));

    // A non-looping buffer plays to its end, stops and returns to the start; one-shot status clears.
    Rig r2(48000, 2, 0, 64);
    auto s = r2.make(0, 100 * 4, pcm16(48000, 2));
    std::vector<int16_t> ones(200, 10000);
    r2.write(s, 0, s16_bytes(ones));
    r2.dev->play(s, 0, 0);
    auto o = r2.render(300);
    CHECK(o[99 * 2] != 0.0f && o[100 * 2] == 0.0f);   // exactly 100 frames of sound
    r2.dev->get_status(s, status);
    CHECK(!(status & dsx::status_flag::playing));
    uint32_t pos = 5;
    r2.dev->get_current_position(s, &pos, nullptr);
    CHECK(pos == 0);

    // The play cursor never stands still while a ring plays and advances at the output rate (the game rebuilds its audio after 100 identical polls).
    Rig r3(48000, 2, 480, 256);
    auto ring = r3.make(0, 22016 * 4, pcm16(44100, 2));
    r3.dev->play(ring, 0, dsx::play_flag::looping);
    uint32_t last = ~0u;
    bool always_moving = true;
    uint64_t total = 0;
    for (int i = 0; i < 300; ++i) {   // 300 polls of 10 ms
        r3.render(480);
        uint32_t cur = 0;
        r3.dev->get_current_position(ring, &cur, nullptr);
        if (i > 3) always_moving = always_moving && cur != last;
        last = cur;
        total += 480;
    }
    CHECK(always_moving);
    CHECK_NEAR(double(last / 4), std::fmod((double(total) - 480.0) * 44100.0 / 48000.0, 22016.0), 4.0);   // 44.1 kHz source frames heard after 3 s

    // The output does not depend on how the caller chops its requests.
    Rig a(48000, 2, 100, 256), c(48000, 2, 100, 256);
    for (Rig* r : {&a, &c}) {
        auto id = r->make(F_VOL | F_FREQ, 3000 * 4, pcm16(44100, 2));
        r->write(id, 0, sine16(3000, 2, 440, 44100, 0.4));
        r->dev->set_volume(id, -300);
        r->dev->play(id, 0, dsx::play_flag::looping);
    }
    auto whole = a.render(7000);
    std::vector<float> chopped;
    for (uint32_t done = 0, step = 1; done < 7000; step = step % 97 + 13) {
        const uint32_t m = std::min<uint32_t>(step, 7000 - done);
        auto part = c.render(m);
        chopped.insert(chopped.end(), part.begin(), part.end());
        done += m;
    }
    CHECK(whole == chopped);
}

struct NotifyLog {
    std::vector<uint64_t> tokens;
    static void fn(void* user, uint64_t token) { static_cast<NotifyLog*>(user)->tokens.push_back(token); }
};

void test_notifications() {
    Rig rig(48000, 2, 0, 128);
    NotifyLog log;
    rig.dev->set_notify_callback(&NotifyLog::fn, &log);
    auto b = rig.make(F_NOTIFY, 4800 * 4, pcm16(48000, 2));
    CHECK(rig.dev->set_notifications(b, std::vector<dsx::Notify>{{1000 * 4, 11}, {3000 * 4, 33}, {dsx::notify_offset_stop, 99}}) == dsx::hr::ok);
    CHECK(rig.dev->set_notifications(b, std::vector<dsx::Notify>{{4800 * 4, 1}}) == dsx::hr::invalid_param);
    rig.dev->play(b, 0, dsx::play_flag::looping);
    CHECK(rig.dev->set_notifications(b, std::vector<dsx::Notify>{{4, 1}}) == dsx::hr::invalid_call);   // not while playing
    rig.render(1100);
    CHECK(log.tokens == std::vector<uint64_t>({11}));   // passed 1000, not 3000
    rig.render(2000);
    CHECK(log.tokens == std::vector<uint64_t>({11, 33}));
    rig.render(4800);   // a whole lap: each position once more
    CHECK(log.tokens == std::vector<uint64_t>({11, 33, 11, 33}));
    log.tokens.clear();
    rig.dev->stop(b);
    rig.render(1);
    CHECK(log.tokens == std::vector<uint64_t>({99}));   // DSBPN_OFFSETSTOP
    auto plain = rig.make(0, 400, pcm16(48000, 2));
    CHECK(rig.dev->set_notifications(plain, std::vector<dsx::Notify>{{0, 1}}) == dsx::hr::control_unavailable);
}

void test_duplicate() {
    Rig rig(48000, 2);
    auto a = rig.make(F_VOL, 400, pcm16(48000, 2));
    std::vector<int16_t> v(200, 8000);
    rig.write(a, 0, s16_bytes(v));
    rig.dev->set_volume(a, -600);
    auto dup = rig.dev->duplicate_buffer(a);
    CHECK(dup.hr == dsx::hr::ok && dup.id != a);
    int32_t vol = 0;
    CHECK(rig.dev->get_volume(dup.id, vol) == dsx::hr::ok && vol == -600);
    rig.dev->play(a, 0, dsx::play_flag::looping);
    rig.dev->play(dup.id, 0, dsx::play_flag::looping);
    auto o = rig.render(8);
    CHECK_NEAR(o[0], 2.0 * 8000.0 / 32768.0 * 0.5012, 1e-4);   // two voices of the same data add
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto p = rig.dev->create_buffer(pd);
    CHECK(rig.dev->duplicate_buffer(p.id).hr == dsx::hr::invalid_param);
}

void test_multichannel_beds() {
    // 5.1 -> stereo: ITU downmix (centre and surrounds at -3 dB, LFE dropped), by nue_chanmix.
    Rig rig(48000, 2);
    WaveFormat f = pcm16(48000, 6);
    f.channel_mask = speaker::surround_5_1;
    auto b = rig.make(0, 8 * 12, f);
    std::vector<int16_t> frame = {0, 0, 16384, 16384, 0, 0};   // FC and LFE at 0.5
    std::vector<int16_t> all;
    for (int i = 0; i < 8; ++i) all.insert(all.end(), frame.begin(), frame.end());
    rig.write(b, 0, s16_bytes(all));
    rig.dev->play(b, 0, dsx::play_flag::looping);
    auto o = rig.render(16);
    CHECK_NEAR(o[2 * 8], 0.5 * 0.70710678, 1e-5);
    CHECK_NEAR(o[2 * 8 + 1], 0.5 * 0.70710678, 1e-5);

    Rig r2(48000, 2);
    auto b2 = r2.make(0, 8 * 12, f);
    std::vector<int16_t> back = {0, 0, 0, 0, 16384, 0};   // BL only
    std::vector<int16_t> all2;
    for (int i = 0; i < 8; ++i) all2.insert(all2.end(), back.begin(), back.end());
    r2.write(b2, 0, s16_bytes(all2));
    r2.dev->play(b2, 0, dsx::play_flag::looping);
    auto o2 = r2.render(16);
    CHECK_NEAR(o2[16], 0.5 * 0.70710678, 1e-5);
    CHECK(o2[17] == 0.0f);

    // 5.1 -> 5.1: each channel stays on its speaker.
    Rig r3(48000, 6);
    auto b3 = r3.make(0, 8 * 12, f);
    std::vector<int16_t> distinct = {1000, 2000, 3000, 4000, 5000, 6000};
    std::vector<int16_t> all3;
    for (int i = 0; i < 8; ++i) all3.insert(all3.end(), distinct.begin(), distinct.end());
    r3.write(b3, 0, s16_bytes(all3));
    r3.dev->play(b3, 0, dsx::play_flag::looping);
    auto o3 = r3.render(16);
    bool same = true;
    for (int c = 0; c < 6; ++c) same = same && std::fabs(o3[size_t(8) * 6 + size_t(c)] - float(distinct[size_t(c)]) / 32768.0f) < 1e-6f;
    CHECK(same);

    // Stereo into 5.1 stays on the front pair (no invented surround).
    Rig r4(48000, 6);
    auto b4 = r4.make(0, 64 * 4, pcm16(48000, 2));
    r4.write(b4, 0, s16_bytes(std::vector<int16_t>(64 * 2, 16384)));
    r4.dev->play(b4, 0, dsx::play_flag::looping);
    auto o4 = r4.render(8);
    CHECK_NEAR(o4[2 * 6 + 0], 0.5, 1e-6);
    CHECK_NEAR(o4[2 * 6 + 1], 0.5, 1e-6);
    CHECK(o4[2 * 6 + 2] == 0.0f && o4[2 * 6 + 4] == 0.0f);
}

std::vector<uint8_t> constant16(uint32_t frames, int16_t v) { return s16_bytes(std::vector<int16_t>(frames, v)); }

void test_ds3d() {
    Rig rig(48000, 2);
    auto b = rig.make(F_3D | F_VOL, 480 * 2, pcm16(48000, 1));
    rig.write(b, 0, constant16(480, 16384));   // 0.5
    rig.dev->play(b, 0, dsx::play_flag::looping);
    CHECK(std::string(rig.dev->spatial_name()) == "balance");
    dsx::Buffer3D p;
    // In front at the minimum distance: both speakers at unity of the 2D level.
    p.position = {0, 0, 1};
    CHECK(rig.dev->set_buffer_3d(b, p) == dsx::hr::ok);
    auto o = rig.render(512);
    CHECK_NEAR(o[2 * 400], 0.5, 1e-4);
    CHECK_NEAR(o[2 * 400 + 1], 0.5, 1e-4);
    // To the right: left falls, right stays.
    p.position = {5, 0, 0.01f};
    p.min_distance = 5;
    rig.dev->set_buffer_3d(b, p);
    rig.render(512);
    o = rig.render(512);
    CHECK(o[2 * 400] < 0.05 && std::fabs(o[2 * 400 + 1] - 0.5) < 1e-3);
    // Distance: inverse law, -6 dB per doubling beyond the minimum.
    p = {};
    p.min_distance = 1;
    p.position = {0, 0, 1};
    rig.dev->set_buffer_3d(b, p);
    rig.render(512);
    const float near_level = rig.render(512)[2 * 400];
    p.position = {0, 0, 4};
    rig.dev->set_buffer_3d(b, p);
    rig.render(512);
    CHECK_NEAR(rig.render(512)[2 * 400] / near_level, 0.25, 1e-3);
    // Beyond max distance nothing more is lost.
    p.position = {0, 0, 10000};
    p.max_distance = 100;
    rig.dev->set_buffer_3d(b, p);
    rig.render(512);
    CHECK_NEAR(rig.render(512)[2 * 400] / near_level, 0.01, 1e-4);
    // Cone: facing away gives the outside volume (-20 dB here).
    p = {};
    p.position = {0, 0, 2};
    p.cone_orientation = {0, 0, 1};   // pointing away from the listener at the origin (listener looks +z, source is at +z)
    p.inside_cone_angle = 60;
    p.outside_cone_angle = 90;
    p.cone_outside_volume = -2000;
    p.min_distance = 2;
    rig.dev->set_buffer_3d(b, p);
    rig.render(512);
    CHECK_NEAR(rig.render(512)[2 * 400] / 0.5, 0.1, 1e-3);
    // Disable mode: like a 2D buffer.
    p = {};
    p.position = {50, 0, 0};
    p.mode = dsx::Mode3D::disable;
    rig.dev->set_buffer_3d(b, p);
    rig.render(512);
    o = rig.render(512);
    CHECK_NEAR(o[2 * 400], 0.5, 1e-4);
    CHECK_NEAR(o[2 * 400 + 1], 0.5, 1e-4);
    // Parameter validation and gating.
    dsx::Buffer3D bad;
    bad.min_distance = 0;
    CHECK(rig.dev->set_buffer_3d(b, bad) == dsx::hr::invalid_param);
    auto flat = rig.make(0, 400, pcm16(48000, 1));
    CHECK(rig.dev->set_buffer_3d(flat, dsx::Buffer3D{}) == dsx::hr::control_unavailable);
    dsx::Listener3D l;
    l.doppler_factor = 11;
    CHECK(rig.dev->set_listener(l) == dsx::hr::invalid_param);
}

void test_doppler_and_listener() {
    // A 1 kHz source approaching at a tenth of the speed of sound plays 1/(1-0.1) times faster.
    Rig rig(48000, 2);
    auto b = rig.make(F_3D, 4800 * 2, pcm16(48000, 1));
    std::vector<int16_t> tone(4800);
    for (int i = 0; i < 4800; ++i) tone[size_t(i)] = int16_t(std::lrint(16000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * i / 48000.0)));
    rig.write(b, 0, s16_bytes(tone));
    rig.dev->play(b, 0, dsx::play_flag::looping);
    dsx::Buffer3D p;
    p.position = {0, 0, 50};
    p.velocity = {0, 0, -34.3f};
    p.min_distance = 100;
    rig.dev->set_buffer_3d(b, p);
    auto out = rig.render(48000);
    CHECK(std::abs(zero_crossings(out, 2, 0) - int(2000 * (1.0 / 0.9))) <= 12);
    // The other way (receding) plays lower: the sign of the velocities along source -> listener is the one DirectSound uses.
    p.velocity = {0, 0, 34.3f};
    rig.dev->set_buffer_3d(b, p);
    rig.render(2048);
    auto away = rig.render(48000);
    CHECK(std::abs(zero_crossings(away, 2, 0) - int(2000 * (1.0 / 1.1))) <= 12);
    // A listener turned to face +x puts a source at +x straight ahead (centre) and a source at +z to the left.
    dsx::Listener3D l;
    l.front = {1, 0, 0};
    l.top = {0, 1, 0};
    rig.dev->set_listener(l);
    p.velocity = {0, 0, 0};
    p.position = {10, 0, 0};
    p.min_distance = 10;
    rig.dev->set_buffer_3d(b, p);
    rig.render(1024);
    auto ahead = rig.render(4800);
    CHECK_NEAR(rms(ahead, 2, 0), rms(ahead, 2, 1), 1e-4);
    p.position = {0, 0, 10};
    rig.dev->set_buffer_3d(b, p);
    rig.render(1024);
    auto side = rig.render(4800);
    CHECK(rms(side, 2, 0) > 5.0 * rms(side, 2, 1));   // facing +x, +z is on the left
}

void test_spatial_modes() {
    // A speaker output uses nue_spatial's pairwise panning: a source straight ahead lands on the centre channel.
    Rig r6(48000, 6);
    auto b = r6.make(F_3D, 480 * 2, pcm16(48000, 1));
    r6.write(b, 0, constant16(480, 16384));
    r6.dev->play(b, 0, dsx::play_flag::looping);
    CHECK(std::string(r6.dev->spatial_name()) == "pan");
    dsx::Buffer3D p;
    p.position = {0, 0, 1};
    r6.dev->set_buffer_3d(b, p);
    r6.render(1024);
    auto o = r6.render(1024);
    CHECK_NEAR(rms(o, 6, 2), 0.5, 1e-3);
    CHECK(rms(o, 6, 0) < 1e-4 && rms(o, 6, 4) < 1e-4);
    p.position = {0, 0, -1};   // behind: both surrounds
    r6.dev->set_buffer_3d(b, p);
    r6.render(1024);
    o = r6.render(1024);
    CHECK(rms(o, 6, 4) > 0.3 && rms(o, 6, 5) > 0.3 && rms(o, 6, 2) < 1e-3);
    // Binaural output on a stereo device: the layer asks nue_spatial for it; a source on the right is louder in the right ear.
    Rig rb(48000, 2);
    dsx::SpatialOptions so;
    so.mode = dsx::SpatialMode::binaural;
    rb.dev->set_spatial(so);
    CHECK(std::string(rb.dev->spatial_name()) == "binaural");
    auto bb = rb.make(F_3D, 480 * 2, pcm16(48000, 1));
    std::vector<int16_t> noise(480);
    uint32_t seed = 7;
    for (auto& v : noise) {
        seed = seed * 1664525u + 1013904223u;
        v = int16_t((seed >> 16) - 32768) / 2;
    }
    rb.write(bb, 0, s16_bytes(noise));
    rb.dev->play(bb, 0, dsx::play_flag::looping);
    p = {};
    p.position = {2, 0, 0};
    p.min_distance = 2;
    rb.dev->set_buffer_3d(bb, p);
    rb.render(2048);
    auto ob = rb.render(4800);
    CHECK(rms(ob, 2, 1) > 2.0 * rms(ob, 2, 0));
}

void test_environment() {
    // The environment is nue_reverb: a source with a send gain leaves a tail after it ends, a source without a send leaves none.
    auto run = [](float send) {
        Rig rig(48000, 2, 0, 256);
        dsx::Environment env = dsx::eax2_to_environment(dsx::Eax2Listener{});
        env.params.decay_time = 1.0f;
        env.params.gain = 1.0f;
        rig.dev->set_environment(env);
        auto b = rig.make(0, 200 * 4, pcm16(48000, 2));
        std::vector<int16_t> burst(400);
        uint32_t seed = 3;
        for (auto& v : burst) {
            seed = seed * 1664525u + 1013904223u;
            v = int16_t((seed >> 16) - 32768) / 4;
        }
        rig.write(b, 0, s16_bytes(burst));
        dsx::EnvironmentSend s;
        s.send_gain = send;
        rig.dev->set_buffer_send(b, s);
        rig.dev->play(b, 0, 0);   // one shot
        return rig.render(24000);
    };
    auto dry = run(0.0f), wet = run(1.0f);
    CHECK(rms(dry, 2, 0, 12000, 24000) == 0.0);
    CHECK(rms(wet, 2, 0, 12000, 24000) > 0.0);
    // EAX 2.0 mappings.
    dsx::Eax2Listener l;
    dsx::Environment e = dsx::eax2_to_environment(l);
    CHECK(e.enabled && std::fabs(e.params.decay_time - 1.49f) < 1e-3f);
    CHECK_NEAR(e.params.gain, std::pow(10.0, -1000.0 / 2000.0), 1e-3);
    dsx::Eax2Buffer eb;
    dsx::EnvironmentSend s = dsx::eax2_to_send(eb);
    CHECK(s.direct_gain == 1.0f && s.direct_gain_hf == 1.0f);
    eb.obstruction = -1000;   // -10 dB at HF, LF ratio 0: the direct path loses only HF
    s = dsx::eax2_to_send(eb);
    CHECK_NEAR(s.direct_gain, 1.0, 1e-6);
    CHECK_NEAR(s.direct_gain_hf, std::pow(10.0, -0.5), 1e-4);
    eb.obstruction = 0;
    eb.occlusion = -2000;     // occlusion: both direct and room are lowered, room by the room ratio
    s = dsx::eax2_to_send(eb);
    CHECK(s.direct_gain < 1.0f && s.send_gain < s.direct_gain);
}

// ---- engine and determinism --------------------------------------------------------------------------------------------------------------

uint64_t scripted_session(uint32_t chunk, int* reopens = nullptr) {
    EngineConfig config;
    config.device.block_frames = 256;
    config.sample_rate = 48000;
    auto sink = std::make_unique<ManualSink>(chunk, 480);
    ManualSink* null = sink.get();
    auto engine = Engine::create(config, std::move(sink));
    if (!engine) return 0;
    auto& d = engine->device();
    d.set_cooperative_level(2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto prim = d.create_buffer(pd);
    // A streaming client's startup: primary format with 2 channels at 44.1 kHz, a 0.5 s secondary ring, silence, Play looping.
    d.set_format(prim.id, pcm16(44100, 2));
    dsx::BufferDesc sd;
    sd.flags = 0x18000;   // GETCURRENTPOSITION2 | GLOBALFOCUS
    sd.bytes = 22016 * 4;
    sd.has_format = true;
    sd.format = pcm16(44100, 2);
    auto ring = d.create_buffer(sd);
    engine->start();
    d.play(ring.id, 0, dsx::play_flag::looping);
    uint32_t cursor = 0;
    uint32_t written = 0;
    const uint32_t ring_bytes = 22016 * 4;
    for (int step = 0; step < 100; ++step) {   // 100 x 10 ms, refilling like a game's audio thread
        null->advance(480);
        d.get_current_position(ring.id, &cursor, nullptr);
        const uint32_t lead = (written + ring_bytes - cursor) % ring_bytes;
        const uint32_t want = 8820 * 4 / 2;   // keep about 100 ms ahead
        if (lead < want) {
            const uint32_t bytes = std::min<uint32_t>(want - lead, 3528 * 4);
            std::vector<uint8_t> tone(bytes);
            for (uint32_t i = 0; i + 3 < bytes; i += 4) {
                const double t = double((written + i) / 4) / 44100.0;
                const int16_t v = int16_t(std::lrint(9000.0 * std::sin(2.0 * 3.14159265358979 * 523.25 * t)));
                std::memcpy(&tone[i], &v, 2);
                std::memcpy(&tone[i + 2], &v, 2);
            }
            dsx::Regions r;
            if (d.lock(ring.id, written % ring_bytes, bytes, 0, r) == dsx::hr::ok) d.unlock(ring.id, r, bytes_of(tone, 0, r.bytes1), bytes_of(tone, r.bytes1, r.bytes2));
            written = (written + bytes) % ring_bytes;
        }
    }
    if (reopens) *reopens = int(engine->reopen_count());
    return null->hash();
}

void test_engine() {
    // Same session, different sink chunking: the same bytes reach the device.
    const uint64_t a = scripted_session(480), b = scripted_session(480);
    CHECK(a != 0 && a == b);   // repeatable
    CHECK(scripted_session(120) == a);
    std::printf("scripted session hash: 0x%016llx\n", static_cast<unsigned long long>(a));

    // The primary format follows into the sink: a 6-channel primary reopens the sink with 6 channels and the pan renderer.
    EngineConfig config;
    config.sample_rate = 48000;
    auto sink = std::make_unique<ManualSink>(256, 0);
    ManualSink* null = sink.get();
    auto engine = Engine::create(config, std::move(sink));
    CHECK(engine && null->actual().channels == 2 && std::string(engine->device().spatial_name()) == "balance");
    auto& d = engine->device();
    d.set_cooperative_level(2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto prim = d.create_buffer(pd);
    WaveFormat six = pcm16(44100, 6);
    six.channel_mask = speaker::surround_5_1;
    CHECK(d.set_format(prim.id, six) == dsx::hr::ok);
    CHECK(null->actual().channels == 6 && engine->reopen_count() == 1 && std::string(engine->device().spatial_name()) == "pan");
    CHECK(d.set_format(prim.id, pcm16(44100, 2)) == dsx::hr::ok);   // and back
    CHECK(null->actual().channels == 2 && engine->reopen_count() == 2 && std::string(engine->device().spatial_name()) == "balance");
    // A sink that cannot give 6 channels keeps what it has.
    struct Narrow final : OutputSink {
        ManualSink inner{256, 0};
        bool open(const SinkDesc& desc, SinkCallback cb, void* user) override {
            SinkDesc narrowed = desc;
            narrowed.spec.channels = 2;
            narrowed.spec.channel_mask = speaker::stereo;
            return inner.open(narrowed, cb, user);
        }
        OutputLayout actual() const override { return inner.actual(); }
        bool start() override { return inner.start(); }
        bool stop() override { return inner.stop(); }
        void close() override { inner.close(); }
        bool running() const override { return inner.running(); }
        uint64_t frames_played() const override { return inner.frames_played(); }
        SinkLatency latency() const override { return inner.latency(); }
        SinkStats stats() const override { return inner.stats(); }
    };
    auto e2 = Engine::create(config, std::make_unique<Narrow>());
    auto& d2 = e2->device();
    d2.set_cooperative_level(2);
    auto p2 = d2.create_buffer(pd);
    CHECK(d2.set_format(p2.id, six) == dsx::hr::ok);   // the application's format is accepted whatever the speakers are
    CHECK(e2->sink().actual().channels == 2);

    // Sink bookkeeping.
    ManualSink ns(100, 40);
    SinkDesc desc;
    desc.spec = {48000, 2, speaker::stereo};
    ns.open(desc, [](void*, float* out, uint32_t n) { for (uint32_t i = 0; i < n * 2; ++i) out[i] = 0.5f; }, nullptr);
    ns.advance(1000);   // not started: nothing happens
    CHECK(ns.frames_rendered() == 0);
    ns.start();
    ns.advance(250);
    CHECK(ns.frames_rendered() == 250 && ns.frames_played() == 210);
    const uint64_t h1 = ns.hash();
    ns.reset_hash();
    ns.advance(100);
    CHECK(ns.hash() != h1);
}

}  // namespace

int main() {
    test_units();
    test_creation_and_validation();
    test_lock_semantics();
    test_exact_playback_and_formats();
    test_gain_pan_frequency();
    test_resampling_quality();
    test_cursors_and_transport();
    test_notifications();
    test_duplicate();
    test_multichannel_beds();
    test_ds3d();
    test_doppler_and_listener();
    test_spatial_modes();
    test_environment();
    test_engine();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
