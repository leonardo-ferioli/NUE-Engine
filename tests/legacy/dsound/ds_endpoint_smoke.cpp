// Smoke test on the REAL default output endpoint (through SDL3 and the platform's audio API, via the native stream): the engine opens, the device clock
// advances, the DirectSound play cursor of a looping ring moves at the real-time rate, nothing underruns in half a second, and a 6-channel primary on
// a stereo endpoint is accepted. It plays only zero-filled buffers (silence), never touches the system volume, and skips itself (exit 0) when there is
// no output device. A tool, not part of ctest (it needs hardware and real time). Prints what it saw so the numbers can be read.
#include <chrono>
#include <thread>

#include "nue/nue.h"
#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

int main() {
    nue_init_desc d;
    NUE_STRUCT_INIT(&d);
    d.api_version = NUE_API_VERSION;
    d.app_name = "dsound endpoint smoke";
    d.app_id = "dsound-smoke";
    d.org_name = "NUE";
    d.backend = NUE_BACKEND_AUTO;
    d.subsystems = NUE_SUBSYSTEM_EVENTS | NUE_SUBSYSTEM_AUDIO;
    d.flags = NUE_INIT_NO_LOG_FILE | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_CONFIG_LOAD;
    d.log_level = NUE_LOG_WARN;
    if (NUE_FAILED(nue_init(&d))) {
        std::printf("SKIP: nue_init failed (%s)\n", nue_get_last_error_message());
        return 0;
    }
    int rc = 0;
    {
        nue_audio_device_info info;
        NUE_STRUCT_INIT(&info);
        if (NUE_FAILED(nue_audio_get_device_info(0, &info))) {
            std::printf("SKIP: no default output device\n");
            nue_shutdown();
            return 0;
        }
        std::printf("default output: \"%s\", %u Hz, %u channels\n", info.name, info.preferred.sample_rate, info.preferred.channels);
        EngineConfig config;
        auto engine = Engine::create(config, make_nue_sink());
        if (!engine) {
            std::printf("SKIP: could not open the default output (%s)\n", nue_get_last_error_message());
            nue_shutdown();
            return 0;
        }
        auto& dev = engine->device();
        const SinkLatency lat = engine->sink().latency();
        std::printf("stream: %u Hz, %u channels, device buffer %u frames, device latency %.1f ms\n", engine->sink().actual().sample_rate, engine->sink().actual().channels,
                    lat.device_buffer_frames, double(lat.device_latency_ns) / 1e6);
        dev.set_cooperative_level(2);
        dsx::BufferDesc pd;
        pd.flags = dsx::buffer_flag::primary;
        auto prim = dev.create_buffer(pd);
        CHECK(dev.set_format(prim.id, pcm16(44100, 2)) == dsx::hr::ok);
        dsx::BufferDesc sd;
        sd.flags = 0x18000;
        sd.bytes = 22048 * 4;
        sd.has_format = true;
        sd.format = pcm16(44100, 2);
        auto ring = dev.create_buffer(sd);   // zero-filled: silence
        CHECK(ring.hr == dsx::hr::ok);
        CHECK(engine->start());
        dev.play(ring.id, 0, dsx::play_flag::looping);
        uint32_t last = 0;
        double moved_bytes = 0;   // unwrapped: the ring is 0.5 s long and the test runs longer than that
        dev.get_current_position(ring.id, &last, nullptr);
        const auto t0 = std::chrono::steady_clock::now();
        int frozen = 0, max_frozen = 0;
        for (int i = 0; i < 50; ++i) {   // 50 polls of 10 ms, as a streaming client does
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            uint32_t cur = 0;
            dev.get_current_position(ring.id, &cur, nullptr);
            moved_bytes += double((cur + sd.bytes - last) % sd.bytes);
            frozen = (cur == last) ? frozen + 1 : 0;
            max_frozen = std::max(max_frozen, frozen);
            last = cur;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double moved = moved_bytes / 4.0 / 44100.0;
        std::printf("play cursor moved %.3f s of audio in %.3f s of wall time; longest run of identical polls: %d; underruns: %llu\n", moved, elapsed, max_frozen,
                    static_cast<unsigned long long>(engine->sink().stats().underruns));
        CHECK(std::fabs(moved - elapsed) < 0.15);   // the cursor runs at the real-time rate (the client derives how much to produce from it)
        CHECK(max_frozen < 10);                     // never stands still (a client rebuilds its audio after 100)
        CHECK(engine->sink().stats().underruns == 0);
        // A 6-channel primary: the stream follows when the device has the channels, and keeps the device's layout otherwise.
        WaveFormat six = pcm16(44100, 6);
        six.channel_mask = speaker::surround_5_1;
        CHECK(dev.set_format(prim.id, six) == dsx::hr::ok);
        nue_audio_spec app{}, dv{};
        nue_audio_stream_get_spec(static_cast<nue_audio_stream>(engine->sink().native_handle()), &app, &dv);
        std::printf("stream spec: application %u ch %u Hz, device %u ch %u Hz\n", app.channels, app.sample_rate, dv.channels, dv.sample_rate);
        std::printf("after a 6-channel primary: stream has %u channels (reopened %u time(s)), spatial: %s\n", engine->sink().actual().channels, engine->reopen_count(),
                    dev.spatial_name());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(engine->sink().running());
        rc = g_failures ? 1 : 0;
        std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    }
    nue_shutdown();
    return rc;
}
