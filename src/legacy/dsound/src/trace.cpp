// Level A trace: recorder (a Device decorator) and replayer. See include/nue_legacy/dsound_trace.hpp.
#include "nue_legacy/dsound_trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace nue::dsound {
namespace {

constexpr uint64_t fnv_basis = 0xcbf29ce484222325ull, fnv_prime = 0x100000001b3ull;

enum Op : uint8_t {
    op_coop = 1, op_reset, op_create, op_duplicate, op_destroy, op_set_format, op_lock, op_unlock, op_play, op_stop, op_get_position, op_get_status, op_set_position,
    op_restore, op_set_volume, op_set_pan, op_set_frequency, op_set_notifications, op_set_listener, op_set_buffer_3d, op_set_environment, op_set_send, op_set_output,
    op_render, op_end = 255
};

const char* op_name(uint8_t op) {
    static const char* names[] = {"?", "set_cooperative_level", "reset", "create_buffer", "duplicate_buffer", "destroy_buffer", "set_format", "lock", "unlock", "play", "stop",
                                  "get_current_position", "get_status", "set_current_position", "restore", "set_volume", "set_pan", "set_frequency", "set_notifications",
                                  "set_listener", "set_buffer_3d", "set_environment", "set_buffer_send", "set_output", "render"};
    return op < sizeof(names) / sizeof(names[0]) ? names[op] : op == op_end ? "end" : "?";
}

// ---- serialisation --------------------------------------------------------------------------------------------------------------------------
struct W {
    std::vector<uint8_t> b;
    template <class T> void raw(const T& v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        b.insert(b.end(), p, p + sizeof(T));
    }
    void u8(uint8_t v) { raw(v); }
    void u16(uint16_t v) { raw(v); }
    void u32(uint32_t v) { raw(v); }
    void i32(int32_t v) { raw(v); }
    void u64(uint64_t v) { raw(v); }
    void f32(float v) { raw(v); }
    void vec(const Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void format(const WaveFormat& f) { u8(uint8_t(f.type)); u16(f.channels); u32(f.sample_rate); u16(f.bits); u32(f.channel_mask); }
    void regions(const Regions& r) { u32(r.offset1); u32(r.bytes1); u32(r.offset2); u32(r.bytes2); }
    void listener(const Listener3D& l) { vec(l.position); vec(l.velocity); vec(l.front); vec(l.top); f32(l.distance_factor); f32(l.rolloff_factor); f32(l.doppler_factor); }
    void buffer3d(const Buffer3D& p) {
        vec(p.position); vec(p.velocity); u32(p.inside_cone_angle); u32(p.outside_cone_angle); vec(p.cone_orientation); i32(p.cone_outside_volume);
        f32(p.min_distance); f32(p.max_distance); u32(uint32_t(p.mode));
    }
    void send(const EnvironmentSend& s) { f32(s.direct_gain); f32(s.direct_gain_hf); f32(s.send_gain); f32(s.send_gain_hf); }
    void environment(const Environment& e) {
        u8(e.enabled ? 1 : 0);
        u32(uint32_t(sizeof(nue_reverb_params)));
        bytes(reinterpret_cast<const uint8_t*>(&e.params), sizeof(nue_reverb_params));
    }
};

struct R {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    R(const uint8_t* a, size_t n) : p(a), end(a + n) {}
    template <class T> T raw() {
        T v{};
        if (size_t(end - p) < sizeof(T)) { ok = false; return v; }
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    uint8_t u8() { return raw<uint8_t>(); }
    uint16_t u16() { return raw<uint16_t>(); }
    uint32_t u32() { return raw<uint32_t>(); }
    int32_t i32() { return raw<int32_t>(); }
    uint64_t u64() { return raw<uint64_t>(); }
    float f32() { return raw<float>(); }
    Vec3 vec() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    const uint8_t* bytes(size_t n) {
        if (size_t(end - p) < n) { ok = false; return nullptr; }
        const uint8_t* r = p;
        p += n;
        return r;
    }
    WaveFormat format() {
        WaveFormat f;
        f.type = SampleType(u8()); f.channels = u16(); f.sample_rate = u32(); f.bits = u16(); f.channel_mask = u32();
        return f;
    }
    Regions regions() { Regions r; r.offset1 = u32(); r.bytes1 = u32(); r.offset2 = u32(); r.bytes2 = u32(); return r; }
    Listener3D listener() {
        Listener3D l; l.position = vec(); l.velocity = vec(); l.front = vec(); l.top = vec();
        l.distance_factor = f32(); l.rolloff_factor = f32(); l.doppler_factor = f32();
        return l;
    }
    Buffer3D buffer3d() {
        Buffer3D p3; p3.position = vec(); p3.velocity = vec(); p3.inside_cone_angle = u32(); p3.outside_cone_angle = u32(); p3.cone_orientation = vec();
        p3.cone_outside_volume = i32(); p3.min_distance = f32(); p3.max_distance = f32(); p3.mode = Mode3D(u32());
        return p3;
    }
    EnvironmentSend send() { EnvironmentSend s; s.direct_gain = f32(); s.direct_gain_hf = f32(); s.send_gain = f32(); s.send_gain_hf = f32(); return s; }
    Environment environment() {
        Environment e;
        e.enabled = u8() != 0;
        const uint32_t n = u32();
        const uint8_t* src = bytes(n);
        if (src && n == sizeof(nue_reverb_params)) std::memcpy(&e.params, src, n);
        else ok = false;
        return e;
    }
};

uint64_t hash_samples(const float* x, size_t count, uint64_t h) {
    for (size_t i = 0; i < count; ++i) {
        const float c = std::min(1.0f, std::max(-1.0f, x[i]));
        const auto q = uint16_t(int16_t(std::lrintf(c * 32767.0f)));
        h = (h ^ (q & 0xffu)) * fnv_prime;
        h = (h ^ (q >> 8)) * fnv_prime;
    }
    return h;
}

// ---- the recording device ------------------------------------------------------------------------------------------------------------------------
class RecordingDevice final : public Device {
public:
    RecordingDevice(std::unique_ptr<Device> inner, TraceRecorder& rec) : d_(std::move(inner)), rec_(rec) {}

    Hr set_cooperative_level(uint32_t level) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_cooperative_level(level);
        W w; w.u32(level); w.i32(h);
        rec_.append(op_coop, w.b);
        return h;
    }
    void reset() override {
        std::lock_guard lock(rec_.mutex());
        d_->reset();
        rec_.append(op_reset, {});
    }
    Caps caps() const override { return d_->caps(); }
    CreateResult create_buffer(const BufferDesc& desc) override {
        std::lock_guard lock(rec_.mutex());
        const CreateResult r = d_->create_buffer(desc);
        W w; w.u32(desc.flags); w.u32(desc.bytes); w.u8(desc.has_format ? 1 : 0); w.format(desc.format); w.i32(r.hr); w.u32(r.id.value); w.u8(r.existing_primary ? 1 : 0);
        rec_.append(op_create, w.b);
        return r;
    }
    CreateResult duplicate_buffer(BufferId source) override {
        std::lock_guard lock(rec_.mutex());
        const CreateResult r = d_->duplicate_buffer(source);
        W w; w.u32(source.value); w.i32(r.hr); w.u32(r.id.value);
        rec_.append(op_duplicate, w.b);
        return r;
    }
    void destroy_buffer(BufferId id) override {
        std::lock_guard lock(rec_.mutex());
        d_->destroy_buffer(id);
        W w; w.u32(id.value);
        rec_.append(op_destroy, w.b);
    }
    Hr set_format(BufferId id, const WaveFormat& f) override {
        const Hr h = d_->set_format(id, f);   // may reopen the output (joins the mix thread): not under the recorder lock
        std::lock_guard lock(rec_.mutex());
        W w; w.u32(id.value); w.format(f); w.i32(h);
        rec_.append(op_set_format, w.b);
        return h;
    }
    Hr get_format(BufferId id, WaveFormat& out) const override { return d_->get_format(id, out); }
    Hr get_caps(BufferId id, BufferCaps& out) const override { return d_->get_caps(id, out); }
    Hr lock(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags, Regions& out) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->lock(id, offset, bytes, flags, out);
        W w; w.u32(id.value); w.u32(offset); w.u32(bytes); w.u32(flags); w.i32(h); w.regions(h >= 0 ? out : Regions{});
        rec_.append(op_lock, w.b);
        return h;
    }
    Hr unlock(BufferId id, const Regions& regions, Bytes part1, Bytes part2) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->unlock(id, regions, part1, part2);
        W w; w.u32(id.value); w.regions(regions); w.i32(h);
        w.u32(uint32_t(part1.size())); w.bytes(reinterpret_cast<const uint8_t*>(part1.data()), part1.size());
        w.u32(uint32_t(part2.size())); w.bytes(reinterpret_cast<const uint8_t*>(part2.data()), part2.size());
        rec_.append(op_unlock, w.b);
        if (h >= 0) {
            rec_.note_pcm(reinterpret_cast<const uint8_t*>(part1.data()), regions.bytes1 <= part1.size() ? regions.bytes1 : part1.size());
            rec_.note_pcm(reinterpret_cast<const uint8_t*>(part2.data()), regions.bytes2 <= part2.size() ? regions.bytes2 : part2.size());
        }
        return h;
    }
    Hr play(BufferId id, uint32_t priority, uint32_t flags) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->play(id, priority, flags);
        W w; w.u32(id.value); w.u32(priority); w.u32(flags); w.i32(h);
        rec_.append(op_play, w.b);
        return h;
    }
    Hr stop(BufferId id) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->stop(id);
        W w; w.u32(id.value); w.i32(h);
        rec_.append(op_stop, w.b);
        return h;
    }
    Hr get_status(BufferId id, uint32_t& out) const override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->get_status(id, out);
        W w; w.u32(id.value); w.i32(h); w.u32(h >= 0 ? out : 0);
        rec_.append(op_get_status, w.b);
        return h;
    }
    Hr get_current_position(BufferId id, uint32_t* play_cursor, uint32_t* write_cursor) const override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->get_current_position(id, play_cursor, write_cursor);
        W w; w.u32(id.value); w.u8(play_cursor ? 1 : 0); w.u8(write_cursor ? 1 : 0); w.i32(h);
        w.u32(h >= 0 && play_cursor ? *play_cursor : 0); w.u32(h >= 0 && write_cursor ? *write_cursor : 0);
        rec_.append(op_get_position, w.b);
        return h;
    }
    Hr set_current_position(BufferId id, uint32_t byte_position) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_current_position(id, byte_position);
        W w; w.u32(id.value); w.u32(byte_position); w.i32(h);
        rec_.append(op_set_position, w.b);
        return h;
    }
    Hr restore(BufferId id) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->restore(id);
        W w; w.u32(id.value); w.i32(h);
        rec_.append(op_restore, w.b);
        return h;
    }
    Hr set_volume(BufferId id, int32_t mb) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_volume(id, mb);
        W w; w.u32(id.value); w.i32(mb); w.i32(h);
        rec_.append(op_set_volume, w.b);
        return h;
    }
    Hr get_volume(BufferId id, int32_t& out) const override { return d_->get_volume(id, out); }
    Hr set_pan(BufferId id, int32_t mb) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_pan(id, mb);
        W w; w.u32(id.value); w.i32(mb); w.i32(h);
        rec_.append(op_set_pan, w.b);
        return h;
    }
    Hr get_pan(BufferId id, int32_t& out) const override { return d_->get_pan(id, out); }
    Hr set_frequency(BufferId id, uint32_t hz) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_frequency(id, hz);
        W w; w.u32(id.value); w.u32(hz); w.i32(h);
        rec_.append(op_set_frequency, w.b);
        return h;
    }
    Hr get_frequency(BufferId id, uint32_t& out) const override { return d_->get_frequency(id, out); }
    Hr set_notifications(BufferId id, std::span<const Notify> positions) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_notifications(id, positions);
        W w; w.u32(id.value); w.u32(uint32_t(positions.size()));
        for (const Notify& n : positions) { w.u32(n.offset); w.u64(n.token); }
        w.i32(h);
        rec_.append(op_set_notifications, w.b);
        return h;
    }
    Hr set_listener(const Listener3D& l) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_listener(l);
        W w; w.listener(l); w.i32(h);
        rec_.append(op_set_listener, w.b);
        return h;
    }
    Listener3D listener() const override { return d_->listener(); }
    Hr set_buffer_3d(BufferId id, const Buffer3D& p) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_buffer_3d(id, p);
        W w; w.u32(id.value); w.buffer3d(p); w.i32(h);
        rec_.append(op_set_buffer_3d, w.b);
        return h;
    }
    Hr get_buffer_3d(BufferId id, Buffer3D& out) const override { return d_->get_buffer_3d(id, out); }
    void set_environment(const Environment& e) override {
        std::lock_guard lock(rec_.mutex());
        d_->set_environment(e);
        W w; w.environment(e);
        rec_.append(op_set_environment, w.b);
    }
    Hr set_buffer_send(BufferId id, const EnvironmentSend& s) override {
        std::lock_guard lock(rec_.mutex());
        const Hr h = d_->set_buffer_send(id, s);
        W w; w.u32(id.value); w.send(s); w.i32(h);
        rec_.append(op_set_send, w.b);
        return h;
    }
    void set_spatial(const SpatialOptions& o) override { d_->set_spatial(o); }   // the player's choice, not the client's: not part of the trace
    void set_hrtf(std::span<const float> az, std::span<const float> el, std::span<const float> irs, uint32_t n, uint32_t rate) override { d_->set_hrtf(az, el, irs, n, rate); }
    const char* spatial_name() const override { return d_->spatial_name(); }
    void set_output(const OutputLayout& layout, uint32_t latency_frames) override {
        std::lock_guard lock(rec_.mutex());
        d_->set_output(layout, latency_frames);
        W w; w.u32(layout.sample_rate); w.u32(layout.channels); w.u32(layout.channel_mask); w.u32(latency_frames);
        rec_.append(op_set_output, w.b);
    }
    OutputLayout output() const override { return d_->output(); }
    void set_notify_callback(NotifyFn fn, void* user) override { d_->set_notify_callback(fn, user); }
    void set_format_callback(FormatChangedFn fn, void* user) override { d_->set_format_callback(fn, user); }
    void render(float* out, uint32_t frames) override {
        std::lock_guard lock(rec_.mutex());
        d_->render(out, frames);
        const uint32_t channels = d_->output().channels;
        W w; w.u32(frames); w.u64(hash_samples(out, size_t(frames) * channels, fnv_basis));
        rec_.append(op_render, w.b);
        rec_.note_render(frames, out, channels);
    }
    DeviceStats stats() const override { return d_->stats(); }
    void lose_buffers() override { d_->lose_buffers(); }

private:
    std::unique_ptr<Device> d_;
    TraceRecorder& rec_;
};

}  // namespace

// ---- recorder ---------------------------------------------------------------------------------------------------------------------------------
TraceRecorder::TraceRecorder() = default;

void TraceRecorder::append(uint8_t opcode, const std::vector<uint8_t>& payload) {   // the caller holds mutex()
    body_.push_back(opcode);
    const uint32_t n = uint32_t(payload.size());
    const auto* p = reinterpret_cast<const uint8_t*>(&n);
    body_.insert(body_.end(), p, p + 4);
    body_.insert(body_.end(), payload.begin(), payload.end());
    ++records_;
    if (opcode == op_unlock) ++unlocks_;
}
void TraceRecorder::note_pcm(const uint8_t* data, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) pcm_hash_ = (pcm_hash_ ^ data[i]) * fnv_prime;
    pcm_bytes_ += bytes;
}
void TraceRecorder::note_render(uint32_t frames, const float* samples, uint32_t channels) {
    mixed_hash_ = hash_samples(samples, size_t(frames) * channels, mixed_hash_);
    ++renders_;
    frames_ += frames;
}
uint64_t TraceRecorder::records() const {
    std::lock_guard lock(mutex_);
    return records_;
}
uint64_t TraceRecorder::pcm_hash() const {
    std::lock_guard lock(mutex_);
    return pcm_hash_;
}
std::vector<uint8_t> TraceRecorder::finish() const {
    std::lock_guard lock(mutex_);
    std::vector<uint8_t> out = {'N', 'D', 'S', 'T'};
    W header;
    header.u32(trace_version);
    out.insert(out.end(), header.b.begin(), header.b.end());
    out.insert(out.end(), body_.begin(), body_.end());
    W end;
    end.u64(records_); end.u64(renders_); end.u64(frames_); end.u64(unlocks_); end.u64(pcm_bytes_); end.u64(pcm_hash_); end.u64(mixed_hash_);
    out.push_back(op_end);
    const uint32_t n = uint32_t(end.b.size());
    const auto* p = reinterpret_cast<const uint8_t*>(&n);
    out.insert(out.end(), p, p + 4);
    out.insert(out.end(), end.b.begin(), end.b.end());
    return out;
}
bool TraceRecorder::save(const std::string& path) const {
    const std::vector<uint8_t> data = finish();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

std::unique_ptr<Device> make_recording_device(std::unique_ptr<Device> inner, TraceRecorder& recorder) {
    return std::make_unique<RecordingDevice>(std::move(inner), recorder);
}

// ---- reading ------------------------------------------------------------------------------------------------------------------------------------
TraceInfo trace_info(const uint8_t* data, size_t bytes) {
    TraceInfo info;
    if (bytes < 8 || std::memcmp(data, "NDST", 4) != 0) { info.error = "not a DirectSound trace (bad magic)"; return info; }
    R r(data + 4, bytes - 4);
    info.version = r.u32();
    if (info.version != trace_version) { info.error = "unsupported trace version " + std::to_string(info.version); return info; }
    while (r.ok && r.p < r.end) {
        const uint8_t op = r.u8();
        const uint32_t n = r.u32();
        const uint8_t* payload = r.bytes(n);
        if (!r.ok) break;
        if (op == op_end) {
            R t(payload, n);
            info.records = t.u64(); info.renders = t.u64(); info.rendered_frames = t.u64(); info.unlocks = t.u64(); info.pcm_bytes = t.u64(); info.pcm_hash = t.u64(); info.mixed_hash = t.u64();
            info.valid = t.ok;
            if (!t.ok) info.error = "short totals record";
            return info;
        }
    }
    info.error = "truncated trace (no totals record)";
    return info;
}

ReplayResult replay_trace(const uint8_t* data, size_t bytes, Device& dev) {
    ReplayResult res;
    const TraceInfo info = trace_info(data, bytes);
    if (!info.valid) { res.error = info.error; return res; }
    R r(data + 8, bytes - 8);
    uint64_t pcm = fnv_basis, mixed = fnv_basis;
    std::vector<float> scratch;
    auto fail = [&](uint64_t n, uint8_t op, const std::string& what) {
        std::ostringstream s;
        s << "record " << n << " (" << op_name(op) << "): " << what;
        res.error = s.str();
        res.records = n;
        res.pcm_hash = pcm;
        res.mixed_hash = mixed;
    };
    auto hex = [](uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "0x%08x", v); return std::string(b); };
    uint64_t n = 0;
    while (r.ok && r.p < r.end) {
        const uint8_t op = r.u8();
        const uint32_t len = r.u32();
        const uint8_t* payload = r.bytes(len);
        if (!r.ok) break;
        if (op == op_end) break;
        ++n;
        R p(payload, len);
        auto check_hr = [&](Hr got, Hr recorded) -> bool {
            if (got == recorded) return true;
            fail(n, op, "answer " + hex(uint32_t(got)) + ", recorded " + hex(uint32_t(recorded)));
            return false;
        };
        switch (op) {
            case op_coop: { const uint32_t level = p.u32(); const Hr rec = p.i32(); if (!check_hr(dev.set_cooperative_level(level), rec)) return res; break; }
            case op_reset: dev.reset(); break;
            case op_create: {
                BufferDesc d; d.flags = p.u32(); d.bytes = p.u32(); d.has_format = p.u8() != 0; d.format = p.format();
                const Hr rec = p.i32(); const uint32_t id = p.u32(); const bool existing = p.u8() != 0;
                const CreateResult c = dev.create_buffer(d);
                if (!check_hr(c.hr, rec)) return res;
                if (c.hr >= 0 && (c.id.value != id || c.existing_primary != existing)) { fail(n, op, "buffer id " + hex(c.id.value) + ", recorded " + hex(id)); return res; }
                break;
            }
            case op_duplicate: {
                const uint32_t src = p.u32(); const Hr rec = p.i32(); const uint32_t id = p.u32();
                const CreateResult c = dev.duplicate_buffer(BufferId{src});
                if (!check_hr(c.hr, rec)) return res;
                if (c.hr >= 0 && c.id.value != id) { fail(n, op, "buffer id " + hex(c.id.value) + ", recorded " + hex(id)); return res; }
                break;
            }
            case op_destroy: dev.destroy_buffer(BufferId{p.u32()}); break;
            case op_set_format: { const uint32_t id = p.u32(); const WaveFormat f = p.format(); const Hr rec = p.i32(); if (!check_hr(dev.set_format(BufferId{id}, f), rec)) return res; break; }
            case op_lock: {
                const uint32_t id = p.u32(), offset = p.u32(), len2 = p.u32(), flags = p.u32(); const Hr rec = p.i32(); const Regions want = p.regions();
                Regions got;
                const Hr h = dev.lock(BufferId{id}, offset, len2, flags, got);
                if (!check_hr(h, rec)) return res;
                if (h >= 0 && (got.offset1 != want.offset1 || got.bytes1 != want.bytes1 || got.offset2 != want.offset2 || got.bytes2 != want.bytes2)) {
                    fail(n, op, "regions differ from the record"); return res;
                }
                break;
            }
            case op_unlock: {
                const uint32_t id = p.u32(); const Regions rg = p.regions(); const Hr rec = p.i32();
                const uint32_t n1 = p.u32(); const uint8_t* d1 = p.bytes(n1); const uint32_t n2 = p.u32(); const uint8_t* d2 = p.bytes(n2);
                if (!p.ok) { fail(n, op, "short record"); return res; }
                const Hr h = dev.unlock(BufferId{id}, rg, Bytes(static_cast<const std::byte*>(static_cast<const void*>(d1)), n1), Bytes(static_cast<const std::byte*>(static_cast<const void*>(d2)), n2));
                if (!check_hr(h, rec)) return res;
                if (h >= 0) {
                    for (uint32_t i = 0; i < std::min(n1, rg.bytes1); ++i) pcm = (pcm ^ d1[i]) * fnv_prime;
                    for (uint32_t i = 0; i < std::min(n2, rg.bytes2); ++i) pcm = (pcm ^ d2[i]) * fnv_prime;
                }
                break;
            }
            case op_play: { const uint32_t id = p.u32(), pr = p.u32(), fl = p.u32(); const Hr rec = p.i32(); if (!check_hr(dev.play(BufferId{id}, pr, fl), rec)) return res; break; }
            case op_stop: { const uint32_t id = p.u32(); const Hr rec = p.i32(); if (!check_hr(dev.stop(BufferId{id}), rec)) return res; break; }
            case op_get_status: {
                const uint32_t id = p.u32(); const Hr rec = p.i32(); const uint32_t want = p.u32();
                uint32_t got = 0;
                const Hr h = dev.get_status(BufferId{id}, got);
                if (!check_hr(h, rec)) return res;
                if (h >= 0 && got != want) { fail(n, op, "status " + hex(got) + ", recorded " + hex(want)); return res; }
                break;
            }
            case op_get_position: {
                const uint32_t id = p.u32(); const bool wp = p.u8() != 0, ww = p.u8() != 0; const Hr rec = p.i32(); const uint32_t play = p.u32(), write = p.u32();
                uint32_t gp = 0, gw = 0;
                const Hr h = dev.get_current_position(BufferId{id}, wp ? &gp : nullptr, ww ? &gw : nullptr);
                if (!check_hr(h, rec)) return res;
                if (h >= 0 && ((wp && gp != play) || (ww && gw != write))) {
                    fail(n, op, "cursors " + std::to_string(gp) + "/" + std::to_string(gw) + ", recorded " + std::to_string(play) + "/" + std::to_string(write));
                    return res;
                }
                break;
            }
            case op_set_position: { const uint32_t id = p.u32(), pos = p.u32(); const Hr rec = p.i32(); if (!check_hr(dev.set_current_position(BufferId{id}, pos), rec)) return res; break; }
            case op_restore: { const uint32_t id = p.u32(); const Hr rec = p.i32(); if (!check_hr(dev.restore(BufferId{id}), rec)) return res; break; }
            case op_set_volume: { const uint32_t id = p.u32(); const int32_t v = p.i32(); const Hr rec = p.i32(); if (!check_hr(dev.set_volume(BufferId{id}, v), rec)) return res; break; }
            case op_set_pan: { const uint32_t id = p.u32(); const int32_t v = p.i32(); const Hr rec = p.i32(); if (!check_hr(dev.set_pan(BufferId{id}, v), rec)) return res; break; }
            case op_set_frequency: { const uint32_t id = p.u32(), hz = p.u32(); const Hr rec = p.i32(); if (!check_hr(dev.set_frequency(BufferId{id}, hz), rec)) return res; break; }
            case op_set_notifications: {
                const uint32_t id = p.u32(), count = p.u32();
                std::vector<Notify> list;
                for (uint32_t i = 0; i < count && p.ok; ++i) { Notify nt; nt.offset = p.u32(); nt.token = p.u64(); list.push_back(nt); }
                const Hr rec = p.i32();
                if (!check_hr(dev.set_notifications(BufferId{id}, list), rec)) return res;
                break;
            }
            case op_set_listener: { const Listener3D l = p.listener(); const Hr rec = p.i32(); if (!check_hr(dev.set_listener(l), rec)) return res; break; }
            case op_set_buffer_3d: { const uint32_t id = p.u32(); const Buffer3D b3 = p.buffer3d(); const Hr rec = p.i32(); if (!check_hr(dev.set_buffer_3d(BufferId{id}, b3), rec)) return res; break; }
            case op_set_environment: dev.set_environment(p.environment()); break;
            case op_set_send: { const uint32_t id = p.u32(); const EnvironmentSend s = p.send(); const Hr rec = p.i32(); if (!check_hr(dev.set_buffer_send(BufferId{id}, s), rec)) return res; break; }
            case op_set_output: { OutputLayout l; l.sample_rate = p.u32(); l.channels = p.u32(); l.channel_mask = p.u32(); const uint32_t lat = p.u32(); dev.set_output(l, lat); break; }
            case op_render: {
                const uint32_t frames = p.u32(); const uint64_t want = p.u64();
                const uint32_t channels = dev.output().channels;
                scratch.assign(size_t(frames) * channels, 0.0f);
                dev.render(scratch.data(), frames);
                const uint64_t got = hash_samples(scratch.data(), scratch.size(), fnv_basis);
                mixed = hash_samples(scratch.data(), scratch.size(), mixed);
                if (got != want) { fail(n, op, "mixed block differs from the record (" + std::to_string(frames) + " frames)"); return res; }
                break;
            }
            default: fail(n, op, "unknown opcode"); return res;
        }
        if (!p.ok) { fail(n, op, "short record"); return res; }
    }
    if (!r.ok) { res.error = "truncated trace"; return res; }
    res.ok = true;
    res.records = n;
    res.pcm_hash = pcm;
    res.mixed_hash = mixed;
    if (pcm != info.pcm_hash || mixed != info.mixed_hash) {
        res.ok = false;
        res.error = "the totals differ from the record (pcm or mixed hash)";
    }
    return res;
}

}  // namespace nue::dsound
