// Level A trace: record a client's session, replay it on a fresh Device, detect divergence, compare two clients by the PCM they write. Headless.
#include <cstdio>

#include "nue_legacy/dsound_trace.hpp"
#include "test_support.hpp"

using namespace nue::dsound;
using namespace nue::dsound::testing;
namespace dsx = nue::dsound;

namespace {

// A client: the primary format, a 0.5 s ring at 44.1 kHz, a second buffer with controls, notification and 3D, a failing call or two, `ms` milliseconds of
// polling at 10 ms. `chunk_ms` is how much it writes per refill (two clients with different chunking write the same tone).
void run_client(Device& d, uint32_t ms, uint32_t chunk_ms, uint32_t* written_frames_out = nullptr) {
    d.set_output({48000, 2, speaker::stereo}, 480);
    d.set_cooperative_level(2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto prim = d.create_buffer(pd);
    d.set_format(prim.id, pcm16(44100, 2));
    dsx::BufferDesc sd;
    sd.flags = 0x18000;
    sd.bytes = 22016 * 4;
    sd.has_format = true;
    sd.format = pcm16(44100, 2);
    auto ring = d.create_buffer(sd);
    dsx::BufferDesc fx;
    fx.flags = dsx::buffer_flag::ctrl_volume | dsx::buffer_flag::ctrl_pan | dsx::buffer_flag::ctrl_frequency | dsx::buffer_flag::ctrl_3d;
    fx.bytes = 4800 * 2;
    fx.has_format = true;
    fx.format = pcm16(48000, 1);
    auto sfx = d.create_buffer(fx);
    {   // the second buffer: a blip, panned, quieter, 3D placed, played once
        std::vector<int16_t> blip(4800);
        for (size_t i = 0; i < blip.size(); ++i) blip[i] = int16_t(std::lrint(6000.0 * std::sin(2.0 * 3.14159265358979 * 880.0 * double(i) / 48000.0)));
        const auto bytes = s16_bytes(blip);
        dsx::Regions r;
        d.lock(sfx.id, 0, uint32_t(bytes.size()), 0, r);
        d.unlock(sfx.id, r, bytes_of(bytes, 0, r.bytes1), bytes_of(bytes, r.bytes1, r.bytes2));
        d.set_volume(sfx.id, -600);
        d.set_pan(sfx.id, 3000);
        d.set_frequency(sfx.id, 0);
        dsx::Buffer3D b3;
        b3.position = {2, 0, 3};
        d.set_buffer_3d(sfx.id, b3);
        dsx::Regions bad;
        d.lock(sfx.id, 99999, 4, 0, bad);   // a call that fails: its answer is part of the record
        d.set_volume(ring.id, -100);        // CONTROLUNAVAIL on the ring
    }
    d.play(ring.id, 0, dsx::play_flag::looping);
    d.play(sfx.id, 0, 0);
    const uint32_t ring_bytes = 22016 * 4;
    uint32_t written = 0;
    uint64_t total_frames = 0;
    std::vector<float> out(480 * 2);
    for (uint32_t t = 0; t < ms; t += 10) {
        d.render(out.data(), 480);
        uint32_t cursor = 0, write_cursor = 0, status = 0;
        d.get_current_position(ring.id, &cursor, &write_cursor);
        d.get_status(ring.id, status);
        const uint32_t lead = (written + ring_bytes - cursor) % ring_bytes;
        const uint32_t want = 8820 * 4 / 2;
        if ((t % chunk_ms) == 0 && lead < want) {
            const uint32_t bytes = std::min<uint32_t>(want - lead, uint32_t(44.1 * chunk_ms) * 4);
            std::vector<uint8_t> tone(bytes);
            for (uint32_t i = 0; i + 3 < bytes; i += 4) {
                const double ts = double(total_frames + i / 4) / 44100.0;
                const int16_t v = int16_t(std::lrint(9000.0 * std::sin(2.0 * 3.14159265358979 * 523.25 * ts)));
                std::memcpy(&tone[i], &v, 2);
                std::memcpy(&tone[i + 2], &v, 2);
            }
            dsx::Regions r;
            if (d.lock(ring.id, written % ring_bytes, bytes, 0, r) == dsx::hr::ok) d.unlock(ring.id, r, bytes_of(tone, 0, r.bytes1), bytes_of(tone, r.bytes1, r.bytes2));
            written = (written + bytes) % ring_bytes;
            total_frames += bytes / 4;
        }
    }
    d.stop(ring.id);
    d.destroy_buffer(sfx.id);
    if (written_frames_out) *written_frames_out = uint32_t(total_frames);
}

void test_record_and_replay() {
    TraceRecorder rec;
    auto recorded = make_recording_device(make_device(), rec);
    run_client(*recorded, 1000, 10);
    const std::vector<uint8_t> data = rec.finish();
    const TraceInfo info = trace_info(data.data(), data.size());
    CHECK(info.valid && info.version == trace_version);
    CHECK(info.records > 300 && info.renders == 100 && info.rendered_frames == 48000 && info.unlocks > 10);
    CHECK(info.pcm_bytes > 22016 * 4 / 2 && info.pcm_hash == rec.pcm_hash() && info.records == rec.records());
    // The same calls on a fresh device: every answer and every mixed block equals the record.
    auto fresh = make_device();
    const ReplayResult r = replay_trace(data.data(), data.size(), *fresh);
    if (!r.ok) std::printf("replay error: %s\n", r.error.c_str());
    CHECK(r.ok && r.records == info.records && r.pcm_hash == info.pcm_hash && r.mixed_hash == info.mixed_hash);
    // Through a file.
    const std::string path = "nue_dsound_trace_test.ndst";
    CHECK(rec.save(path));
    std::vector<uint8_t> loaded;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) loaded.insert(loaded.end(), buf, buf + n);
        std::fclose(f);
        std::remove(path.c_str());
    }
    CHECK(loaded == data);
    CHECK(info.pcm_hash == 0x089f203438490ba9ull);   // the bytes the client wrote: the same on every platform
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    CHECK(info.mixed_hash == 0x6b578d46977ef039ull);   // pinned: identical in the 32-bit and the 64-bit build
#endif
    std::printf("trace: %llu records, %zu bytes, pcm hash 0x%016llx, mixed hash 0x%016llx\n", static_cast<unsigned long long>(info.records), data.size(),
                static_cast<unsigned long long>(info.pcm_hash), static_cast<unsigned long long>(info.mixed_hash));
}

void test_divergence_is_found() {
    TraceRecorder rec;
    auto recorded = make_recording_device(make_device(), rec);
    run_client(*recorded, 300, 10);
    const std::vector<uint8_t> data = rec.finish();
    // A device configured differently (a smaller write-cursor margin) gives other write cursors: the replay names the record.
    dsx::Config other;
    other.write_margin_ms = 10;
    auto different = make_device(other);
    const ReplayResult r = replay_trace(data.data(), data.size(), *different);
    CHECK(!r.ok && r.error.find("record ") == 0 && r.error.find("get_current_position") != std::string::npos);
    std::printf("divergence: %s\n", r.error.c_str());
    // Another spatial mode places the 3D blip differently: the mixed blocks differ and the replay names the render.
    auto b = make_device();
    dsx::SpatialOptions pan;
    pan.mode = dsx::SpatialMode::pan;
    b->set_spatial(pan);
    const ReplayResult q = replay_trace(data.data(), data.size(), *b);
    CHECK(!q.ok && q.error.find("render") != std::string::npos);
    std::printf("divergence: %s\n", q.error.c_str());
    // Damaged traces are refused with a reason, never replayed.
    std::vector<uint8_t> bad = data;
    bad[0] = 'X';
    auto d1 = make_device();
    CHECK(!replay_trace(bad.data(), bad.size(), *d1).ok && !trace_info(bad.data(), bad.size()).valid);
    bad = data;
    bad[4] = 9;   // version
    CHECK(trace_info(bad.data(), bad.size()).error.find("version") != std::string::npos);
    bad = data;
    bad.resize(bad.size() / 2);
    auto d2 = make_device();
    CHECK(!replay_trace(bad.data(), bad.size(), *d2).ok);
    bad = data;
    bad[bad.size() / 2] ^= 0x5a;   // a flipped byte in the middle
    auto d3 = make_device();
    const ReplayResult flipped = replay_trace(bad.data(), bad.size(), *d3);
    CHECK(!flipped.ok || flipped.pcm_hash != 0);   // either a record diverges or the totals do; it never silently equals
    CHECK(!flipped.ok);
}

void test_compare_two_clients_by_pcm() {
    // Two clients that write the same tone with different call patterns (10 ms and 30 ms refills) differ as traces but write the same PCM, in order.
    // The ring is written ahead of the play cursor, so compare only the first N frames both wrote: the hash of the bytes up to a common length.
    auto pcm_prefix_hash = [](uint32_t chunk_ms, uint32_t frames_wanted) {
        TraceRecorder rec;
        auto recorded = make_recording_device(make_device(), rec);
        run_client(*recorded, 2000, chunk_ms);
        const std::vector<uint8_t> data = rec.finish();
        // Walk the records and hash the first `frames_wanted` frames of the ring's data (the ring is buffer 2 in this client; the blip buffer is 3).
        uint64_t h = 0xcbf29ce484222325ull;
        uint64_t left = uint64_t(frames_wanted) * 4;
        size_t pos = 8;
        while (pos + 5 <= data.size() && left) {
            const uint8_t op = data[pos];
            uint32_t len;
            std::memcpy(&len, &data[pos + 1], 4);
            const uint8_t* payload = &data[pos + 5];
            pos += 5 + len;
            if (op != 8) continue;   // unlock
            uint32_t id, n1;
            std::memcpy(&id, payload, 4);
            std::memcpy(&n1, payload + 4 + 16 + 4, 4);
            if ((id & 0xfff) != 2) continue;   // the ring (slot 1 is the primary, slot 2 the ring)
            const uint8_t* d1 = payload + 4 + 16 + 4 + 4;
            uint32_t n2;
            std::memcpy(&n2, d1 + n1, 4);
            const uint8_t* d2 = d1 + n1 + 4;
            for (uint32_t i = 0; i < n1 && left; ++i, --left) h = (h ^ d1[i]) * 0x100000001b3ull;
            for (uint32_t i = 0; i < n2 && left; ++i, --left) h = (h ^ d2[i]) * 0x100000001b3ull;
        }
        return std::pair<uint64_t, uint64_t>(h, left);
    };
    const auto a = pcm_prefix_hash(10, 44100);   // the first second of the stream
    const auto b = pcm_prefix_hash(30, 44100);
    CHECK(a.second == 0 && b.second == 0);
    CHECK(a.first == b.first);   // the same PCM stream, whatever the call pattern
    const auto c = pcm_prefix_hash(10, 44100 / 2);
    CHECK(c.first != a.first);   // and the comparison is not vacuous
}

void test_through_the_engine() {
    TraceRecorder rec;
    EngineConfig config;
    config.sample_rate = 48000;
    config.trace = &rec;
    auto sink = std::make_unique<ManualSink>(480, 480);
    ManualSink* raw = sink.get();
    auto engine = Engine::create(config, std::move(sink));
    CHECK(engine != nullptr);
    if (!engine) return;
    auto& d = engine->device();
    d.set_cooperative_level(2);
    dsx::BufferDesc pd;
    pd.flags = dsx::buffer_flag::primary;
    auto prim = d.create_buffer(pd);
    d.set_format(prim.id, pcm16(44100, 2));
    dsx::BufferDesc sd;
    sd.flags = 0x18000;
    sd.bytes = 22016 * 4;
    sd.has_format = true;
    sd.format = pcm16(44100, 2);
    auto ring = d.create_buffer(sd);
    engine->start();
    d.play(ring.id, 0, dsx::play_flag::looping);
    std::vector<uint8_t> tone(2205 * 4, 0x20);
    for (int i = 0; i < 50; ++i) {
        raw->advance(480);
        uint32_t cursor = 0;
        d.get_current_position(ring.id, &cursor, nullptr);
        dsx::Regions r;
        if (d.lock(ring.id, (uint32_t(i) * 2205 * 4) % sd.bytes, 2205 * 4, 0, r) == dsx::hr::ok) d.unlock(ring.id, r, bytes_of(tone, 0, r.bytes1), bytes_of(tone, r.bytes1, r.bytes2));
    }
    const std::vector<uint8_t> data = rec.finish();
    const TraceInfo info = trace_info(data.data(), data.size());
    CHECK(info.valid && info.renders > 0 && info.rendered_frames == 50u * 480u);
    auto fresh = make_device(config.device);
    const ReplayResult r = replay_trace(data.data(), data.size(), *fresh);
    if (!r.ok) std::printf("replay error: %s\n", r.error.c_str());
    CHECK(r.ok && r.mixed_hash == info.mixed_hash);
}

}  // namespace

int main() {
    test_record_and_replay();
    test_divergence_is_found();
    test_compare_two_clients_by_pcm();
    test_through_the_engine();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
