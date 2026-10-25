// MixerDevice: the Level A implementation (see include/nue_legacy/dsound_device.hpp). A software mixer with DirectSound semantics.
// Rate conversion is nue_resampler, channel layouts nue_chanmix, 3D and the environment go through detail::Placement (nue_spatial, nue_reverb).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#include "nue/audio_dsp.h"
#include "nue_legacy/dsound_device.hpp"
#include "placement.hpp"

namespace nue::dsound {

float millibels_to_amplitude(int32_t mb) { return mb <= volume_min ? 0.0f : std::pow(10.0f, float(mb) / 2000.0f); }

void pan_gains(int32_t pan, float& left, float& right) {
    // DirectSound's pan attenuates only the opposite channel, by |pan| hundredths of dB.
    left = pan > 0 ? millibels_to_amplitude(-pan) : 1.0f;
    right = pan < 0 ? millibels_to_amplitude(pan) : 1.0f;
}

namespace {

bool format_ok(const WaveFormat& f, uint32_t max_channels, uint32_t max_rate) {
    if (f.channels < 1 || f.channels > max_channels || f.channels > 8) return false;
    if (f.sample_rate < frequency_min || f.sample_rate > max_rate) return false;
    if (f.type == SampleType::ieee_float) return f.bits == 32;
    return f.bits == 8 || f.bits == 16 || f.bits == 24 || f.bits == 32;
}

// Owner of a nue_resampler. Copying a voice (DuplicateSoundBuffer) gives the copy no resampler: it makes its own when it plays.
struct Resampler {
    nue_resampler h = nullptr;
    Resampler() = default;
    Resampler(const Resampler&) {}
    Resampler& operator=(const Resampler&) {
        reset();
        return *this;
    }
    Resampler(Resampler&& o) noexcept : h(o.h) { o.h = nullptr; }
    Resampler& operator=(Resampler&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    ~Resampler() { reset(); }
    void reset() {
        if (h) nue_resampler_destroy(h);
        h = nullptr;
    }
};

struct Voice {
    bool used = false, primary = false, lost = false, playing = false, looping = false, primary_playing = false;
    uint32_t generation = 1, flags = 0;
    WaveFormat format;
    std::vector<uint8_t> data;
    uint32_t frames = 0, frame_bytes = 0;
    double pos = 0;                // next source frame to mix (0 .. frames); whole frames
    double start = 0, consumed = 0;   // position at Play and source frames mixed since (output frames x source rate / output rate): the heard position is start + consumed - latency
    double last_step = 1.0;
    int32_t volume = 0, pan = 0;
    uint32_t frequency = 0;
    std::vector<Notify> notifies;
    double notify_from = 0;        // heard position (unwrapped, frames) up to which notifications were already checked
    bool has_3d = false;
    Buffer3D b3d;
    EnvironmentSend send;
    std::vector<float> block;      // `block_frames` frames of `channels` floats at the output rate
    Resampler rs;                  // used while the buffer's rate (times the Doppler ratio) differs from the output rate
    bool rs_active = false;        // while true `pos` is where the next source frame is FED to the resampler (a few frames ahead of what is output)
    uint32_t rs_lat = 0;           // look-ahead of the resampler in source frames
    double rs_in = 0, rs_out = 0;
};

constexpr uint32_t stage_frames = 512;   // source frames handed to the resampler at a time

class MixerDevice final : public Device {
public:
    explicit MixerDevice(const Config& c) : cfg_(c) {
        voices_.resize(size_t(c.max_buffers) + 1);
        inputs_.reserve(voices_.size());
        stage_.assign(size_t(stage_frames) * 8, 0.0f);
        scratch_.assign(size_t(4096) * 8, 0.0f);
        // Slot 0 is the primary buffer (created on demand).
        primary_format_.sample_rate = c.default_rate;
        primary_format_.channels = uint16_t(c.default_channels);
        set_output_locked({48000, 2, speaker::stereo}, 0);
    }

    // ---- IDirectSound
    Hr set_cooperative_level(uint32_t level) override {
        if (level < coop::normal || level > coop::write_primary) return hr::invalid_param;
        std::lock_guard lock(mutex_);
        coop_ = level;
        return hr::ok;
    }
    void reset() override {
        std::lock_guard lock(mutex_);
        for (auto& v : voices_) {
            if (v.used) placement_.remove_source(id_of(int(&v - voices_.data())).value);
            const uint32_t gen = v.generation + 1;
            v = Voice{};
            v.generation = gen;
        }
        coop_ = coop::normal;
        primary_format_ = WaveFormat{};
        primary_format_.sample_rate = cfg_.default_rate;
        primary_format_.channels = uint16_t(cfg_.default_channels);
        listener_ = Listener3D{};
        env_ = Environment{};
        fifo_pos_ = fifo_count_ = 0;
        configure_placement();
    }
    Caps caps() const override {
        Caps c;
        c.flags = caps_flag::primary_mono | caps_flag::primary_stereo | caps_flag::primary_8bit | caps_flag::primary_16bit | caps_flag::continuous_rate |
                  caps_flag::secondary_mono | caps_flag::secondary_stereo | caps_flag::secondary_8bit | caps_flag::secondary_16bit;
        return c;
    }

    CreateResult create_buffer(const BufferDesc& d) override {
        std::lock_guard lock(mutex_);
        CreateResult r;
        if (d.flags & buffer_flag::primary) {
            if (d.has_format || d.bytes != 0) { r.hr = hr::invalid_param; return r; }
            if (d.flags & (buffer_flag::ctrl_fx | buffer_flag::static_buffer)) { r.hr = hr::invalid_param; return r; }
            Voice& p = voices_[0];
            if (p.used) {
                r.hr = hr::ok;
                r.id = id_of(0);
                r.existing_primary = true;
                return r;
            }
            const uint32_t primary_generation = p.generation + 1;
            p = Voice{};
            p.used = true;
            p.primary = true;
            p.flags = d.flags;
            p.generation = primary_generation;
            p.format = primary_format_;
            p.has_3d = (d.flags & buffer_flag::ctrl_3d) != 0;
            r.id = id_of(0);
            r.hr = hr::ok;
            return r;
        }
        if (!d.has_format) { r.hr = hr::invalid_param; return r; }
        if (d.flags & buffer_flag::ctrl_fx) { r.hr = hr::unsupported; return r; }
        if ((d.flags & buffer_flag::loc_hardware) && (d.flags & buffer_flag::loc_software)) { r.hr = hr::invalid_param; return r; }
        if (d.flags & buffer_flag::loc_hardware) { r.hr = hr::unsupported; return r; }   // measured: Windows answers E_NOTIMPL, there are no hardware voices
        if (!format_ok(d.format, 8, frequency_max)) { r.hr = hr::invalid_param; return r; }
        if (d.bytes < size_min || d.bytes > size_max) { r.hr = hr::invalid_param; return r; }
        const uint32_t fb = d.format.frame_bytes();
        const uint32_t bytes = (d.bytes + fb - 1) / fb * fb;   // measured: Windows rounds the size UP to a whole number of frames
        if (bytes > size_max) { r.hr = hr::invalid_param; return r; }
        const int slot = free_slot();
        if (slot < 0) { r.hr = hr::out_of_memory; return r; }
        Voice& v = voices_[size_t(slot)];
        const uint32_t gen = v.generation + 1;
        v = Voice{};
        v.generation = gen;
        v.used = true;
        v.flags = d.flags & ~buffer_flag::primary;
        v.format = d.format;
        v.frame_bytes = fb;
        v.frames = bytes / fb;
        v.data.assign(bytes, d.format.bits == 8 ? uint8_t(0x80) : uint8_t(0));   // silence
        v.frequency = d.format.sample_rate;
        v.has_3d = (d.flags & buffer_flag::ctrl_3d) != 0;
        v.block.assign(size_t(cfg_.block_frames) * d.format.channels, 0.0f);
        r.id = id_of(slot);
        r.hr = hr::ok;
        if (v.has_3d) sync_object(slot);
        return r;
    }

    CreateResult duplicate_buffer(BufferId source) override {
        std::lock_guard lock(mutex_);
        CreateResult r;
        Voice* s = find(source);
        if (!s || s->primary) { r.hr = hr::invalid_param; return r; }
        const int slot = free_slot();
        if (slot < 0) { r.hr = hr::out_of_memory; return r; }
        Voice& v = voices_[size_t(slot)];
        const uint32_t gen = v.generation + 1;
        v = *s;
        v.generation = gen;
        v.playing = v.looping = false;
        v.pos = v.start = v.consumed = 0;
        v.rs_active = false;
        v.notifies.clear();
        v.notify_from = 0;
        r.id = id_of(slot);
        r.hr = hr::ok;
        if (v.has_3d) sync_object(slot);
        return r;
    }

    void destroy_buffer(BufferId id) override {
        std::lock_guard lock(mutex_);
        const int slot = slot_of(id);
        if (slot < 0) return;
        placement_.remove_source(id.value);
        Voice& v = voices_[size_t(slot)];
        const uint32_t gen = v.generation;
        v = Voice{};
        v.generation = gen + 1;
    }

    // ---- format and caps
    Hr set_format(BufferId id, const WaveFormat& f) override {
        FormatChangedFn fn = nullptr;
        void* user = nullptr;
        {
            std::lock_guard lock(mutex_);
            Voice* v = find(id);
            if (!v) return hr::invalid_param;
            if (!v->primary) return hr::invalid_call;
            if (coop_ == coop::normal) return hr::priority_level_needed;
            if (f.channels > cfg_.max_primary_channels && f.channels <= 8 && f.sample_rate >= frequency_min) return hr::allocated;   // the device cannot do that many channels (measured: DSERR_ALLOCATED)
            if (!format_ok(f, cfg_.max_primary_channels, cfg_.max_primary_rate)) return hr::invalid_param;
            primary_format_ = f;
            v->format = f;
            fn = format_cb_;
            user = format_user_;
        }
        if (fn) fn(user, f);   // outside the lock: the callback may reopen the sink, which joins the mix thread
        return hr::ok;
    }
    Hr get_format(BufferId id, WaveFormat& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        out = v->primary ? primary_format_ : v->format;
        return hr::ok;
    }
    Hr get_caps(BufferId id, BufferCaps& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        out.flags = v->flags | (v->primary ? 0u : buffer_flag::loc_software);
        out.bytes = v->primary ? 0 : uint32_t(v->data.size());
        return hr::ok;
    }

    // ---- data
    Hr lock(BufferId id, uint32_t offset, uint32_t bytes, uint32_t flags, Regions& out) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (v->primary) return coop_ == coop::write_primary ? hr::unsupported : hr::invalid_call;
        if (v->lost) return hr::buffer_lost;
        if (flags & ~(lock_flag::from_write_cursor | lock_flag::entire_buffer)) return hr::invalid_param;
        const uint32_t size = uint32_t(v->data.size());
        if (flags & lock_flag::from_write_cursor) offset = write_cursor_bytes(*v);
        if (flags & lock_flag::entire_buffer) bytes = size;   // measured: the whole buffer starting at `offset` (wrapping), not forced to 0
        if (bytes > size || offset >= size || bytes == 0) return hr::invalid_param;
        out = {};
        out.offset1 = offset;
        if (offset + bytes <= size) {
            out.bytes1 = bytes;
        } else {
            out.bytes1 = size - offset;
            out.offset2 = 0;
            out.bytes2 = bytes - out.bytes1;
        }
        return hr::ok;
    }

    Hr unlock(BufferId id, const Regions& r, Bytes p1, Bytes p2) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v || v->primary) return hr::invalid_param;
        const uint32_t size = uint32_t(v->data.size());
        if (uint64_t(r.offset1) + r.bytes1 > size || uint64_t(r.offset2) + r.bytes2 > size) return hr::invalid_param;
        if (p1.size() < r.bytes1 || p2.size() < r.bytes2) return hr::invalid_param;
        if (r.bytes1) std::memcpy(v->data.data() + r.offset1, p1.data(), r.bytes1);
        if (r.bytes2) std::memcpy(v->data.data() + r.offset2, p2.data(), r.bytes2);
        return hr::ok;
    }

    // ---- transport
    Hr play(BufferId id, uint32_t priority, uint32_t flags) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (flags & ~0x3fu) return hr::invalid_param;
        if (v->primary) {
            if (!(flags & play_flag::looping)) return hr::invalid_param;
            v->primary_playing = true;
            return hr::ok;
        }
        if (priority != 0) return hr::invalid_param;
        if (v->lost) return hr::buffer_lost;
        v->looping = (flags & play_flag::looping) != 0;
        if (!v->playing) {
            v->playing = true;
            v->start = v->pos;
            v->consumed = 0;
            v->notify_from = v->pos;
            v->rs_active = false;
        }
        return hr::ok;
    }

    Hr stop(BufferId id) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (v->primary) {
            v->primary_playing = false;
            return hr::ok;
        }
        if (v->playing) {
            v->pos = std::floor(heard_frames(*v));   // resume where it was heard, not where the mixer had got to
            v->playing = false;
            v->rs_active = false;
            fire_stop(*v, id);
        }
        return hr::ok;
    }

    Hr get_status(BufferId id, uint32_t& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        out = 0;
        if (v->primary) {
            if (v->primary_playing) out |= status_flag::playing | status_flag::looping;
            return hr::ok;
        }
        if (v->lost) out |= status_flag::buffer_lost;
        if (v->playing) out |= status_flag::playing | (v->looping ? status_flag::looping : 0u);
        return hr::ok;   // measured: no location bit for a software buffer
    }

    Hr get_current_position(BufferId id, uint32_t* play_cursor, uint32_t* write_cursor) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (!play_cursor && !write_cursor) return hr::invalid_param;   // measured
        if (v->primary) {
            if (play_cursor) *play_cursor = 0;
            if (write_cursor) *write_cursor = 0;
            return hr::ok;
        }
        if (v->lost) return hr::buffer_lost;
        const uint32_t play = play_cursor_bytes(*v);
        if (play_cursor) *play_cursor = play;
        if (write_cursor) *write_cursor = v->playing ? write_cursor_bytes(*v) : play;
        return hr::ok;
    }

    Hr set_current_position(BufferId id, uint32_t byte_position) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (v->primary) return hr::invalid_call;
        if (v->lost) return hr::buffer_lost;
        if (byte_position >= v->data.size()) return hr::invalid_param;
        v->pos = double(byte_position / v->frame_bytes);
        v->start = v->pos;
        v->consumed = 0;
        v->notify_from = v->pos;
        v->rs_active = false;
        return hr::ok;
    }

    Hr restore(BufferId id) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        v->lost = false;
        return hr::ok;
    }

    // ---- controls
    Hr set_volume(BufferId id, int32_t mb) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (mb < volume_min || mb > volume_max) return hr::invalid_param;   // measured: the range is checked before the control flag
        if (!(v->flags & buffer_flag::ctrl_volume)) return hr::control_unavailable;
        v->volume = mb;
        return hr::ok;
    }
    Hr get_volume(BufferId id, int32_t& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (!(v->flags & buffer_flag::ctrl_volume)) return hr::control_unavailable;
        out = v->volume;
        return hr::ok;
    }
    Hr set_pan(BufferId id, int32_t mb) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (mb < pan_left || mb > pan_right) return hr::invalid_param;
        if (!(v->flags & buffer_flag::ctrl_pan)) return hr::control_unavailable;
        v->pan = mb;
        return hr::ok;
    }
    Hr get_pan(BufferId id, int32_t& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (!(v->flags & buffer_flag::ctrl_pan)) return hr::control_unavailable;
        out = v->pan;
        return hr::ok;
    }
    Hr set_frequency(BufferId id, uint32_t hz) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (v->primary) return hr::control_unavailable;
        if (hz != 0 && (hz < frequency_min || hz > frequency_max)) return hr::invalid_param;
        if (!(v->flags & buffer_flag::ctrl_frequency)) return hr::control_unavailable;
        if (hz == 0) hz = v->format.sample_rate;
        v->frequency = hz;
        return hr::ok;
    }
    Hr get_frequency(BufferId id, uint32_t& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (v->primary) return hr::control_unavailable;
        if (!(v->flags & buffer_flag::ctrl_frequency)) return hr::control_unavailable;
        out = v->frequency;
        return hr::ok;
    }
    Hr set_notifications(BufferId id, std::span<const Notify> positions) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (v->primary || !(v->flags & buffer_flag::ctrl_position_notify)) return hr::control_unavailable;
        if (v->playing) return hr::invalid_call;
        for (const Notify& n : positions)
            if (n.offset != notify_offset_stop && n.offset >= v->data.size()) return hr::invalid_param;
        v->notifies.assign(positions.begin(), positions.end());
        std::stable_sort(v->notifies.begin(), v->notifies.end(), [](const Notify& a, const Notify& b) { return a.offset < b.offset; });
        return hr::ok;
    }

    // ---- 3D
    Hr set_listener(const Listener3D& l) override {
        if (l.rolloff_factor < 0 || l.rolloff_factor > 10 || l.doppler_factor < 0 || l.doppler_factor > 10 || l.distance_factor <= 0) return hr::invalid_param;
        std::lock_guard lock(mutex_);
        listener_ = l;
        placement_.set_listener(l);
        return hr::ok;
    }
    Listener3D listener() const override {
        std::lock_guard lock(mutex_);
        return listener_;
    }
    Hr set_buffer_3d(BufferId id, const Buffer3D& p) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (!v->has_3d || v->primary) return hr::control_unavailable;
        if (p.min_distance <= 0 || p.max_distance <= 0 || p.inside_cone_angle > 360 || p.outside_cone_angle > 360 || p.inside_cone_angle > p.outside_cone_angle ||
            p.cone_outside_volume < volume_min || p.cone_outside_volume > volume_max)
            return hr::invalid_param;
        v->b3d = p;
        sync_object(slot_of(id));
        return hr::ok;
    }
    Hr get_buffer_3d(BufferId id, Buffer3D& out) const override {
        std::lock_guard lock(mutex_);
        const Voice* v = find(id);
        if (!v) return hr::invalid_param;
        if (!v->has_3d || v->primary) return hr::control_unavailable;
        out = v->b3d;
        return hr::ok;
    }
    void set_environment(const Environment& e) override {
        std::lock_guard lock(mutex_);
        env_ = e;
        placement_.set_environment(e);
    }
    Hr set_buffer_send(BufferId id, const EnvironmentSend& s) override {
        std::lock_guard lock(mutex_);
        Voice* v = find(id);
        if (!v || v->primary) return hr::invalid_param;
        v->send = s;
        return hr::ok;
    }

    // ---- spatial rendering
    void set_spatial(const SpatialOptions& o) override {
        std::lock_guard lock(mutex_);
        spatial_ = o;
        configure_placement();
    }
    void set_hrtf(std::span<const float> az, std::span<const float> el, std::span<const float> irs, uint32_t ir_frames, uint32_t ir_rate) override {
        std::lock_guard lock(mutex_);
        placement_.set_hrtf(az, el, irs, ir_frames, ir_rate);
    }
    const char* spatial_name() const override {
        std::lock_guard lock(mutex_);
        return placement_.name();
    }

    // ---- output
    void set_output(const OutputLayout& layout, uint32_t latency_frames) override {
        std::lock_guard lock(mutex_);
        set_output_locked(layout, latency_frames);
    }
    OutputLayout output() const override {
        std::lock_guard lock(mutex_);
        return out_;
    }
    void set_notify_callback(NotifyFn fn, void* user) override {
        std::lock_guard lock(mutex_);
        notify_cb_ = fn;
        notify_user_ = user;
    }
    void set_format_callback(FormatChangedFn fn, void* user) override {
        std::lock_guard lock(mutex_);
        format_cb_ = fn;
        format_user_ = user;
    }

    void render(float* out, uint32_t frames) override {
        std::array<uint64_t, 64> fired;
        size_t fired_count = 0;
        NotifyFn cb = nullptr;
        void* user = nullptr;
        {
            std::lock_guard lock(mutex_);
            const uint32_t channels = out_.channels;
            while (frames) {
                if (fifo_count_ == 0) {
                    mix_block();
                    fifo_pos_ = 0;
                    fifo_count_ = cfg_.block_frames;
                }
                const uint32_t n = std::min(frames, fifo_count_);
                std::memcpy(out, fifo_.data() + size_t(fifo_pos_) * channels, size_t(n) * channels * sizeof(float));
                out += size_t(n) * channels;
                fifo_pos_ += n;
                fifo_count_ -= n;
                frames -= n;
            }
            cb = notify_cb_;
            user = notify_user_;
            fired_count = pending_count_;
            std::copy(pending_.begin(), pending_.begin() + std::ptrdiff_t(pending_count_), fired.begin());
            pending_count_ = 0;
        }
        if (cb)
            for (size_t i = 0; i < fired_count; ++i) cb(user, fired[i]);
    }

    DeviceStats stats() const override {
        std::lock_guard lock(mutex_);
        DeviceStats s = stats_;
        for (size_t i = 1; i < voices_.size(); ++i) {
            if (voices_[i].used) ++s.buffers;
            if (voices_[i].used && voices_[i].playing) ++s.playing;
        }
        return s;
    }

    void lose_buffers() override {
        std::lock_guard lock(mutex_);
        for (size_t i = 1; i < voices_.size(); ++i)
            if (voices_[i].used) {
                voices_[i].lost = true;
                voices_[i].playing = false;
            }
    }

private:
    // ---- handles
    BufferId id_of(int slot) const { return BufferId{(voices_[size_t(slot)].generation << 12) | uint32_t(slot + 1)}; }
    int slot_of(BufferId id) const {
        const uint32_t slot = (id.value & 0xfff);
        if (slot == 0 || slot > voices_.size()) return -1;
        const Voice& v = voices_[slot - 1];
        if (!v.used || (id.value >> 12) != v.generation) return -1;
        return int(slot - 1);
    }
    Voice* find(BufferId id) {
        const int s = slot_of(id);
        return s < 0 ? nullptr : &voices_[size_t(s)];
    }
    const Voice* find(BufferId id) const { return const_cast<MixerDevice*>(this)->find(id); }
    int free_slot() const {
        for (size_t i = 1; i < voices_.size(); ++i)
            if (!voices_[i].used) return int(i);
        return -1;
    }

    // ---- cursors
    // Source position (frames, unwrapped from the Play) that the speaker is reproducing now.
    double heard_frames(const Voice& v) const {
        const double lat = double(latency_frames_ + fifo_count_) * v.last_step;
        double unwrapped = v.start + std::max(0.0, v.consumed - lat);
        if (v.looping) return std::fmod(unwrapped, double(v.frames));
        return std::min(unwrapped, double(v.frames));
    }
    uint32_t play_cursor_bytes(const Voice& v) const {
        const double f = v.playing ? heard_frames(v) : v.pos;
        const uint32_t frame = std::min<uint32_t>(uint32_t(f), v.frames ? v.frames - 1 : 0);
        return frame * v.frame_bytes;
    }
    uint32_t write_cursor_bytes(const Voice& v) const {
        // The first frame the mixer has not read yet plus one block (it may be mixing right now), and never closer to the play cursor than
        // the margin Windows gives (measured about 30 ms): data written at the write cursor is always in time.
        double ahead = double(cfg_.block_frames) * (v.playing ? v.last_step : 0.0);
        if (v.playing) {
            double queued = v.pos - heard_frames(v);   // frames mixed but not yet heard
            if (queued < 0) queued += v.looping ? double(v.frames) : 0.0;
            ahead = std::max(ahead, double(cfg_.write_margin_ms) * 0.001 * double(v.frequency) - std::max(0.0, queued));
        }
        double f = v.pos + ahead;
        if (v.playing) {
            if (v.looping) f = std::fmod(f, double(v.frames));
            else f = std::min(f, double(v.frames - 1));
        }
        return uint32_t(f) * v.frame_bytes;
    }

    // ---- 3D plumbing
    void sync_object(int slot) {
        if (slot < 0) return;
        placement_.set_object(id_of(slot).value, voices_[size_t(slot)].b3d);
    }
    void configure_placement() {
        placement_.configure(out_, cfg_.block_frames, cfg_.max_buffers + 1, spatial_);
        placement_.set_environment(env_);
        placement_.set_listener(listener_);
        for (size_t i = 1; i < voices_.size(); ++i)
            if (voices_[i].used && voices_[i].has_3d) sync_object(int(i));
    }
    void set_output_locked(const OutputLayout& layout, uint32_t latency_frames) {
        out_ = layout;
        if (out_.channels == 0) out_.channels = 2;
        if (!out_.channel_mask) out_.channel_mask = speaker::default_mask(out_.channels);
        latency_frames_ = latency_frames;
        fifo_.assign(size_t(cfg_.block_frames) * out_.channels, 0.0f);
        fifo_pos_ = fifo_count_ = 0;
        configure_placement();
    }

    // ---- notifications
    void queue_notify(uint64_t token) {
        if (pending_count_ < pending_.size()) pending_[pending_count_++] = token;
    }
    void fire_stop(Voice& v, BufferId) {
        for (const Notify& n : v.notifies)
            if (n.offset == notify_offset_stop) queue_notify(n.token);
    }
    // Notifications whose offset the heard cursor passed in this block.
    void check_notifications(Voice& v) {
        if (v.notifies.empty() || !v.playing) return;
        const double lat = double(latency_frames_ + cfg_.block_frames) * v.last_step;   // heard = mixed - latency
        const double heard_unw = v.start + std::max(0.0, v.consumed - lat);
        if (heard_unw <= v.notify_from) return;
        const double size = double(v.frames);
        for (const Notify& n : v.notifies) {
            if (n.offset == notify_offset_stop) continue;
            const double o = double(n.offset / v.frame_bytes);
            // first crossing at o + k*size > notify_from and <= heard_unw (k >= 0)
            double k = std::ceil((v.notify_from - o) / size);
            if (k < 0) k = 0;
            double at = o + k * size;
            if (at <= v.notify_from) at += size;
            if (at <= heard_unw && (v.looping || at < size + 1e-9)) queue_notify(n.token);
        }
        v.notify_from = heard_unw;
    }

    // ---- the mixer
    static void convert_frame(const Voice& v, int64_t frame, float* out) {
        const uint32_t ch = v.format.channels;
        const uint8_t* p = v.data.data() + size_t(frame) * v.frame_bytes;
        switch (v.format.bits) {
            case 8:
                for (uint32_t c = 0; c < ch; ++c) out[c] = (float(p[c]) - 128.0f) * (1.0f / 128.0f);
                break;
            case 16:
                for (uint32_t c = 0; c < ch; ++c) {
                    int16_t s;
                    std::memcpy(&s, p + c * 2, 2);
                    out[c] = float(s) * (1.0f / 32768.0f);
                }
                break;
            case 24:
                for (uint32_t c = 0; c < ch; ++c) {
                    int32_t s = int32_t(uint32_t(p[c * 3]) << 8 | uint32_t(p[c * 3 + 1]) << 16 | uint32_t(p[c * 3 + 2]) << 24) >> 8;
                    out[c] = float(s) * (1.0f / 8388608.0f);
                }
                break;
            default:   // 32
                if (v.format.type == SampleType::ieee_float) {
                    for (uint32_t c = 0; c < ch; ++c) std::memcpy(&out[c], p + c * 4, 4);
                } else {
                    for (uint32_t c = 0; c < ch; ++c) {
                        int32_t s;
                        std::memcpy(&s, p + c * 4, 4);
                        out[c] = float(double(s) * (1.0 / 2147483648.0));
                    }
                }
        }
    }
    // A source frame by index with the buffer's loop rules; outside a non-looping buffer it is silence.
    static void fetch(const Voice& v, int64_t index, float* out) {
        const int64_t n = int64_t(v.frames);
        if (v.looping) {
            index %= n;
            if (index < 0) index += n;
        } else if (index < 0 || index >= n) {
            for (uint32_t c = 0; c < v.format.channels; ++c) out[c] = 0.0f;
            return;
        }
        convert_frame(v, index, out);
    }

    // Feeds `count` source frames starting at `first` through the voice's resampler into `dst` (capacity `cap` output frames).
    // Returns the source frames consumed; *produced is the number of output frames written.
    uint32_t resample_chunk(Voice& v, int64_t first, uint32_t count, float* dst, uint32_t cap, uint32_t* produced) {
        const uint32_t ch = v.format.channels;
        count = std::min(count, stage_frames);
        for (uint32_t i = 0; i < count; ++i) fetch(v, first + int64_t(i), stage_.data() + size_t(i) * ch);
        uint32_t used = 0;
        nue_resampler_process(v.rs.h, stage_.data(), count, &used, dst, cap, produced);
        return used;
    }

    // (Re)starts the voice's resampler at the current position without a break in the stream: the filter history is the audio before the
    // position and the first output frame is the audio at the position, to within one output frame (the resampler's phase after the history was
    // fed is not ours to choose); v.consumed absorbs that difference so the cursors stay exact.
    void prime_resampler(Voice& v, double ratio) {
        nue_resampler_reset(v.rs.h);
        v.rs_lat = nue_resampler_latency_frames(v.rs.h);
        const int64_t pos0 = int64_t(v.pos);
        const int64_t from = pos0 - 2 * int64_t(v.rs_lat);
        const uint32_t total = 3 * v.rs_lat;
        uint32_t done = 0;
        double discarded = 0;
        while (done < total) {
            uint32_t produced = 0;
            const uint32_t used = resample_chunk(v, from + int64_t(done), total - done, scratch_.data(), uint32_t(scratch_.size() / v.format.channels), &produced);
            discarded += produced;
            if (used == 0 && produced == 0) break;
            done += used;
        }
        // The next output frame is at from + discarded * ratio; the position we want is pos0.
        v.consumed += discarded * ratio - 2.0 * double(v.rs_lat);
        v.pos = double(from + int64_t(done));
        v.rs_active = true;
    }

    // Produces `frames` output-rate frames of voice `v` into v.block, advancing v.pos. Returns false if the voice ended during the block.
    bool produce(Voice& v, uint32_t frames, double pitch) {
        const uint32_t ch = v.format.channels;
        const double in_rate = double(v.frequency) * pitch, out_rate = double(out_.sample_rate);
        const double ratio = in_rate / out_rate;
        const bool unity = std::fabs(ratio - 1.0) < 1e-9;
        bool ended = false;
        if (unity) {
            if (v.rs_active) {   // back to the bit-exact path: continue where the output is now
                double next = v.start + v.consumed;
                if (v.looping) next = std::fmod(next, double(v.frames));
                v.pos = std::floor(std::min(next, double(v.frames)));
                v.rs_active = false;
            }
            double pos = v.pos;
            for (uint32_t i = 0; i < frames; ++i) {
                float* o = v.block.data() + size_t(i) * ch;
                if (!v.looping && pos >= double(v.frames)) {
                    ended = true;
                    for (uint32_t c = 0; c < ch; ++c) o[c] = 0.0f;
                    continue;
                }
                fetch(v, int64_t(pos), o);
                pos += 1.0;
                if (v.looping && pos >= double(v.frames)) pos -= double(v.frames);
            }
            v.consumed += double(frames);
            v.pos = ended ? 0.0 : pos;
            if (!ended && !v.looping && pos >= double(v.frames)) ended = true;
            return !ended;
        }
        if (!v.rs.h) {
            if (nue_resampler_create(ch, in_rate, out_rate, cfg_.resample_quality, &v.rs.h) != NUE_OK) v.rs.h = nullptr;
            v.rs_in = in_rate;
            v.rs_out = out_rate;
        }
        if (!v.rs.h) {   // no resampler available: silence, but the cursors keep moving
            for (uint32_t i = 0; i < frames * ch; ++i) v.block[i] = 0.0f;
            v.consumed += double(frames) * ratio;
            return true;
        }
        if (v.rs_in != in_rate || v.rs_out != out_rate) {
            nue_resampler_set_rates(v.rs.h, in_rate, out_rate);
            v.rs_in = in_rate;
            v.rs_out = out_rate;
        }
        if (!v.rs_active) prime_resampler(v, ratio);
        uint32_t made = 0;
        int64_t pos = int64_t(v.pos);
        uint32_t guard = 0;
        while (made < frames && guard++ < 4096) {
            uint32_t produced = 0;
            const uint32_t want = std::min<uint32_t>(stage_frames, uint32_t(std::ceil(double(frames - made) * ratio)) + 4);
            const uint32_t used = resample_chunk(v, pos, want, v.block.data() + size_t(made) * ch, frames - made, &produced);
            pos += used;
            made += produced;
            if (used == 0 && produced == 0) break;
        }
        for (uint32_t i = made * ch; i < frames * ch; ++i) v.block[i] = 0.0f;
        v.consumed += double(frames) * ratio;
        if (v.looping) pos %= int64_t(v.frames);
        v.pos = double(pos);
        if (!v.looping && v.start + v.consumed >= double(v.frames)) {
            ended = true;
            v.pos = 0.0;
        }
        return !ended;
    }

    void mix_block() {
        const uint32_t frames = cfg_.block_frames;
        const uint32_t out_ch = out_.channels;
        float* out = fifo_.data();
        std::fill(out, out + size_t(frames) * out_ch, 0.0f);
        inputs_.clear();
        const float master = voices_[0].used && (voices_[0].flags & buffer_flag::ctrl_volume) ? millibels_to_amplitude(voices_[0].volume) : 1.0f;
        for (size_t slot = 1; slot < voices_.size(); ++slot) {
            Voice& v = voices_[slot];
            if (!v.used || !v.playing || v.frames == 0) continue;
            const BufferId id = id_of(int(slot));
            const bool object3d = v.has_3d && v.b3d.mode != Mode3D::disable;
            const float ratio = object3d ? placement_.pitch_ratio(id.value) : 1.0f;
            v.last_step = double(v.frequency) * double(ratio) / double(out_.sample_rate);
            const bool alive = produce(v, frames, double(ratio));
            detail::SourceBlock b;
            b.id = id.value;
            b.data = v.block.data();
            b.channels = v.format.channels;
            b.mask = v.format.effective_mask();
            b.send = v.send;
            const float volume = millibels_to_amplitude(v.volume) * master;
            if (object3d) {
                b.kind = detail::SourceKind::object;
                b.gain[0] = volume;
            } else if (v.format.channels <= 2) {
                b.kind = detail::SourceKind::stereo2d;
                float l, r;
                pan_gains(v.pan, l, r);
                b.gain[0] = volume * l;
                b.gain[1] = volume * r;
            } else {
                b.kind = detail::SourceKind::bed;
                b.gain[0] = volume;
            }
            inputs_.push_back(b);
            check_notifications(v);
            if (!alive) {
                v.playing = false;
                v.consumed = 0;
                v.start = 0;
                v.rs_active = false;
                fire_stop(v, id);
            }
        }
        placement_.render(std::span<const detail::SourceBlock>(inputs_.data(), inputs_.size()), out, frames);
        float peak = 0;
        for (size_t i = 0, n = size_t(frames) * out_ch; i < n; ++i) {
            const float a = std::fabs(out[i]);
            if (a > 1.0f) ++stats_.clipped_samples;
            peak = std::max(peak, a);
        }
        stats_.peak = peak;
        ++stats_.blocks_mixed;
    }

    Config cfg_;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::vector<detail::SourceBlock> inputs_;
    detail::Placement placement_;
    SpatialOptions spatial_;
    OutputLayout out_;
    WaveFormat primary_format_;
    uint32_t coop_ = coop::normal, latency_frames_ = 0;
    Listener3D listener_;
    Environment env_;
    std::vector<float> fifo_, stage_, scratch_;
    uint32_t fifo_pos_ = 0, fifo_count_ = 0;
    DeviceStats stats_;
    NotifyFn notify_cb_ = nullptr;
    void* notify_user_ = nullptr;
    FormatChangedFn format_cb_ = nullptr;
    void* format_user_ = nullptr;
    std::array<uint64_t, 64> pending_{};
    size_t pending_count_ = 0;
};

}  // namespace

std::unique_ptr<Device> make_device(const Config& config) { return std::make_unique<MixerDevice>(config); }

// ---- EAX 2.0 mappings ----------------------------------------------------------------------------------------------------------------------

Environment eax2_to_environment(const Eax2Listener& l) {
    nue_eax_reverb e{};
    e.room = l.room;
    e.room_hf = l.room_hf;
    e.room_lf = 0;
    e.decay_time = l.decay_time;
    e.decay_hf_ratio = l.decay_hf_ratio;
    e.decay_lf_ratio = 1.0f;
    e.reflections = l.reflections;
    e.reflections_delay = l.reflections_delay;
    e.reverb = l.reverb;
    e.reverb_delay = l.reverb_delay;
    e.echo_time = 0.25f;
    e.modulation_time = 0.25f;
    e.air_absorption_hf = l.air_absorption_hf;
    e.hf_reference = 5000.0f;
    e.lf_reference = 250.0f;
    e.room_rolloff_factor = l.room_rolloff;
    e.diffusion = std::clamp(l.environment_diffusion, 0.0f, 1.0f) * 100.0f;
    // EAX gives the environment size in meters (1..100): density follows the cube of the size like OpenAL Soft's conversion (small rooms are dense).
    e.density = std::clamp(std::pow(std::max(l.environment_size, 1.0f), 3.0f) / 16.0f / 100.0f, 0.0f, 1.0f) * 100.0f;
    e.flags = l.flags;
    Environment out;
    out.enabled = true;
    nue_reverb_params_from_eax(&out.params, &e);
    return out;
}

EnvironmentSend eax2_to_send(const Eax2Buffer& b) {
    EnvironmentSend s;
    // EAX 2.0: obstruction lowers the HF of the direct path (and obstruction * LF ratio the LF); occlusion lowers both direct and room, the
    // room by occlusion * occlusion_room_ratio.
    const float lf_mb = float(b.direct) + float(b.obstruction) * b.obstruction_lf_ratio + float(b.occlusion) * b.occlusion_lf_ratio;
    const float hf_mb = float(b.direct) + float(b.direct_hf) + float(b.obstruction) + float(b.occlusion);
    s.direct_gain = std::clamp(std::pow(10.0f, lf_mb / 2000.0f), 0.0f, 1.0f);
    s.direct_gain_hf = std::clamp(std::pow(10.0f, (hf_mb - lf_mb) / 2000.0f), 0.0f, 1.0f);
    const float room_mb = float(b.room) + float(b.occlusion) * b.occlusion_room_ratio;
    s.send_gain = std::clamp(std::pow(10.0f, room_mb / 2000.0f), 0.0f, 1.0f);
    s.send_gain_hf = std::clamp(std::pow(10.0f, float(b.room_hf) / 2000.0f), 0.0f, 1.0f);
    return s;
}

}  // namespace nue::dsound
