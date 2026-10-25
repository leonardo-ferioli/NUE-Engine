// COM facade tests: the DirectSound objects of dsound.dll (src/legacy/dsound/dll/com_facade.cpp) driven through the real interfaces of the Windows SDK's
// dsound.h, over an Engine with a deterministic sink. No sound card, no game, no window other than the desktop.
// The central test replays the call sequence of a real streaming client (a primary buffer, a 0.5 s looping secondary ring polled every 10 ms: probe of
// 1/2/4/6 channels, refill with wrapping Locks, teardown, re-creation as after a stall) call for call; the others check the COM rules, argument
// validation and the 3D / notification interfaces.
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <ks.h>
#include <ksmedia.h>

#include <memory>

#include "com_facade.hpp"
#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

namespace {

struct Rig final : com::DeviceProvider {
    std::unique_ptr<Engine> engine;
    ManualSink* sink = nullptr;
    std::unique_ptr<com::Facade> facade;
    int released = 0;
    Rig() {
        EngineConfig c;
        c.sample_rate = 48000;
        auto s = std::make_unique<ManualSink>(256, 480);
        sink = s.get();
        engine = Engine::create(c, std::move(s));
        engine->start();
        facade = std::make_unique<com::Facade>(*this);
    }
    dsx::Device* acquire() override { return &engine->device(); }
    void release() override { ++released; }
    HRESULT create(IDirectSound** ds, const GUID* guid = nullptr, IUnknown* outer = nullptr) { return facade->create(guid, ds, outer); }
};

WAVEFORMATEXTENSIBLE wave(uint16_t channels, uint32_t rate, uint16_t bits = 16, uint32_t mask = 0, bool extensible = false) {
    WAVEFORMATEXTENSIBLE w = {};
    w.Format.wFormatTag = extensible ? WAVE_FORMAT_EXTENSIBLE : WAVE_FORMAT_PCM;
    w.Format.nChannels = channels;
    w.Format.nSamplesPerSec = rate;
    w.Format.wBitsPerSample = bits;
    w.Format.nBlockAlign = WORD(channels * bits / 8);
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    if (extensible) {
        w.Format.cbSize = 22;
        w.Samples.wValidBitsPerSample = bits;
        w.dwChannelMask = mask;
        w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
    return w;
}
DSBUFFERDESC desc(DWORD flags, DWORD bytes, WAVEFORMATEX* format) {
    DSBUFFERDESC d = {};
    d.dwSize = sizeof(DSBUFFERDESC);
    d.dwFlags = flags;
    d.dwBufferBytes = bytes;
    d.lpwfxFormat = format;
    return d;
}

// ---- a streaming client, as the game does it ----------------------------------------------------------------------------------------------

struct Client {
    Rig& g;
    IDirectSound* ds = nullptr;
    IDirectSoundBuffer *primary = nullptr, *ring = nullptr;
    uint32_t ring_bytes = 0;
    explicit Client(Rig& rig) : g(rig) {}

    // SetFormat on the primary with `channels` channels (plain WAVEFORMATEX up to 2, WAVEFORMATEXTENSIBLE with mask 0x33 / 0x3F above); when that works
    // it releases the previous secondary buffer and creates a new 0.5 s ring (flags 0x18000) of that format.
    HRESULT try_format(uint16_t channels, uint32_t rate) {
        const bool ext = channels > 2;
        const uint32_t mask = channels == 6 ? 0x3F : channels == 4 ? 0x33 : 0;
        WAVEFORMATEXTENSIBLE w = wave(channels, rate, 16, mask, ext);
        const HRESULT hr = primary->SetFormat(&w.Format);
        if (FAILED(hr)) return hr;
        if (ring) {
            ring->Release();
            ring = nullptr;
        }
        const uint32_t frames = (rate * 500 / 1000) & ~0xFu;
        ring_bytes = frames * channels * 2;
        DSBUFFERDESC d = desc(0x18000, ring_bytes, &w.Format);
        return ds->CreateSoundBuffer(&d, &ring, nullptr);
    }
    bool init(uint16_t channels, uint32_t rate, bool probe) {
        if (g.create(&ds) != DS_OK) return false;
        CHECK_HR(ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY), DS_OK);
        if (probe) {   // only the probe asks for the device capabilities
            DSCAPS caps = {};
            caps.dwSize = sizeof(DSCAPS);
            CHECK_HR(ds->GetCaps(&caps), DS_OK);
            CHECK(caps.dwFlags == 0xF1F);   // the formats a Windows probe reports
        }
        DSBUFFERDESC pd = desc(DSBCAPS_PRIMARYBUFFER, 0, nullptr);
        CHECK_HR(ds->CreateSoundBuffer(&pd, &primary, nullptr), DS_OK);
        if (probe)
            for (uint16_t c : {1, 2, 4, 6}) CHECK_HR(try_format(c, rate), DS_OK);   // the capability probe
        CHECK_HR(try_format(channels, rate), DS_OK);
        // Zero the whole ring: Lock(0, bytes, &p1, &n1, 0, 0, 0), memset, Unlock(p1, n1, 0, 0), then Play(0, 0, DSBPLAY_LOOPING).
        void* p1 = nullptr;
        DWORD n1 = 0;
        CHECK_HR(ring->Lock(0, ring_bytes, &p1, &n1, nullptr, nullptr, 0), DS_OK);
        CHECK(n1 == ring_bytes);
        std::memset(p1, 0, ring_bytes);
        CHECK_HR(ring->Unlock(p1, n1, nullptr, 0), DS_OK);
        CHECK_HR(ring->Play(0, 0, DSBPLAY_LOOPING), DS_OK);
        return true;
    }
    uint32_t poll_status() {
        DWORD st = 0;
        CHECK_HR(ring->GetStatus(&st), DS_OK);
        return st;
    }
    uint32_t play_cursor() {
        DWORD pos = 0;
        CHECK_HR(ring->GetCurrentPosition(&pos, nullptr), DS_OK);
        return pos;
    }
    // Lock(offset, bytes, &p1, &n1, &p2, &n2, 0), copy `data`, Unlock(p1, n1, p2, n2)
    void write(uint32_t offset, const std::vector<uint8_t>& data) {
        void *p1 = nullptr, *p2 = nullptr;
        DWORD n1 = 0, n2 = 0;
        CHECK_HR(ring->Lock(offset, DWORD(data.size()), &p1, &n1, &p2, &n2, 0), DS_OK);
        std::memcpy(p1, data.data(), n1);
        if (n2) std::memcpy(p2, data.data() + n1, n2);
        CHECK_HR(ring->Unlock(p1, n1, p2, n2), DS_OK);
    }
    void shutdown() {   // Release secondary, primary, DirectSound
        CHECK(ring->Release() == 0);
        CHECK(primary->Release() == 0);
        CHECK(ds->Release() == 0);
        ring = primary = nullptr;
        ds = nullptr;
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

void test_streaming_client_sequence() {
    Rig g;
    Client game(g);
    CHECK(game.init(2, 44100, true));
    CHECK(game.ring_bytes == 22048 * 4);   // 0.5 s: (rate * 500 / 1000) & ~15 frames
    // The ring really is what the client asked for.
    DSBCAPS caps = {};
    caps.dwSize = sizeof(DSBCAPS);
    CHECK_HR(game.ring->GetCaps(&caps), DS_OK);
    CHECK(caps.dwFlags == (0x18000u | 0x8u) && caps.dwBufferBytes == game.ring_bytes);   // + DSBCAPS_LOCSOFTWARE
    CHECK((game.poll_status() & 3) == 1);   // playing, not lost
    // Refill loop of the audio thread: keep ~100 ms ahead of the play cursor, at 10 ms steps; the ring is looping so the cursor wraps.
    uint32_t written = 0;   // bytes produced (a ring offset, kept modulo the size)
    uint64_t total_frames = 0;
    bool wrapped = false;
    uint32_t last_play = ~0u;
    int frozen = 0, max_frozen = 0;
    for (int step = 0; step < 200; ++step) {
        g.sink->advance(480);
        const uint32_t play = game.play_cursor();
        CHECK(play < game.ring_bytes && play % 4 == 0);
        frozen = (play == last_play) ? frozen + 1 : 0;   // the client rebuilds its audio when the cursor stands still for 100 polls
        max_frozen = std::max(max_frozen, frozen);
        last_play = play;
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
    CHECK(max_frozen < 100 && max_frozen == 0);   // the cursor moves at every poll
    CHECK(g.facade->report().find("failed") == std::string::npos);   // not one DirectSound call failed
    CHECK(g.sink->frames_rendered() == 200u * 480u);
    // Teardown and re-creation, as the client does after 100 polls with a frozen cursor.
    game.shutdown();
    CHECK(g.facade->live_objects() == 0 && g.released == 1);
    Client again(g);
    CHECK(again.init(2, 44100, false));
    CHECK(again.play_cursor() == 0);   // a fresh device: the new ring starts at the beginning
    again.shutdown();

    // 5.1 as a surround setting would ask: 6-channel primary and ring; the engine follows with a 6-channel output.
    Rig g6;
    Client sur(g6);
    CHECK(sur.init(6, 44100, true));
    CHECK(g6.sink->actual().channels == 6 && g6.engine->reopen_count() >= 1);
    sur.shutdown();
}

void test_com_behaviour() {
    Rig g;
    IDirectSound* ds = nullptr;
    // DirectSoundCreate argument validation.
    CHECK_HR(g.facade->create(nullptr, nullptr, nullptr), E_INVALIDARG);
    CHECK_HR(g.create(&ds, nullptr, reinterpret_cast<IUnknown*>(0x4000)), CLASS_E_NOAGGREGATION);
    CHECK(ds == nullptr);
    const GUID other = {0x04030201, 0x0605, 0x0807, {9, 10, 11, 12, 13, 14, 15, 16}};
    CHECK_HR(g.create(&ds, &other), DSERR_NODRIVER);
    CHECK_HR(g.create(&ds, &DSDEVID_DefaultPlayback), DS_OK);   // DSDEVID_DefaultPlayback is the default device
    CHECK(ds != nullptr);
    // Reference counting is shared by every interface of one object; Release returns the new count.
    CHECK(ds->AddRef() == 2);
    void* pp = nullptr;
    CHECK_HR(ds->QueryInterface(IID_IUnknown, &pp), DS_OK);
    CHECK(pp == ds && ds->AddRef() == 4);
    CHECK_HR(ds->QueryInterface(IID_IDirectSound8, &pp), E_NOINTERFACE);   // a legacy DirectSoundCreate object is not IDirectSound8
    CHECK(pp == nullptr);
    CHECK_HR(ds->QueryInterface(IID_IKsPropertySet, &pp), E_NOINTERFACE);  // EAX is not offered
    CHECK_HR(ds->Initialize(nullptr), DSERR_ALREADYINITIALIZED);
    CHECK_HR(ds->Compact(), DS_OK);
    for (int i = 0; i < 3; ++i) ds->Release();
    // Method validation.
    CHECK_HR(ds->SetCooperativeLevel(reinterpret_cast<HWND>(uintptr_t(0xdead)), DSSCL_PRIORITY), E_INVALIDARG);   // not a window
    CHECK_HR(ds->SetCooperativeLevel(GetDesktopWindow(), 9), E_INVALIDARG);
    CHECK_HR(ds->GetCaps(nullptr), E_INVALIDARG);
    DSCAPS tiny = {};
    tiny.dwSize = 40;
    CHECK_HR(ds->GetCaps(&tiny), E_INVALIDARG);
    IDirectSoundBuffer* b = nullptr;
    WAVEFORMATEXTENSIBLE st = wave(2, 44100);
    CHECK_HR(ds->CreateSoundBuffer(nullptr, &b, nullptr), E_INVALIDARG);                            // no description
    DSBUFFERDESC d = desc(0, 4096, &st.Format);
    d.dwSize = 99;
    CHECK_HR(ds->CreateSoundBuffer(&d, &b, nullptr), E_INVALIDARG);                                 // bad dwSize
    d = desc(0, 4096, &st.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &b, reinterpret_cast<IUnknown*>(5)), CLASS_E_NOAGGREGATION);
    d = desc(0, 4096, nullptr);
    CHECK_HR(ds->CreateSoundBuffer(&d, &b, nullptr), E_INVALIDARG);                                 // secondary without a format
    d = desc(DSBCAPS_PRIMARYBUFFER, 0, &st.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &b, nullptr), E_INVALIDARG);                                 // primary with a format
    WAVEFORMATEX unk = {};
    unk.wFormatTag = 0x55;   // an unknown format tag
    unk.nChannels = 2;
    d = desc(0, 4096, &unk);
    CHECK_HR(ds->CreateSoundBuffer(&d, &b, nullptr), E_NOTIMPL);                                    // as Windows answers for an unknown tag
    // Primary: priority level, format, QueryInterface, refcounts of the shared primary.
    IDirectSoundBuffer* primary = nullptr;
    d = desc(DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, nullptr);   // primary with CTRL3D: the listener lives here
    CHECK_HR(ds->CreateSoundBuffer(&d, &primary, nullptr), DS_OK);
    CHECK_HR(ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_NORMAL), DS_OK);
    CHECK_HR(primary->SetFormat(&st.Format), DSERR_PRIOLEVELNEEDED);
    CHECK_HR(ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY), DS_OK);
    CHECK_HR(primary->SetFormat(&st.Format), DS_OK);
    CHECK_HR(primary->SetFormat(&st.Format), DS_OK);
    WAVEFORMATEX bad = st.Format;   // blockAlign that does not match channels * bits / 8
    bad.nBlockAlign = 7;
    CHECK_HR(primary->SetFormat(&bad), E_INVALIDARG);
    WAVEFORMATEX unk_set = unk;
    unk_set.nBlockAlign = 4;
    unk_set.wBitsPerSample = 16;
    CHECK_HR(primary->SetFormat(&unk_set), DS_OK);   // measured: an unknown tag on SetFormat is accepted and ignored
    IDirectSoundBuffer* primary2 = nullptr;
    d = desc(DSBCAPS_PRIMARYBUFFER, 0, nullptr);
    CHECK_HR(ds->CreateSoundBuffer(&d, &primary2, nullptr), DS_OK);
    CHECK(primary2 == primary);   // the same object again, with one more reference
    CHECK(primary->AddRef() == 3);
    primary->Release();
    primary->Release();
    // Secondary: Lock/Unlock discipline.
    IDirectSoundBuffer* buf = nullptr;
    WAVEFORMATEXTENSIBLE w48 = wave(2, 48000);
    d = desc(DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLPOSITIONNOTIFY, 4000, &w48.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &buf, nullptr), DS_OK);
    void *p1 = nullptr, *p2 = nullptr;
    DWORD n1 = 0, n2 = 0;
    CHECK_HR(buf->Lock(0, 100, nullptr, &n1, nullptr, nullptr, 0), E_INVALIDARG);        // ppvAudioPtr1 is required
    CHECK_HR(buf->Lock(3900, 400, &p1, &n1, nullptr, nullptr, 0), DS_OK);                 // wraps but no second region requested: still succeeds (measured)
    CHECK(n1 == 100);
    CHECK_HR(buf->Unlock(p1, 100, nullptr, 0), DS_OK);
    CHECK_HR(buf->Lock(3900, 400, &p1, &n1, &p2, &n2, 0), DS_OK);
    CHECK(n1 == 100 && n2 == 300);
    uint8_t* a1 = static_cast<uint8_t*>(p1);
    uint8_t* a2 = static_cast<uint8_t*>(p2);
    CHECK(a2 == a1 - 3900);   // the second region is the start of the same mirror
    void* q1 = nullptr;
    DWORD qn1 = 0;
    CHECK_HR(buf->Lock(0, 8, &q1, &qn1, nullptr, nullptr, 0), DS_OK);                      // a second Lock while the first is outstanding is fine (measured)
    CHECK_HR(buf->Unlock(q1, 8, nullptr, 0), DS_OK);
    CHECK_HR(buf->Unlock(a1 + 4, 96, a2, 300), E_INVALIDARG);                              // pointers that Lock did not return
    CHECK_HR(buf->Unlock(a1, 100, a2, 300), DS_OK);
    CHECK_HR(buf->Unlock(a1, 100, a2, 300), E_INVALIDARG);                                 // not locked any more
    // Controls and status through the interface.
    CHECK_HR(buf->SetVolume(-600), DS_OK);
    LONG lv = 0;
    CHECK_HR(buf->GetVolume(&lv), DS_OK);
    CHECK(lv == -600);
    CHECK_HR(buf->SetVolume(-10001), E_INVALIDARG);
    CHECK_HR(buf->SetPan(5000), DS_OK);
    CHECK_HR(buf->SetFrequency(22050), DS_OK);
    DWORD dv = 0;
    CHECK_HR(buf->GetFrequency(&dv), DS_OK);
    CHECK(dv == 22050);
    CHECK_HR(buf->Play(1, 0, 0), E_INVALIDARG);                                            // dwReserved1 must be 0
    CHECK_HR(buf->Play(0, 0, DSBPLAY_LOOPING), DS_OK);
    CHECK_HR(buf->GetStatus(&dv), DS_OK);
    CHECK((dv & 5) == 5);   // playing + looping
    CHECK_HR(buf->GetStatus(nullptr), E_INVALIDARG);
    CHECK_HR(buf->Stop(), DS_OK);
    CHECK_HR(buf->Initialize(nullptr, nullptr), DSERR_ALREADYINITIALIZED);
    // GetFormat: the size query, a full read, a short read.
    CHECK_HR(buf->GetFormat(nullptr, 0, &dv), DS_OK);
    CHECK(dv == 18);
    WAVEFORMATEXTENSIBLE got = {};
    CHECK_HR(buf->GetFormat(&got.Format, sizeof(got), &dv), DS_OK);
    CHECK(dv == 18 && got.Format.nSamplesPerSec == 48000);
    CHECK_HR(buf->GetFormat(&got.Format, 8, &dv), DS_OK);
    CHECK(dv == 8);
    // Reference counts: the device outlives its buffers (they hold it), buffers die with their last Release.
    CHECK(ds->Release() == 0);   // the application's reference to the device is gone ...
    CHECK_HR(buf->Play(0, 0, DSBPLAY_LOOPING), DS_OK);   // ... the buffer still works
    CHECK(buf->Release() == 0);
    CHECK(primary->Release() == 0);
    CHECK(g.facade->live_objects() == 0);   // and then nothing is left
}

void test_duplicate_and_formats() {
    Rig g;
    IDirectSound* ds = nullptr;
    g.create(&ds);
    ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    IDirectSoundBuffer* out = nullptr;
    // 8-bit mono, 24-bit stereo and float buffers are accepted.
    WAVEFORMATEXTENSIBLE m8 = wave(1, 22050, 8);
    DSBUFFERDESC d = desc(0, 800, &m8.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &out, nullptr), DS_OK);
    WAVEFORMATEXTENSIBLE s24 = wave(2, 48000, 24, 0, true);
    s24.dwChannelMask = 3;
    d = desc(0, 4800, &s24.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &out, nullptr), DS_OK);
    WAVEFORMATEXTENSIBLE fl = wave(2, 48000, 32, 3, true);
    fl.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    d = desc(0, 9600, &fl.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &out, nullptr), DS_OK);
    WAVEFORMATEXTENSIBLE nine = wave(9, 48000, 16, 0x1FF, true);
    d = desc(0, 9600, &nine.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &out, nullptr), E_INVALIDARG);   // measured: 9 channels
    WAVEFORMATEXTENSIBLE plain6 = wave(6, 48000);
    d = desc(0, 9600, &plain6.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &out, nullptr), E_INVALIDARG);   // more than 2 channels need WAVEFORMATEXTENSIBLE
    // Fill through Lock/Unlock, duplicate, check the copy has the data and its own state.
    IDirectSoundBuffer* src = nullptr;
    WAVEFORMATEXTENSIBLE st = wave(2, 44100);
    d = desc(DSBCAPS_CTRLVOLUME, 4000, &st.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &src, nullptr), DS_OK);
    void* p1 = nullptr;
    DWORD n1 = 0;
    CHECK_HR(src->Lock(0, 4000, &p1, &n1, nullptr, nullptr, 0), DS_OK);
    std::memset(p1, 0x42, 4000);
    CHECK_HR(src->Unlock(p1, 4000, nullptr, 0), DS_OK);
    IDirectSoundBuffer* dup = nullptr;
    CHECK_HR(ds->DuplicateSoundBuffer(src, &dup), DS_OK);
    CHECK(dup != src);
    CHECK_HR(dup->Lock(0, 4, &p1, &n1, nullptr, nullptr, 0), DS_OK);
    CHECK(*static_cast<uint8_t*>(p1) == 0x42);
    CHECK_HR(dup->Unlock(p1, 4, nullptr, 0), DS_OK);
    CHECK_HR(ds->DuplicateSoundBuffer(reinterpret_cast<IDirectSoundBuffer*>(0xbad0), &dup), E_INVALIDARG);
}

void test_3d_and_notify() {
    Rig g;
    IDirectSound* ds = nullptr;
    g.create(&ds);
    ds->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY);
    IDirectSoundBuffer* primary = nullptr;
    DSBUFFERDESC d = desc(DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, nullptr);
    CHECK_HR(ds->CreateSoundBuffer(&d, &primary, nullptr), DS_OK);
    // The listener comes from the primary buffer (and only when it has CTRL3D).
    IDirectSound3DListener* listener = nullptr;
    CHECK_HR(primary->QueryInterface(IID_IDirectSound3DListener, reinterpret_cast<void**>(&listener)), DS_OK);
    CHECK_HR(listener->SetPosition(1, 2, 3, DS3D_IMMEDIATE), DS_OK);
    D3DVECTOR pos = {};
    CHECK_HR(listener->GetPosition(&pos), DS_OK);
    CHECK(pos.x == 1 && pos.y == 2 && pos.z == 3);
    CHECK_HR(listener->SetDopplerFactor(11, DS3D_IMMEDIATE), E_INVALIDARG);   // beyond 10
    // Deferred settings apply at CommitDeferredSettings only.
    CHECK_HR(listener->SetPosition(9, 9, 9, DS3D_DEFERRED), DS_OK);
    CHECK_HR(listener->GetPosition(&pos), DS_OK);
    CHECK(pos.x == 9);                                                          // Get returns what was set ...
    CHECK(g.engine->device().listener().position.x == 1);                       // ... but the mixer still has the old scene
    CHECK_HR(listener->CommitDeferredSettings(), DS_OK);
    CHECK(g.engine->device().listener().position.x == 9);
    DS3DLISTENER all = {};
    all.dwSize = sizeof(all);
    CHECK_HR(listener->GetAllParameters(&all), DS_OK);
    CHECK(all.vPosition.x == 9 && all.flDopplerFactor == 1.0f);
    // A 3D buffer: mono with CTRL3D, QueryInterface gives IDirectSound3DBuffer, a buffer without CTRL3D does not.
    IDirectSoundBuffer* b = nullptr;
    WAVEFORMATEXTENSIBLE mono = wave(1, 48000);
    d = desc(DSBCAPS_CTRL3D, 960, &mono.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &b, nullptr), DS_OK);
    IDirectSound3DBuffer* b3 = nullptr;
    CHECK_HR(b->QueryInterface(IID_IDirectSound3DBuffer, reinterpret_cast<void**>(&b3)), DS_OK);
    CHECK_HR(b3->SetPosition(4, 0, 0, DS3D_IMMEDIATE), DS_OK);
    CHECK_HR(b3->SetMinDistance(2, DS3D_IMMEDIATE), DS_OK);
    CHECK_HR(b3->SetMaxDistance(100, DS3D_IMMEDIATE), DS_OK);
    CHECK_HR(b3->SetMinDistance(-1, DS3D_IMMEDIATE), E_INVALIDARG);            // a negative minimum distance
    DS3DBUFFER all3 = {};
    all3.dwSize = sizeof(all3);
    CHECK_HR(b3->GetAllParameters(&all3), DS_OK);
    CHECK(all3.vPosition.x == 4 && all3.flMinDistance == 2 && all3.flMaxDistance == 100);
    CHECK_HR(b3->SetMode(7, DS3D_IMMEDIATE), E_INVALIDARG);                    // SetMode out of range
    CHECK_HR(b->QueryInterface(IID_IDirectSoundNotify, reinterpret_cast<void**>(&listener)), E_NOINTERFACE);   // no CTRLPOSITIONNOTIFY
    IDirectSoundBuffer* nb = nullptr;
    WAVEFORMATEXTENSIBLE st = wave(2, 48000);
    d = desc(DSBCAPS_CTRLPOSITIONNOTIFY, 9600 * 4, &st.Format);
    CHECK_HR(ds->CreateSoundBuffer(&d, &nb, nullptr), DS_OK);
    void* unused = nullptr;
    CHECK_HR(nb->QueryInterface(IID_IDirectSound3DBuffer, &unused), E_NOINTERFACE);   // not a 3D buffer
    IDirectSoundNotify* notify = nullptr;
    CHECK_HR(nb->QueryInterface(IID_IDirectSoundNotify, reinterpret_cast<void**>(&notify)), DS_OK);
    HANDLE e1 = CreateEventA(nullptr, TRUE, FALSE, nullptr), e2 = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    DSBPOSITIONNOTIFY list[2] = {{2400 * 4, e1}, {DSBPN_OFFSETSTOP, e2}};
    CHECK_HR(notify->SetNotificationPositions(2, list), DS_OK);
    CHECK_HR(notify->SetNotificationPositions(2, nullptr), E_INVALIDARG);
    nb->Play(0, 0, DSBPLAY_LOOPING);
    g.sink->advance(4800);
    CHECK(WaitForSingleObject(e1, 0) == WAIT_OBJECT_0 && WaitForSingleObject(e2, 0) == WAIT_TIMEOUT);
    nb->Stop();
    g.sink->advance(1);
    CHECK(WaitForSingleObject(e2, 0) == WAIT_OBJECT_0);
    CloseHandle(e1);
    CloseHandle(e2);
}

void test_speaker_config_and_caps() {
    Rig g;
    IDirectSound* ds = nullptr;
    g.create(&ds);
    DWORD config = 0;
    CHECK_HR(ds->GetSpeakerConfig(&config), DS_OK);
    CHECK((config & 0xFFFF) == DSSPEAKER_STEREO);
    CHECK_HR(ds->GetSpeakerConfig(nullptr), E_INVALIDARG);
    CHECK_HR(ds->SetSpeakerConfig(DSSPEAKER_5POINT1), DS_OK);   // accepted: the output follows the device and the primary format
    DSCAPS caps = {};
    caps.dwSize = sizeof(caps);
    CHECK_HR(ds->GetCaps(&caps), DS_OK);
    CHECK(caps.dwFlags == 0xF1F && caps.dwMinSecondarySampleRate == 100 && caps.dwMaxSecondarySampleRate == 200000);
    CHECK(caps.dwMaxHwMixingAllBuffers == 0 && caps.dwPrimaryBuffers == 1);
}

}  // namespace

int main() {
    test_streaming_client_sequence();
    test_com_behaviour();
    test_duplicate_and_formats();
    test_3d_and_notify();
    test_speaker_config_and_caps();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
