// The layer on the native audio stack end to end, headless: nue_init on the null backend with its deterministic virtual clock, the native sink
// (a nue_audio_stream with a callback), a streaming client writing a looping ring while the clock is advanced, and the frames the virtual device
// "played" read back and checked. Same call sequence twice => the same output hash.
#include <string>

#include "nue/nue.h"
#include "nue/null_backend.h"
#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

namespace {

struct Nue {
    bool ok = false;
    Nue() {
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.app_name = "dsound test";
        d.app_id = "dsound-test";
        d.org_name = "NUE";
        d.backend = NUE_BACKEND_NULL;
        d.flags = NUE_INIT_HEADLESS | NUE_INIT_DETERMINISTIC | NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD;
        d.log_level = NUE_LOG_WARN;
        ok = NUE_SUCCEEDED(nue_init(&d));
    }
    ~Nue() {
        if (ok) nue_shutdown();
    }
};

uint64_t fnv(const std::vector<float>& v) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (float x : v) {
        const auto q = uint16_t(int16_t(std::lrintf(std::clamp(x, -1.0f, 1.0f) * 32767.0f)));
        h = (h ^ (q & 0xffu)) * 0x100000001b3ull;
        h = (h ^ (q >> 8)) * 0x100000001b3ull;
    }
    return h;
}

struct Session {
    std::vector<float> played;   // what the virtual device consumed, interleaved at the stream's channel count
    uint32_t channels = 0, rate = 0, reopens = 0;
    bool cursor_always_moved = true;
    uint64_t sink_frames = 0;
};

// A streaming client for one second of virtual time: 10 ms steps, a 0.5 s ring at 44.1 kHz, keeps about 100 ms ahead of the play cursor.
Session run_client(uint32_t steps) {
    Session s;
    EngineConfig config;
    config.latency_ms = 20;
    auto engine = Engine::create(config, make_nue_sink());
    CHECK(engine != nullptr);
    if (!engine) return s;
    auto* stream = static_cast<nue_audio_stream>(engine->sink().native_handle());
    CHECK(stream != nullptr);
    s.channels = engine->sink().actual().channels;
    s.rate = engine->sink().actual().sample_rate;
    CHECK(NUE_SUCCEEDED(nue_null_audio_set_capture(stream, s.rate * 4)));
    auto& d = engine->device();
    d.set_cooperative_level(2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto prim = d.create_buffer(pd);
    CHECK(d.set_format(prim.id, pcm16(44100, 2)) == dsx::hr::ok);
    dsx::BufferDesc sd;
    sd.flags = 0x18000;
    sd.bytes = 22016 * 4;
    sd.has_format = true;
    sd.format = pcm16(44100, 2);
    auto ring = d.create_buffer(sd);
    CHECK(ring.hr == dsx::hr::ok);
    CHECK(engine->start());
    d.play(ring.id, 0, dsx::play_flag::looping);
    const uint32_t ring_bytes = 22016 * 4;
    uint32_t written = 0, last_cursor = ~0u;
    for (uint32_t step = 0; step < steps; ++step) {
        nue_null_advance_time(10'000'000);   // 10 ms
        uint32_t cursor = 0;
        d.get_current_position(ring.id, &cursor, nullptr);
        if (step > 2 && cursor == last_cursor) s.cursor_always_moved = false;
        last_cursor = cursor;
        const uint32_t lead = (written + ring_bytes - cursor) % ring_bytes;
        const uint32_t want = 8820 * 4 / 2;
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
    s.sink_frames = engine->sink().frames_played();
    s.played.resize(size_t(s.rate) * 4 * s.channels);
    uint32_t got = 0;
    CHECK(NUE_SUCCEEDED(nue_null_audio_read_played(stream, s.played.data(), s.rate * 4, &got)));
    s.played.resize(size_t(got) * s.channels);
    s.reopens = engine->reopen_count();
    return s;
}

void test_native_stream() {
    Nue n;
    CHECK(n.ok);
    if (!n.ok) return;
    const Session a = run_client(100);
    CHECK(a.channels == 2 && a.rate >= 8000);
    CHECK(a.cursor_always_moved);
    CHECK(a.sink_frames > a.rate / 2);   // the device clock ran (about one second minus the latency)
    CHECK(!a.played.empty());
    // The tone is there: 523.25 Hz over the last half second of the capture, in both channels.
    if (a.played.size() > size_t(a.rate) * 2) {
        std::vector<float> tail(a.played.end() - std::ptrdiff_t(a.rate / 2) * 2, a.played.end());
        const double crossings = zero_crossings(tail, 2, 0);
        CHECK_NEAR(crossings, 523.25, 12.0);   // two zero crossings per period: 523.25 Hz x 0.5 s x 2
        CHECK_NEAR(rms(tail, 2, 0), 9000.0 / 32768.0 / std::sqrt(2.0), 0.01);
        CHECK_NEAR(rms(tail, 2, 1), rms(tail, 2, 0), 1e-6);
    }
    // Repeatable: the same call sequence on a fresh engine produces the same samples.
    const Session b = run_client(100);
    CHECK(fnv(a.played) == fnv(b.played) && !a.played.empty());
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    CHECK(fnv(a.played) == 0xd6f87479857334e9ull);   // the same samples as the manual sink of ds_device_tests: the native stream adds nothing
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    CHECK(fnv(a.played) == 0xd6f87479857334e9ull);   // the same samples as the manual sink of ds_device_tests: the native stream adds nothing
#endif
    std::printf("native stream session hash: 0x%016llx (%zu frames)\n", static_cast<unsigned long long>(fnv(a.played)), a.played.size() / 2);
}

void test_surround_primary_reopens_the_stream() {
    Nue n;
    CHECK(n.ok);
    if (!n.ok) return;
    EngineConfig config;
    auto engine = Engine::create(config, make_nue_sink());
    CHECK(engine != nullptr);
    if (!engine) return;
    auto& d = engine->device();
    d.set_cooperative_level(2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto prim = d.create_buffer(pd);
    WaveFormat six = pcm16(44100, 6);
    six.channel_mask = speaker::surround_5_1;
    const uint32_t before = engine->sink().actual().channels;
    CHECK(d.set_format(prim.id, six) == dsx::hr::ok);
    // The null device accepts any layout: the stream follows the primary format; a device with fewer channels would keep its own (NueSink).
    CHECK(engine->sink().actual().channels == 6 && engine->reopen_count() == 1 && before == 2);
    CHECK(std::string(d.spatial_name()) == "pan");
}

}  // namespace

int main() {
    test_native_stream();
    test_surround_primary_reopens_the_stream();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
