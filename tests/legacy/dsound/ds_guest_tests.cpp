// Guest facade tests: the guest ABI side of the DirectSound layer, driven the way translated code drives it (a COM pointer whose first word is a
// vtable of call tokens, stdcall words, structures in guest memory), with a fake guest arena (host addresses unrelated to guest addresses) and a
// deterministic sink. No game, no window. The central test replays the call sequence of a real streaming client (the NFS Underground 2 audio
// module, found by disassembly): probe of 1/2/4/6 channels, a 0.5 s ring, refill with wrapping Locks, teardown, re-creation as after a stall.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "test_guest.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

namespace {

// ---- the game's startup, exactly as its audio module does it ------------------------------------------------------------------------------

struct GameAudio {
    Guest& g;
    uint32_t ds = 0, primary = 0, ring = 0;
    uint32_t ring_bytes = 0;
    uint32_t hwnd = 0x1234;
    explicit GameAudio(Guest& guest) : g(guest) {}

    // 0x725a85: SetFormat on the primary with `channels` channels (plain WAVEFORMATEX up to 2, WAVEFORMATEXTENSIBLE with mask 0x33 / 0x3F above);
    // when that works it releases the previous secondary buffer and creates a new 0.5 s ring (flags 0x18000) of that format. Every probe step
    // therefore creates (and the next one releases) a ring of its own.
    uint32_t try_format(uint16_t channels, uint32_t rate) {
        const bool ext = channels > 2;
        const uint32_t mask = channels == 6 ? 0x3F : channels == 4 ? 0x33 : 0;
        const uint32_t hr = g.call(primary, buf_slot::SetFormat, {g.wave(channels, rate, 16, mask, ext)});
        if (int32_t(hr) < 0) return hr;
        if (ring) {
            g.call(ring, buf_slot::Release);
            ring = 0;
        }
        const uint32_t frames = (rate * 500 / 1000) & ~0xFu;
        ring_bytes = frames * channels * 2;
        const uint32_t pp2 = g.cell();
        const uint32_t made = g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0x18000, ring_bytes, g.wave(channels, rate, 16, mask, ext)), pp2, 0});
        if (int32_t(made) >= 0) ring = g.get32(pp2);
        return made;
    }
    // 0x725dd9 (capability probe) and 0x725f52 (start of the real audio)
    bool init(uint16_t channels, uint32_t rate, bool probe) {
        const uint32_t pp = g.cell();
        if (g.facade->direct_sound_create(0, pp, 0) != HR_OK) return false;
        ds = g.get32(pp);
        g.call(ds, ds_slot::SetCoop, {hwnd, 2});
        if (probe) {   // only the probe asks for the device capabilities
            const uint32_t caps = g.arena.alloc(96);
            const uint32_t caps_size = 96;
            std::memcpy(g.arena.host(caps, 4), &caps_size, 4);
            CHECK_HR(g.call(ds, ds_slot::GetCaps, {caps}), HR_OK);
            CHECK(g.get32(caps + 4) == 0xF1F);   // the formats a Windows probe reports
        }
        const uint32_t out = g.cell();
        CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(1, 0, 0), out, 0}), HR_OK);   // primary: flags DSBCAPS_PRIMARYBUFFER
        primary = g.get32(out);
        if (probe)
            for (uint16_t c : {1, 2, 4, 6}) CHECK_HR(try_format(c, rate), HR_OK);   // the capability probe
        CHECK_HR(try_format(channels, rate), HR_OK);
        // Zero the whole ring: Lock(0, bytes, &p1, &n1, 0, 0, 0), memset, Unlock(p1, n1, 0, 0), then Play(0, 0, DSBPLAY_LOOPING).
        const uint32_t p1 = g.cell(), n1 = g.cell();
        CHECK_HR(g.call(ring, buf_slot::Lock, {0, ring_bytes, p1, n1, 0, 0, 0}), HR_OK);
        CHECK(g.get32(n1) == ring_bytes);
        std::memset(g.arena.host(g.get32(p1), ring_bytes), 0, ring_bytes);
        CHECK_HR(g.call(ring, buf_slot::Unlock, {g.get32(p1), ring_bytes, 0, 0}), HR_OK);
        CHECK_HR(g.call(ring, buf_slot::Play, {0, 0, 1}), HR_OK);
        return true;
    }
    // 0x7260f7 (one poll of the audio thread): GetStatus, GetCurrentPosition(&play, NULL), write up to the lead.
    uint32_t poll_status() {
        const uint32_t st = g.cell();
        CHECK_HR(g.call(ring, buf_slot::GetStatus, {st}), HR_OK);
        return g.get32(st);
    }
    uint32_t play_cursor() {
        const uint32_t pos = g.cell();
        CHECK_HR(g.call(ring, buf_slot::GetCurrentPosition, {pos, 0}), HR_OK);
        return g.get32(pos);
    }
    // Lock(offset, bytes, &p1, &n1, &p2, &n2, 0), copy `data`, Unlock(p1, n1, p2, n2)
    void write(uint32_t offset, const std::vector<uint8_t>& data) {
        const uint32_t p1 = g.cell(), n1 = g.cell(), p2 = g.cell(), n2 = g.cell();
        CHECK_HR(g.call(ring, buf_slot::Lock, {offset, uint32_t(data.size()), p1, n1, p2, n2, 0}), HR_OK);
        const uint32_t a1 = g.get32(p1), l1 = g.get32(n1), a2 = g.get32(p2), l2 = g.get32(n2);
        std::memcpy(g.arena.host(a1, l1), data.data(), l1);
        if (l2) std::memcpy(g.arena.host(a2, l2), data.data() + l1, l2);
        CHECK_HR(g.call(ring, buf_slot::Unlock, {a1, l1, a2, l2}), HR_OK);
    }
    void shutdown() {   // 0x725bb5: Release secondary, primary, DirectSound
        CHECK(g.call(ring, buf_slot::Release) == 0);
        CHECK(g.call(primary, buf_slot::Release) == 0);
        CHECK(g.call(ds, ds_slot::Release) == 0);
    }
};

std::vector<uint8_t> tone_stereo(uint32_t first_frame, uint32_t frames, uint32_t rate) {
    std::vector<uint8_t> b(size_t(frames) * 4);
    for (uint32_t i = 0; i < frames; ++i) {
        const int16_t v = int16_t(std::lrint(9000.0 * std::sin(2.0 * 3.14159265358979 * 523.25 * double(first_frame + i) / rate)));
        std::memcpy(&b[size_t(i) * 4], &v, 2);
        std::memcpy(&b[size_t(i) * 4 + 2], &v, 2);
    }
    return b;
}

void test_game_sequence() {
    Guest g;
    GameAudio game(g);
    CHECK(game.init(2, 44100, true));
    CHECK(game.ring_bytes == 22048 * 4);   // 0.5 s: (rate * 500 / 1000) & ~15 frames
    // The ring really is what the game asked for.
    const uint32_t caps = g.arena.alloc(20);
    const uint32_t cs = 20;
    std::memcpy(g.arena.host(caps, 4), &cs, 4);
    CHECK_HR(g.call(game.ring, buf_slot::GetCaps, {caps}), HR_OK);
    CHECK(g.get32(caps + 4) == (0x18000u | 0x8u) && g.get32(caps + 8) == game.ring_bytes);   // + DSBCAPS_LOCSOFTWARE
    CHECK((game.poll_status() & 3) == 1);   // playing, not lost
    // Refill loop of the audio thread: keep ~100 ms ahead of the play cursor, at 10 ms steps; the ring is looping so the cursor wraps.
    uint32_t written = 0;   // bytes the game has produced (a ring offset, kept modulo the size)
    uint64_t total_frames = 0;
    bool wrapped = false;
    for (int step = 0; step < 200; ++step) {
        g.sink->advance(480);
        const uint32_t play = game.play_cursor();
        CHECK(play < game.ring_bytes && play % 4 == 0);
        const uint32_t lead = (written + game.ring_bytes - play) % game.ring_bytes;
        const uint32_t want = 4410 * 4;
        if (lead < want) {
            const uint32_t bytes = std::min<uint32_t>(want - lead, 1764 * 4);
            game.write(written, tone_stereo(uint32_t(total_frames), bytes / 4, 44100));
            if (written + bytes >= game.ring_bytes) wrapped = true;   // this write went through both Lock regions
            written = (written + bytes) % game.ring_bytes;
            total_frames += bytes / 4;
        }
    }
    CHECK(wrapped);
    CHECK(g.facade->report().find("failed") == std::string::npos);   // not one DirectSound call failed
    // The device really played what the game wrote: the sink saw a tone of the right frequency (523.25 Hz at 48 kHz).
    CHECK(g.sink->frames_rendered() == 200u * 480u);
    // Teardown and re-creation, as the game does after 100 polls with a frozen cursor.
    game.shutdown();
    CHECK(g.facade->live_objects() == 0);
    GameAudio again(g);
    CHECK(again.init(2, 44100, false));
    uint32_t pos = again.play_cursor();
    CHECK(pos == 0);   // a fresh device: the new ring starts at the beginning
    again.shutdown();
    CHECK(g.arena.live >= 0);

    // 5.1 as the game's surround setting would ask: 6-channel primary and ring; the engine follows with a 6-channel output.
    Guest g6;
    GameAudio sur(g6);
    CHECK(sur.init(6, 44100, true));
    CHECK(g6.sink->actual().channels == 6 && g6.engine->reopen_count() >= 1);
    sur.shutdown();
}

void test_com_behaviour() {
    Guest g;
    uint32_t ds = 0;
    // DirectSoundCreate argument validation.
    CHECK_HR(g.facade->direct_sound_create(0, 0, 0), HR_INVALIDARG);
    CHECK_HR(g.create(&ds, 0, 0x4000), HR_NOAGG);
    CHECK(ds == 0);
    const uint8_t other[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK_HR(g.create(&ds, g.guid(other)), HR_NODRIVER);
    const uint8_t def_playback[16] = {0x00, 0x00, 0xF0, 0xDE, 0x6D, 0x9C, 0xED, 0x47, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03};
    CHECK_HR(g.create(&ds, g.guid(def_playback)), HR_OK);   // DSDEVID_DefaultPlayback is the default device
    CHECK(ds != 0);
    // Reference counting is shared by every interface of one object; Release returns the new count.
    CHECK(g.call(ds, ds_slot::AddRef) == 2);
    const uint32_t pp = g.cell();
    CHECK_HR(g.call(ds, ds_slot::QI, {g.guid(IID_IUnknown), pp}), HR_OK);
    CHECK(g.get32(pp) == ds && g.call(ds, ds_slot::AddRef) == 4);
    CHECK_HR(g.call(ds, ds_slot::QI, {g.guid(IID_IDirectSound8), pp}), HR_NOINTERFACE);   // a legacy DirectSoundCreate object is not IDirectSound8
    CHECK(g.get32(pp) == 0);
    CHECK_HR(g.call(ds, ds_slot::QI, {g.guid(IID_IKsPropertySet), pp}), HR_NOINTERFACE);  // EAX is not offered
    CHECK_HR(g.call(ds, ds_slot::Initialize, {0}), HR_ALREADYINIT);
    CHECK_HR(g.call(ds, ds_slot::Compact), HR_OK);
    for (int i = 0; i < 3; ++i) g.call(ds, ds_slot::Release);
    // Method validation.
    CHECK_HR(g.call(ds, ds_slot::SetCoop, {0xdead, 2}), HR_INVALIDARG);   // not a window
    CHECK_HR(g.call(ds, ds_slot::SetCoop, {0x1234, 9}), HR_INVALIDARG);
    CHECK_HR(g.call(ds, ds_slot::GetCaps, {0}), HR_INVALIDARG);
    const uint32_t small = g.arena.alloc(96);
    const uint32_t wrong = 40;
    std::memcpy(g.arena.host(small, 4), &wrong, 4);
    CHECK_HR(g.call(ds, ds_slot::GetCaps, {small}), HR_INVALIDARG);
    const uint32_t out = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {0, out, 0}), HR_INVALIDARG);                            // no description
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0, 4096, g.wave(2, 44100), 99), out, 0}), HR_INVALIDARG);   // bad dwSize
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0, 4096, g.wave(2, 44100)), out, 5}), HR_NOAGG);
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0, 4096, 0), out, 0}), HR_INVALIDARG);          // secondary without a format
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(1, 0, g.wave(2, 44100)), out, 0}), HR_INVALIDARG);   // primary with a format
    uint8_t blob[18] = {};
    const uint16_t tag = 0x55;   // an unknown format tag
    std::memcpy(blob, &tag, 2);
    blob[2] = 2;
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0, 4096, g.put(blob, 18)), out, 0}), 0x80004001u);   // E_NOTIMPL, as Windows answers for an unknown tag
    // Primary: priority level, format, QueryInterface, refcounts of the shared primary.
    const uint32_t outp = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(1 | 0x10, 0, 0), outp, 0}), HR_OK);   // primary with CTRL3D: the listener lives here
    const uint32_t primary = g.get32(outp);
    CHECK_HR(g.call(ds, ds_slot::SetCoop, {0x1234, 1}), HR_OK);   // normal
    CHECK_HR(g.call(primary, buf_slot::SetFormat, {g.wave(2, 44100)}), HR_PRIOLEVELNEEDED);
    CHECK_HR(g.call(ds, ds_slot::SetCoop, {0x5678, 2}), HR_OK);
    CHECK_HR(g.call(primary, buf_slot::SetFormat, {g.wave(2, 44100)}), HR_OK);
    CHECK_HR(g.call(primary, buf_slot::SetFormat, {g.wave(2, 44100, 16, 0, false) + 0}), HR_OK);
    uint8_t bad[18] = {};   // blockAlign that does not match channels * bits / 8
    const uint16_t pcm = 1, ch = 2, wrongalign = 7, bits = 16;
    std::memcpy(bad, &pcm, 2); std::memcpy(bad + 2, &ch, 2); const uint32_t rate = 44100; std::memcpy(bad + 4, &rate, 4);
    std::memcpy(bad + 12, &wrongalign, 2); std::memcpy(bad + 14, &bits, 2);
    CHECK_HR(g.call(primary, buf_slot::SetFormat, {g.put(bad, 18)}), HR_INVALIDARG);
    const uint32_t outp2 = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(1, 0, 0), outp2, 0}), HR_OK);
    CHECK(g.get32(outp2) == primary);   // the same object again, with one more reference
    CHECK(g.call(primary, buf_slot::AddRef) == 3);
    g.call(primary, buf_slot::Release);
    g.call(primary, buf_slot::Release);
    // Secondary: Lock/Unlock discipline.
    const uint32_t outs = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0x80 | 0x20 | 0x40 | 0x100, 4000, g.wave(2, 48000)), outs, 0}), HR_OK);
    const uint32_t buf = g.get32(outs);
    const uint32_t p1 = g.cell(), n1 = g.cell(), p2 = g.cell(), n2 = g.cell();
    CHECK_HR(g.call(buf, buf_slot::Lock, {0, 100, 0, n1, 0, 0, 0}), HR_INVALIDARG);       // ppvAudioPtr1 is required
    CHECK_HR(g.call(buf, buf_slot::Lock, {3900, 400, p1, n1, 0, 0, 0}), HR_OK);              // wraps but no second region requested: still succeeds (measured)
    CHECK(g.get32(n1) == 100);
    CHECK_HR(g.call(buf, buf_slot::Unlock, {g.get32(p1), 100, 0, 0}), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::Lock, {3900, 400, p1, n1, p2, n2, 0}), HR_OK);
    CHECK(g.get32(n1) == 100 && g.get32(n2) == 300);
    const uint32_t a1 = g.get32(p1), a2 = g.get32(p2);
    CHECK(a2 == a1 - 3900);   // the second region is the start of the same mirror
    const uint32_t q1 = g.cell(), qn1 = g.cell();
    CHECK_HR(g.call(buf, buf_slot::Lock, {0, 8, q1, qn1, 0, 0, 0}), HR_OK);                  // a second Lock while the first is outstanding is fine (measured)
    CHECK_HR(g.call(buf, buf_slot::Unlock, {g.get32(q1), 8, 0, 0}), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::Unlock, {a1 + 4, 96, a2, 300}), HR_INVALIDARG);        // pointers that Lock did not return
    CHECK_HR(g.call(buf, buf_slot::Unlock, {a1, 100, a2, 300}), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::Unlock, {a1, 100, a2, 300}), HR_INVALIDARG);           // not locked any more
    // Controls and status through the guest ABI.
    CHECK_HR(g.call(buf, buf_slot::SetVolume, {uint32_t(-600)}), HR_OK);
    const uint32_t cell = g.cell();
    CHECK_HR(g.call(buf, buf_slot::GetVolume, {cell}), HR_OK);
    CHECK(int32_t(g.get32(cell)) == -600);
    CHECK_HR(g.call(buf, buf_slot::SetVolume, {uint32_t(-10001)}), HR_INVALIDARG);
    CHECK_HR(g.call(buf, buf_slot::SetPan, {5000}), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::SetFrequency, {22050}), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::GetFrequency, {cell}), HR_OK);
    CHECK(g.get32(cell) == 22050);
    CHECK_HR(g.call(buf, buf_slot::Play, {1, 0, 0}), HR_INVALIDARG);                       // dwReserved1 must be 0
    CHECK_HR(g.call(buf, buf_slot::Play, {0, 0, 1}), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::GetStatus, {cell}), HR_OK);
    CHECK((g.get32(cell) & 5) == 5);   // playing + looping
    CHECK_HR(g.call(buf, buf_slot::GetStatus, {0}), HR_INVALIDARG);
    CHECK_HR(g.call(buf, buf_slot::Stop), HR_OK);
    CHECK_HR(g.call(buf, buf_slot::Initialize, {0, 0}), HR_ALREADYINIT);
    // GetFormat: the size query, a full read, a short read.
    CHECK_HR(g.call(buf, buf_slot::GetFormat, {0, 0, cell}), HR_OK);
    CHECK(g.get32(cell) == 18);
    const uint32_t fmt = g.arena.alloc(64);
    CHECK_HR(g.call(buf, buf_slot::GetFormat, {fmt, 64, cell}), HR_OK);
    CHECK(g.get32(cell) == 18 && g.get32(fmt + 4) == 48000);
    CHECK_HR(g.call(buf, buf_slot::GetFormat, {fmt, 8, cell}), HR_OK);
    CHECK(g.get32(cell) == 8);
    // Reference counts: the device outlives its buffers (they hold it), buffers die with their last Release.
    CHECK(g.call(ds, ds_slot::Release) == 0);   // the application's reference to the device is gone ...
    CHECK_HR(g.call(buf, buf_slot::Play, {0, 0, 1}), HR_OK);   // ... the buffer still works
    CHECK(g.call(buf, buf_slot::Release) == 0);
    CHECK(g.call(primary, buf_slot::Release) == 0);
    CHECK(g.facade->live_objects() == 0);   // and then nothing is left
}

void test_duplicate_and_formats_via_abi() {
    Guest g;
    uint32_t ds = 0;
    g.create(&ds);
    g.call(ds, ds_slot::SetCoop, {0x1234, 2});
    const uint32_t out = g.cell();
    // 8-bit mono, 24-bit stereo and float buffers are accepted; a mask with the wrong channel count is not rejected (the mask is advisory).
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0, 800, g.wave(1, 22050, 8)), out, 0}), HR_OK);
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0, 4800, g.wave(2, 48000, 24)), out, 0}), HR_OK);
    const uint32_t src = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0x80, 4000, g.wave(2, 44100)), src, 0}), HR_OK);
    const uint32_t s = g.get32(src);
    // Fill through Lock/Unlock, duplicate, check the copy has the data and its own state.
    const uint32_t p1 = g.cell(), n1 = g.cell();
    CHECK_HR(g.call(s, buf_slot::Lock, {0, 4000, p1, n1, 0, 0, 0}), HR_OK);
    std::memset(g.arena.host(g.get32(p1), 4000), 0x42, 4000);
    CHECK_HR(g.call(s, buf_slot::Unlock, {g.get32(p1), 4000, 0, 0}), HR_OK);
    const uint32_t dup = g.cell();
    CHECK_HR(g.call(ds, ds_slot::Duplicate, {s, dup}), HR_OK);
    const uint32_t d = g.get32(dup);
    CHECK(d != s);
    CHECK_HR(g.call(d, buf_slot::Lock, {0, 4, p1, n1, 0, 0, 0}), HR_OK);
    CHECK(*g.arena.host(g.get32(p1), 1) == 0x42);
    CHECK_HR(g.call(ds, ds_slot::Duplicate, {0xbad, dup}), HR_INVALIDARG);
}

void test_3d_and_notify_via_abi() {
    Guest g;
    uint32_t ds = 0;
    g.create(&ds);
    g.call(ds, ds_slot::SetCoop, {0x1234, 2});
    const uint32_t out = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(1 | 0x10, 0, 0), out, 0}), HR_OK);
    const uint32_t primary = g.get32(out);
    const uint32_t pp = g.cell();
    // The listener comes from the primary buffer (and only when it has CTRL3D).
    CHECK_HR(g.call(primary, buf_slot::QI, {g.guid(IID_IDirectSound3DListener), pp}), HR_OK);
    const uint32_t listener = g.get32(pp);
    auto fbits = [](float f) { uint32_t w; std::memcpy(&w, &f, 4); return w; };
    CHECK_HR(g.call(listener, 14, {fbits(1), fbits(2), fbits(3), 0}), HR_OK);   // SetPosition(x, y, z, DS3D_IMMEDIATE)
    const uint32_t vec = g.arena.alloc(12);
    CHECK_HR(g.call(listener, 7, {vec}), HR_OK);                                // GetPosition
    float pos[3];
    std::memcpy(pos, g.arena.host(vec, 12), 12);
    CHECK(pos[0] == 1 && pos[1] == 2 && pos[2] == 3);
    CHECK_HR(g.call(listener, 12, {fbits(11), 0}), HR_INVALIDARG);            // SetDopplerFactor beyond 10
    // Deferred settings apply at CommitDeferredSettings only.
    CHECK_HR(g.call(listener, 14, {fbits(9), fbits(9), fbits(9), 1}), HR_OK);   // DS3D_DEFERRED
    CHECK_HR(g.call(listener, 7, {vec}), HR_OK);
    std::memcpy(pos, g.arena.host(vec, 12), 12);
    CHECK(pos[0] == 9);                                                      // Get returns what was set (as Wine does) ...
    CHECK(g.engine->device().listener().position.x == 1);                    // ... but the mixer still has the old scene
    CHECK_HR(g.call(listener, 17), HR_OK);                                      // CommitDeferredSettings
    CHECK(g.engine->device().listener().position.x == 9);
    CHECK_HR(g.call(listener, 7, {vec}), HR_OK);
    std::memcpy(pos, g.arena.host(vec, 12), 12);
    CHECK(pos[0] == 9);
    // A 3D buffer: mono with CTRL3D, QueryInterface gives IDirectSound3DBuffer, a stereo or plain buffer does not.
    const uint32_t outb = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0x10, 960, g.wave(1, 48000)), outb, 0}), HR_OK);
    const uint32_t b = g.get32(outb);
    CHECK_HR(g.call(b, buf_slot::QI, {g.guid(IID_IDirectSound3DBuffer), pp}), HR_OK);
    const uint32_t b3 = g.get32(pp);
    CHECK_HR(g.call(b3, 19, {fbits(4), fbits(0), fbits(0), 0}), HR_OK);         // SetPosition
    CHECK_HR(g.call(b3, 17, {fbits(2), 0}), HR_OK);                             // SetMinDistance
    CHECK_HR(g.call(b3, 16, {fbits(100), 0}), HR_OK);                           // SetMaxDistance
    CHECK_HR(g.call(b3, 17, {fbits(-1), 0}), HR_INVALIDARG);                  // a negative minimum distance
    const uint32_t all = g.arena.alloc(64);
    const uint32_t size64 = 64;
    std::memcpy(g.arena.host(all, 4), &size64, 4);
    CHECK_HR(g.call(b3, 3, {all}), HR_OK);                                      // GetAllParameters
    float px;
    std::memcpy(&px, g.arena.host(all + 4, 4), 4);
    CHECK(px == 4);
    CHECK_HR(g.call(b3, 18, {7, 0}), HR_INVALIDARG);                          // SetMode out of range
    CHECK_HR(g.call(b, buf_slot::QI, {g.guid(IID_IDirectSoundNotify), pp}), HR_NOINTERFACE);   // no CTRLPOSITIONNOTIFY
    const uint32_t outn = g.cell();
    CHECK_HR(g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0x100, 9600 * 4, g.wave(2, 48000)), outn, 0}), HR_OK);
    const uint32_t nb = g.get32(outn);
    CHECK_HR(g.call(nb, buf_slot::QI, {g.guid(IID_IDirectSound3DBuffer), pp}), HR_NOINTERFACE);  // not a 3D buffer
    CHECK_HR(g.call(nb, buf_slot::QI, {g.guid(IID_IDirectSoundNotify), pp}), HR_OK);
    const uint32_t notify = g.get32(pp);
    uint32_t list[4] = {2400 * 4, 0xE1, 0xFFFFFFFFu, 0xE2};   // DSBPOSITIONNOTIFY: offset, event handle
    CHECK_HR(g.call(notify, 3, {2, g.put(list, 16)}), HR_OK);
    CHECK_HR(g.call(notify, 3, {2, 0}), HR_INVALIDARG);
    g.call(nb, buf_slot::Play, {0, 0, 1});
    g.sink->advance(4800);
    CHECK(g.host.events == std::vector<uint32_t>({0xE1}));
    g.call(nb, buf_slot::Stop);
    g.sink->advance(1);
    CHECK(g.host.events == std::vector<uint32_t>({0xE1, 0xE2}));
}

}  // namespace

// Hostile arguments: every method of every interface with random words (wild pointers, huge sizes, stale `this`) must fail or succeed without a host fault,
// and the only guest memory it may touch is the arena (Memory::host answers nullptr for everything else; the facade must check).
void test_hostile_arguments() {
    Guest g;
    uint32_t ds = 0;
    g.create(&ds);
    g.call(ds, ds_slot::SetCoop, {0x1234, 2});
    const uint32_t out = g.cell();
    g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(1 | 0x10, 0, 0), out, 0});
    const uint32_t primary = g.get32(out);
    g.call(ds, ds_slot::CreateSoundBuffer, {g.desc(0x10 | 0x100 | 0x80, 4000, g.wave(1, 48000)), out, 0});
    const uint32_t buf = g.get32(out);
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    const auto& methods = DsoundGuestFacade::methods();
    uint32_t objects[5] = {ds, buf, 0, primary, buf};
    const uint32_t pp = g.cell();
    g.call(buf, buf_slot::QI, {g.guid(IID_IDirectSound3DBuffer), pp});
    objects[2] = g.get32(pp);
    g.call(primary, buf_slot::QI, {g.guid(IID_IDirectSound3DListener), pp});
    objects[3] = g.get32(pp);
    g.call(buf, buf_slot::QI, {g.guid(IID_IDirectSoundNotify), pp});
    objects[4] = g.get32(pp);
    uint64_t calls = 0;
    for (int round = 0; round < 3000; ++round) {
        const size_t m = rnd() % methods.size();
        const MethodDesc& d = methods[m];
        if (d.slot == 2) continue;   // Release would destroy the objects the loop uses
        uint32_t args[10] = {};
        args[0] = (rnd() % 7 == 0) ? rnd() : objects[size_t(d.iface)];   // sometimes a stale or foreign `this`
        for (unsigned k = 1; k < d.words; ++k) {
            const uint32_t pick = rnd() % 6;
            args[k] = pick == 0 ? 0 : pick == 1 ? rnd() : pick == 2 ? 0xFFFFFFF0u : pick == 3 ? g.cell() : pick == 4 ? uint32_t(rnd() % 64) : g.arena.alloc(0) ;
        }
        g.facade->call(m, args);
        ++calls;
    }
    CHECK(calls > 2000);
    CHECK(g.facade->live_objects() >= 3);   // the objects are still there and the facade still answers
    uint32_t pcell = g.cell();
    CHECK_HR(g.call(buf, buf_slot::GetStatus, {pcell}), HR_OK);
    // `this` that is not a COM pointer of this facade: invalid argument, never a fault.
    const uint32_t bogus[1] = {0xDEADBEEFu};
    CHECK(g.facade->call(0, bogus) != 0);
}

int main() {
    // Twice: guest address 0 of the arena at a low address and above 2 GiB (host addresses are unrelated to either).
    for (uint32_t base : {0x10000u, 0xA0000000u}) {
        g_arena_base = base;
        test_game_sequence();
        test_com_behaviour();
        test_duplicate_and_formats_via_abi();
        test_3d_and_notify_via_abi();
        test_hostile_arguments();
    }
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
