#include "placement.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace nue::dsound::detail {
namespace {

constexpr uint32_t max_channels = NUE_AUDIO_MAX_CHANNELS;
constexpr float pi = 3.14159265358979f;

// One-pole low-pass whose gain at Nyquist is k (Jot): y = (1-p) x + p y with p = (1-k)/(1+k).
inline float pole_for(float k) {
    const float c = std::clamp(k, 0.0f, 1.0f);
    return (1.0f - c) / (1.0f + c);
}

struct Entry {
    uint32_t id = 0;
    bool used = false, has_object = false, primed = false;
    nue_spatial_source_params sp{};
    nue_spatial_source src = nullptr;
    float prev[max_channels] = {};   // gains of the previous block per output channel (linear ramp to the new ones)
    float lp = 0;                    // direct-path HF filter state (obstruction/occlusion)
};

struct MixCache {
    uint32_t src_mask = 0, dst_mask = 0;
    nue_chanmix mix = nullptr;
};

}  // namespace

struct Placement::Impl {
    OutputLayout out;
    uint32_t max_block = 1;
    SpatialOptions options;
    Listener3D listener;
    nue_spatial_world world{};
    Environment env;
    std::vector<Entry> entries;
    std::vector<float> mono, send, spat_out, bed_tmp, rev_buf;
    std::vector<float> mono_pool;   // one mono block per object source: a backend keeps the pointers until the end of the block
    std::vector<MixCache> mixers;
    nue_spatial spatial = nullptr;
    nue_reverb reverb = nullptr;
    int left = 0, right = 1;        // output channels of the front pair
    bool use_balance = true;
    std::vector<float> hrtf_az, hrtf_el, hrtf_irs;
    uint32_t hrtf_frames = 0, hrtf_rate = 0;

    Impl() {
        nue_spatial_world_default(&world);
        world.struct_size = sizeof(world);
    }
    ~Impl() { release(); }

    void release() {
        for (Entry& e : entries)
            if (e.src) nue_spatial_source_destroy(e.src);
        entries.clear();
        for (MixCache& m : mixers)
            if (m.mix) nue_chanmix_destroy(m.mix);
        mixers.clear();
        if (spatial) nue_spatial_destroy(spatial);
        spatial = nullptr;
        if (reverb) nue_reverb_destroy(reverb);
        reverb = nullptr;
    }

    int channel_of(uint32_t bit) const {
        uint32_t index = 0;
        for (uint32_t b = 1; b; b <<= 1) {
            if (!(out.channel_mask & b)) continue;
            if (b == bit) return int(index);
            ++index;
        }
        return -1;
    }

    Entry* find(uint32_t id, bool create) {
        if (!id) return nullptr;
        Entry* free_slot = nullptr;
        for (Entry& e : entries) {
            if (e.used && e.id == id) return &e;
            if (!e.used && !free_slot) free_slot = &e;
        }
        if (create && free_slot) {
            if (free_slot->src) nue_spatial_source_destroy(free_slot->src);
            *free_slot = Entry{};
            free_slot->used = true;
            free_slot->id = id;
            nue_spatial_source_params_default(&free_slot->sp);
            return free_slot;
        }
        return nullptr;
    }

    nue_chanmix mixer_for(uint32_t src_mask, uint32_t dst_mask) {
        for (const MixCache& m : mixers)
            if (m.src_mask == src_mask && m.dst_mask == dst_mask) return m.mix;
        nue_chanmix m = nullptr;
        if (nue_chanmix_create(src_mask, dst_mask, 0, &m) != NUE_OK) m = nullptr;
        mixers.push_back({src_mask, dst_mask, m});   // a failure is cached too (null): the bed is silent, nothing is retried every block
        return m;
    }

    void push_world() {
        world.listener.struct_size = sizeof(nue_listener);
        world.listener.position = {listener.position.x, listener.position.y, listener.position.z};
        world.listener.velocity = {listener.velocity.x, listener.velocity.y, listener.velocity.z};
        world.listener.forward = {listener.front.x, listener.front.y, listener.front.z};
        world.listener.up = {listener.top.x, listener.top.y, listener.top.z};
        world.doppler_factor = listener.doppler_factor;
        world.rolloff_factor = listener.rolloff_factor;
        // DirectSound's distance factor is meters per world unit; the spatial module counts world units per meter.
        world.distance_factor = 1.0f / listener.distance_factor;
        world.speed_of_sound = 343.0f / listener.distance_factor;
    }

    // Mono mix of an input block (objects are point sources) through the direct-path HF filter of obstruction/occlusion.
    void mono_of(Entry& e, const SourceBlock& in, uint32_t frames, float* dst) {
        const float inv = 1.0f / float(in.channels);
        const float k = in.send.direct_gain_hf;
        const bool filtered = k < 0.999f;
        const float pole = pole_for(k);
        for (uint32_t f = 0; f < frames; ++f) {
            float s = 0;
            for (uint32_t c = 0; c < in.channels; ++c) s += in.data[size_t(f) * in.channels + c];
            s *= inv;
            if (filtered) {
                e.lp = (1.0f - pole) * s + pole * e.lp;
                s = e.lp;
            } else {
                e.lp = s;
            }
            dst[f] = s;
        }
    }

    void render_object(Entry& e, const SourceBlock& in, float* out_buf, uint32_t frames, bool reverb_on, uint32_t pool_slot, bool& spatial_used) {
        float* mono_block = mono_pool.data() + size_t(pool_slot) * max_block;
        mono_of(e, in, frames, mono_block);
        if (reverb_on && in.send.send_gain > 0.0f) {
            const float g = in.gain[0] * in.send.send_gain;
            for (uint32_t f = 0; f < frames; ++f) send[f] += mono_block[f] * g;
        }
        nue_spatial_source_params sp = e.has_object ? e.sp : nue_spatial_source_params{};
        if (!e.has_object) nue_spatial_source_params_default(&sp);
        const float volume = in.gain[0] * in.send.direct_gain;
        if (use_balance || !spatial) {
            nue_spatial_eval ev{};
            ev.struct_size = sizeof(ev);
            sp.volume = 1.0f;
            nue_spatial_evaluate(&world, &sp, &ev);
            const float gain = ev.total_gain * volume;
            // Balance pan from the lateral position: the near channel stays at unity, the far one falls as cos(pi/2 * |lateral|) (DirectSound's own
            // pan law attenuates only the far side; keeps a source crossing the centre level with a 2D one).
            const float lat = std::clamp(ev.direction.x, -1.0f, 1.0f);
            const float far_gain = std::cos(std::fabs(lat) * pi * 0.5f);
            float target[max_channels] = {};
            if (left == right) {
                target[left] = gain;
            } else {
                target[left] = gain * (lat > 0 ? far_gain : 1.0f);
                target[right] = gain * (lat < 0 ? far_gain : 1.0f);
            }
            if (!e.primed) std::memcpy(e.prev, target, sizeof(target));
            for (uint32_t f = 0; f < frames; ++f) {
                const float t = float(f + 1) / float(frames);
                const float x = mono_block[f];
                out_buf[size_t(f) * out.channels + uint32_t(left)] += x * (e.prev[left] + (target[left] - e.prev[left]) * t);
                if (right != left) out_buf[size_t(f) * out.channels + uint32_t(right)] += x * (e.prev[right] + (target[right] - e.prev[right]) * t);
            }
            std::memcpy(e.prev, target, sizeof(target));
            e.primed = true;
            return;
        }
        if (!e.src && nue_spatial_source_create(spatial, &e.src) != NUE_OK) return;
        if (volume <= 1e-9f) return;   // volume 0 means "unset" to the spatial module: silent sources are simply not rendered
        sp.volume = volume;
        if (nue_spatial_render_source(e.src, &sp, mono_block) == NUE_OK) spatial_used = true;
    }

    void render_stereo(Entry& e, const SourceBlock& in, float* out_buf, uint32_t frames, bool reverb_on) {
        // Front left / front right of the output; mono input goes to both.
        float target[max_channels] = {};
        if (left >= 0) target[left] = in.gain[0];
        if (right >= 0) target[right] = in.gain[1];
        const float send_gain = 0.5f * (in.gain[0] + in.gain[1]) * in.send.send_gain;
        if (!e.primed) std::memcpy(e.prev, target, sizeof(target));
        for (uint32_t f = 0; f < frames; ++f) {
            const float t = float(f + 1) / float(frames);
            const float sl = in.data[size_t(f) * in.channels];
            const float sr = in.channels > 1 ? in.data[size_t(f) * in.channels + 1] : sl;
            for (uint32_t c = 0; c < out.channels; ++c) {
                const float g = e.prev[c] + (target[c] - e.prev[c]) * t;
                if (g == 0.0f && target[c] == 0.0f) continue;
                const float s = (int(c) == right && left != right) ? sr : sl;
                out_buf[size_t(f) * out.channels + c] += s * g;
            }
            if (reverb_on) send[f] += 0.5f * (sl + sr) * send_gain;
        }
        std::memcpy(e.prev, target, sizeof(target));
        e.primed = true;
    }

    void render_bed(Entry& e, const SourceBlock& in, float* out_buf, uint32_t frames) {
        const uint32_t mask = in.mask ? in.mask : speaker::default_mask(in.channels);
        nue_chanmix m = mixer_for(mask, out.channel_mask);
        if (!m) return;
        nue_chanmix_process(m, in.data, frames, bed_tmp.data());
        const float g0 = e.primed ? e.prev[0] : in.gain[0], g1 = in.gain[0];
        const uint32_t n = nue_chanmix_dst_channels(m);
        for (uint32_t f = 0; f < frames; ++f) {
            const float g = g0 + (g1 - g0) * (float(f + 1) / float(frames));
            for (uint32_t c = 0; c < n && c < out.channels; ++c) out_buf[size_t(f) * out.channels + c] += bed_tmp[size_t(f) * n + c] * g;
        }
        e.prev[0] = g1;
        e.primed = true;
    }
};

Placement::Placement() : impl_(new Impl) {}
Placement::~Placement() { delete impl_; }

void Placement::configure(const OutputLayout& output, uint32_t max_block_frames, uint32_t max_sources, const SpatialOptions& options) {
    Impl& s = *impl_;
    s.release();
    s.out = output;
    s.out.channels = std::clamp<uint32_t>(s.out.channels, 1, max_channels);
    if (!s.out.channel_mask) s.out.channel_mask = speaker::default_mask(s.out.channels);
    s.options = options;
    s.max_block = std::max<uint32_t>(max_block_frames, 1);
    s.entries.assign(std::max<uint32_t>(max_sources, 1), Entry{});
    s.mono.assign(s.max_block, 0.0f);
    s.send.assign(s.max_block, 0.0f);
    s.mono_pool.assign(size_t(s.entries.size()) * s.max_block, 0.0f);
    s.spat_out.assign(size_t(s.max_block) * s.out.channels, 0.0f);
    s.rev_buf.assign(size_t(s.max_block) * s.out.channels, 0.0f);
    s.bed_tmp.assign(size_t(s.max_block) * max_channels, 0.0f);
    s.left = std::max(s.channel_of(speaker::front_left), 0);
    s.right = s.channel_of(speaker::front_right);
    if (s.right < 0) s.right = s.left;

    SpatialMode mode = options.mode;
    const bool plain_stereo = s.out.channels == 2 && s.out.channel_mask == speaker::stereo;
    if (mode == SpatialMode::automatic) mode = plain_stereo ? SpatialMode::balance : SpatialMode::pan;
    if (mode == SpatialMode::binaural && !plain_stereo) mode = SpatialMode::pan;   // headphones output is a stereo pair
    s.use_balance = (mode == SpatialMode::balance) && !options.backend;
    const char* name = "balance";
    if (!s.use_balance) {
        nue_spatial_desc d{};
        d.struct_size = sizeof(d);
        d.mode = mode == SpatialMode::binaural ? NUE_SPATIAL_BINAURAL : mode == SpatialMode::ambisonic ? NUE_SPATIAL_AMBISONIC : NUE_SPATIAL_PAN;
        d.mask = s.out.channel_mask;
        d.sample_rate = s.out.sample_rate;
        d.ambisonic_order = options.ambisonic_order;
        d.max_block_frames = s.max_block;
        d.backend = options.backend;
        if (nue_spatial_create(&d, &s.spatial) != NUE_OK) {
            s.spatial = nullptr;
            s.use_balance = true;   // could not create the renderer: fall back to DirectSound's own law
        } else {
            name = options.backend && options.backend->name ? options.backend->name
                   : d.mode == NUE_SPATIAL_BINAURAL        ? "binaural"
                   : d.mode == NUE_SPATIAL_AMBISONIC       ? "ambisonic"
                                                           : "pan";
            if (!s.hrtf_irs.empty())
                nue_spatial_set_hrtf(s.spatial, s.hrtf_az.data(), s.hrtf_el.data(), s.hrtf_irs.data(), uint32_t(s.hrtf_az.size()), s.hrtf_frames, s.hrtf_rate);
        }
    }
    name_ = name;
    if (nue_reverb_create(s.out.channels, s.out.channel_mask, s.out.sample_rate, &s.reverb) != NUE_OK) s.reverb = nullptr;
    if (s.reverb) nue_reverb_set_params(s.reverb, &s.env.params);
    s.push_world();
}

void Placement::set_listener(const Listener3D& l) {
    impl_->listener = l;
    impl_->push_world();
}

void Placement::set_environment(const Environment& e) {
    impl_->env = e;
    if (impl_->reverb) nue_reverb_set_params(impl_->reverb, &e.params);
}

void Placement::set_object(uint32_t id, const Buffer3D& b) {
    Entry* e = impl_->find(id, true);
    if (!e) return;
    nue_spatial_source_params& p = e->sp;
    nue_spatial_source_params_default(&p);
    p.position = {b.position.x, b.position.y, b.position.z};
    p.velocity = {b.velocity.x, b.velocity.y, b.velocity.z};
    p.cone_direction = {b.cone_orientation.x, b.cone_orientation.y, b.cone_orientation.z};
    p.cone_inner_deg = float(b.inside_cone_angle);
    p.cone_outer_deg = float(b.outside_cone_angle);
    p.cone_outer_gain = millibels_to_amplitude(b.cone_outside_volume);
    p.min_distance = b.min_distance;
    p.max_distance = b.max_distance;
    p.rolloff = 1.0f;
    p.model = NUE_ATTEN_INVERSE;
    p.volume = 1.0f;
    p.flags = b.mode == Mode3D::head_relative ? NUE_SPATIAL_HEAD_RELATIVE : 0u;
    e->has_object = true;
}

void Placement::remove_source(uint32_t id) {
    if (Entry* e = impl_->find(id, false)) {
        if (e->src) nue_spatial_source_destroy(e->src);
        *e = Entry{};
    }
}

void Placement::set_hrtf(std::span<const float> az, std::span<const float> el, std::span<const float> irs, uint32_t ir_frames, uint32_t ir_sample_rate) {
    Impl& s = *impl_;
    s.hrtf_az.assign(az.begin(), az.end());
    s.hrtf_el.assign(el.begin(), el.end());
    s.hrtf_irs.assign(irs.begin(), irs.end());
    s.hrtf_frames = ir_frames;
    s.hrtf_rate = ir_sample_rate;
    if (s.spatial && !s.hrtf_irs.empty())
        nue_spatial_set_hrtf(s.spatial, s.hrtf_az.data(), s.hrtf_el.data(), s.hrtf_irs.data(), uint32_t(s.hrtf_az.size()), ir_frames, ir_sample_rate);
}

float Placement::pitch_ratio(uint32_t id) const {
    const Entry* e = const_cast<Impl*>(impl_)->find(id, false);
    if (!e || !e->has_object) return 1.0f;
    nue_spatial_eval ev{};
    ev.struct_size = sizeof(ev);
    nue_spatial_evaluate(&impl_->world, &e->sp, &ev);
    return ev.doppler_ratio;
}

void Placement::render(std::span<const SourceBlock> inputs, float* out_buf, uint32_t frames) {
    Impl& s = *impl_;
    const bool reverb_on = s.env.enabled && s.reverb;
    if (reverb_on) std::fill(s.send.begin(), s.send.begin() + frames, 0.0f);
    bool begun = false, spatial_used = false;
    uint32_t pool_slot = 0;
    for (const SourceBlock& in : inputs) {
        if (!in.data || in.channels == 0) continue;
        Entry* e = s.find(in.id, true);
        if (!e) continue;
        switch (in.kind) {
            case SourceKind::object:
                if (!s.use_balance && s.spatial && !begun) {
                    nue_spatial_begin_block(s.spatial, &s.world, s.spat_out.data(), frames);
                    begun = true;
                }
                if (pool_slot < s.entries.size()) s.render_object(*e, in, out_buf, frames, reverb_on, pool_slot++, spatial_used);
                break;
            case SourceKind::stereo2d: s.render_stereo(*e, in, out_buf, frames, reverb_on); break;
            case SourceKind::bed: s.render_bed(*e, in, out_buf, frames); break;
        }
    }
    if (begun) {
        nue_spatial_end_block(s.spatial);
        const size_t n = size_t(frames) * s.out.channels;
        for (size_t i = 0; i < n; ++i) out_buf[i] += s.spat_out[i];
    }
    if (reverb_on) {
        // The send bus goes in on the front-left channel: nue_reverb sums every channel but the LFE to mono and answers with the wet signal only.
        std::fill(s.rev_buf.begin(), s.rev_buf.begin() + size_t(frames) * s.out.channels, 0.0f);
        for (uint32_t f = 0; f < frames; ++f) s.rev_buf[size_t(f) * s.out.channels + uint32_t(s.left)] = s.send[f];
        nue_reverb_process(s.reverb, s.rev_buf.data(), frames);
        const size_t n = size_t(frames) * s.out.channels;
        for (size_t i = 0; i < n; ++i) out_buf[i] += s.rev_buf[i];
    }
}

}  // namespace nue::dsound::detail
