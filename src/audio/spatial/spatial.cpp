// Spatial rendering: scene maths (distance models, cone, occlusion, Doppler), amplitude panning (sine law stereo, pairwise 2D VBAP), a spherical
// head binaural model with an optional measured HRTF set, ambisonic encode / sampling decode (ACN, SN3D, orders 1..3) and the backend hook.
// The ambisonic harmonics are the standard SN3D real spherical harmonics; VBAP follows V. Pulkki, "Virtual sound source positioning using vector
// base amplitude panning" (JAES 1997); the binaural delay is Woodworth's spherical head formula. No third-party code is used.
#include "nue/audio_spatial.h"
#include "dsp/common.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

using nuea::clampf;
using nuea::kPi;

namespace {

constexpr float kDeg = 180.0f / kPi;

struct V3 { float x, y, z; };
inline V3 mk(const nue_vec3& v) { return {v.x, v.y, v.z}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { const float l = len(a); return l > 1e-9f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1}; }

struct Speaker { uint32_t channel; float az; };       // az in degrees, 0 front, + right

float speaker_azimuth(nue_channel_mask bit) {
    switch (bit) {
    case NUE_SPK_FL: return -30; case NUE_SPK_FR: return 30; case NUE_SPK_FC: return 0;
    case NUE_SPK_FLC: return -15; case NUE_SPK_FRC: return 15;
    case NUE_SPK_BL: return -110; case NUE_SPK_BR: return 110;
    case NUE_SPK_SL: return -90; case NUE_SPK_SR: return 90; case NUE_SPK_BC: return 180;
    case NUE_SPK_TFL: return -30; case NUE_SPK_TFR: return 30; case NUE_SPK_TFC: return 0;
    case NUE_SPK_TBL: return -110; case NUE_SPK_TBR: return 110; case NUE_SPK_TBC: return 180;
    default: return 0;
    }
}

float wrap180(float a) { while (a > 180.0f) a -= 360.0f; while (a < -180.0f) a += 360.0f; return a; }

// SN3D real spherical harmonics, ACN order, up to order 3 (16 channels). (x,y,z) is the unit direction in the ambisonic frame
// (x forward, y left, z up).
void sh_coeffs(float x, float y, float z, uint32_t order, float* c) {
    c[0] = 1.0f;
    if (order < 1) return;
    c[1] = y; c[2] = z; c[3] = x;
    if (order < 2) return;
    const float s3 = 1.7320508f;
    c[4] = s3 * x * y; c[5] = s3 * y * z; c[6] = 0.5f * (3.0f * z * z - 1.0f); c[7] = s3 * x * z; c[8] = 0.5f * s3 * (x * x - y * y);
    if (order < 3) return;
    c[9] = 0.7905694f * y * (3.0f * x * x - y * y);
    c[10] = 3.8729833f * x * y * z;
    c[11] = 0.6123724f * y * (5.0f * z * z - 1.0f);
    c[12] = 0.5f * z * (5.0f * z * z - 3.0f);
    c[13] = 0.6123724f * x * (5.0f * z * z - 1.0f);
    c[14] = 1.9364917f * z * (x * x - y * y);
    c[15] = 0.7905694f * x * (x * x - 3.0f * y * y);
}

struct Hrtf {
    std::vector<float> az, el, ir;       // ir: count * 2 * frames
    uint32_t count = 0, frames = 0;
    uint32_t nearest(float a, float e) const {
        uint32_t best = 0;
        float bd = 1e9f;
        for (uint32_t i = 0; i < count; ++i) {
            const float da = wrap180(az[i] - a), de = el[i] - e;
            const float d = da * da + de * de;
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
};

}  // namespace

struct nue_spatial_s {
    nue_spatial_mode mode = NUE_SPATIAL_PAN;
    nue_channel_mask mask = NUE_MASK_STEREO;
    uint32_t rate = 48000, ch = 2, order = 1, ambi_ch = 4, max_block = 4096;
    int lfe_index = -1;
    std::vector<Speaker> spk;
    std::vector<std::vector<float>> decode;        // [speaker][ambi channel]
    nue_spatial_backend_ops backend{};
    bool has_backend = false;
    const char* active = "pan";
    Hrtf hrtf;
    // block state
    nue_spatial_world world{};
    float* out = nullptr;
    uint32_t frames = 0;
    bool in_block = false;
    std::vector<float> ambi;                        // ambi_ch * frames
    struct Pending { nue_spatial_source src; nue_spatial_source_params params; const float* mono; };
    std::vector<Pending> pending;
};

struct nue_spatial_source_s {
    nue_spatial_s* owner = nullptr;
    std::vector<float> gains;                       // previous per-output-channel gains (PAN) / previous ambisonic coefficients
    std::vector<float> ambi_prev;
    float lp = 0, lp2[2] = {0, 0};
    float delay_line[2][256] = {};
    uint32_t dpos = 0;
    float prev_delay[2] = {0, 0}, prev_gain[2] = {0, 0}, prev_cut[2] = {1, 1};
    bool primed = false;
    // hrtf
    uint32_t hrtf_prev = 0;
    std::vector<float> hist;                        // mono history for FIR
    bool hrtf_primed = false;
};

namespace {

void eval_scene(const nue_spatial_world& w, const nue_spatial_source_params& p, nue_spatial_eval& e) {
    const V3 lp = mk(w.listener.position), lf = norm(mk(w.listener.forward)), lu = norm(mk(w.listener.up));
    const V3 lr = norm(cross(lu, lf));
    const V3 lu2 = cross(lf, lr);
    const bool head = (p.flags & NUE_SPATIAL_HEAD_RELATIVE) != 0;
    const V3 sp = mk(p.position);
    const V3 rel = head ? sp : sub(sp, lp);
    const float dist = len(rel);
    const V3 local = {dot(rel, lr), dot(rel, lu2), dot(rel, lf)};
    e.distance = dist;
    e.direction = dist > 1e-6f ? nue_vec3{local.x / dist, local.y / dist, local.z / dist} : nue_vec3{0, 0, 1};
    e.azimuth_deg = dist > 1e-6f ? std::atan2(local.x, local.z) * kDeg : 0.0f;
    e.elevation_deg = dist > 1e-6f ? std::asin(clampf(local.y / dist, -1.0f, 1.0f)) * kDeg : 0.0f;

    // distance model (mixer.h semantics)
    const float mind = std::max(p.min_distance > 0 ? p.min_distance : 1.0f, 1e-6f);
    const float maxd = std::max(p.max_distance > 0 ? p.max_distance : 1e9f, mind);
    const float roll = std::max(0.0f, p.rolloff * w.rolloff_factor);
    const float d = clampf(dist, mind, maxd);
    float g = 1.0f;
    switch (p.model) {
    case NUE_ATTEN_INVERSE: g = mind / (mind + roll * (d - mind)); break;
    case NUE_ATTEN_LINEAR: g = maxd > mind ? 1.0f - roll * (d - mind) / (maxd - mind) : 1.0f; break;
    case NUE_ATTEN_EXPONENTIAL: g = std::pow(d / mind, -roll); break;
    default: g = 1.0f; break;
    }
    e.distance_gain = clampf(g, 0.0f, 1.0f);

    // cone: angle between the cone axis and the line from the source to the listener
    float cone = 1.0f;
    const V3 cd = mk(p.cone_direction);
    if (len(cd) > 1e-6f && !head && p.cone_outer_deg > 0.0f) {
        const V3 to_l = norm(sub(lp, sp));
        const float ang = std::acos(clampf(dot(norm(cd), to_l), -1.0f, 1.0f)) * kDeg * 2.0f;      // full angle, like DirectSound's cone angles
        const float inner = clampf(p.cone_inner_deg, 0.0f, 360.0f), outer = std::max(clampf(p.cone_outer_deg, 0.0f, 360.0f), inner);
        const float og = clampf(p.cone_outer_gain, 0.0f, 1.0f);
        if (ang <= inner) cone = 1.0f;
        else if (ang >= outer) cone = og;
        else {
            const float t = (ang - inner) / std::max(outer - inner, 1e-6f);
            const float db0 = 0.0f, db1 = nuea::lin_to_db(std::max(og, 1e-5f));
            cone = nuea::db_to_lin(db0 + (db1 - db0) * t);                                      // linear in dB
        }
    }
    e.cone_gain = cone;
    const float occ = clampf(p.occlusion, 0.0f, 1.0f);
    e.occlusion_gain = nuea::db_to_lin(-40.0f * occ);
    const float vol = p.volume > 0 ? p.volume : 1.0f;
    e.total_gain = vol * e.distance_gain * cone * e.occlusion_gain;

    // Doppler: f' / f = (c + df * v_listener_toward_source) / (c + df * v_source_away_from_listener)
    float ratio = 1.0f;
    if (w.doppler_factor > 0.0f && dist > 1e-6f && !head) {
        const V3 u = norm(sub(sp, lp));
        const float c = std::max(w.speed_of_sound, 1.0f);
        const float vl = clampf(dot(mk(w.listener.velocity), u), -0.45f * c, 0.45f * c);
        const float vs = clampf(dot(mk(p.velocity), u), -0.45f * c, 0.45f * c);
        ratio = (c + w.doppler_factor * vl) / (c + w.doppler_factor * vs);
    }
    e.doppler_ratio = ratio;
}

// Pairwise (2D VBAP) gains for a horizontal angle.
void vbap_gains(const std::vector<Speaker>& spk, float az, float spread_deg, float* gains, uint32_t out_ch) {
    for (uint32_t i = 0; i < out_ch; ++i) gains[i] = 0.0f;
    if (spk.empty()) return;
    if (spk.size() == 1) { gains[spk[0].channel] = 1.0f; return; }
    std::vector<Speaker> s = spk;
    std::sort(s.begin(), s.end(), [](const Speaker& a, const Speaker& b) { return a.az < b.az; });
    const size_t n = s.size();
    az = wrap180(az);
    size_t ia = 0, ib = 0;
    bool found = false;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        float a0 = s[i].az, a1 = s[j].az;
        if (j == 0) a1 += 360.0f;
        float t = az;
        if (t < a0) t += 360.0f;
        if (t >= a0 && t <= a1) { ia = i; ib = j; found = true; break; }
    }
    if (!found) { ia = ib = 0; }
    const float a0 = s[ia].az * kPi / 180.0f, a1 = s[ib].az * kPi / 180.0f, t = az * kPi / 180.0f;
    float ga = 0, gb = 0;
    const float l11 = std::sin(a0), l12 = std::cos(a0), l21 = std::sin(a1), l22 = std::cos(a1);       // speaker unit vectors (x right, y forward)
    const float det = l11 * l22 - l12 * l21;
    if (ia != ib && std::fabs(det) > 1e-4f) {
        const float px = std::sin(t), py = std::cos(t);
        ga = (px * l22 - py * l21) / det;
        gb = (-px * l12 + py * l11) / det;
        if (ga < 0 || gb < 0) { ga = std::max(ga, 0.0f); gb = std::max(gb, 0.0f); }     // outside the pair (gap > 180): nearest speaker
    } else {
        ga = 1.0f;
    }
    const float nrm = std::sqrt(ga * ga + gb * gb);
    if (nrm > 1e-9f) { ga /= nrm; gb /= nrm; } else ga = 1.0f;
    gains[s[ia].channel] += ga;
    if (ia != ib) gains[s[ib].channel] += gb;
    const float k = clampf(spread_deg / 180.0f, 0.0f, 1.0f);
    if (k > 0.0f) {
        const float u = 1.0f / std::sqrt(float(n));
        float sum2 = 0;
        for (const Speaker& sp : s) { gains[sp.channel] = (1.0f - k) * gains[sp.channel] + k * u; sum2 += gains[sp.channel] * gains[sp.channel]; }
        const float r = 1.0f / std::sqrt(std::max(sum2, 1e-12f));
        for (const Speaker& sp : s) gains[sp.channel] *= r;
    }
}

void panning_gains(const nue_spatial_s& s, float az, float spread, float* gains) {
    if (s.ch == 1) { gains[0] = 1.0f; return; }
    if (s.spk.size() == 2 && s.spk[0].az == -30 && s.spk[1].az == 30) {                 // stereo: sine law over the full +-90 degrees
        float a = wrap180(az);
        if (a > 90) a = 180 - a; else if (a < -90) a = -180 - a;
        const float k = clampf(spread / 180.0f, 0.0f, 1.0f);
        const float pan = std::sin(a * kPi / 180.0f) * (1.0f - k);
        const float th = (pan + 1.0f) * kPi / 4.0f;
        gains[s.spk[0].channel] = std::cos(th);
        gains[s.spk[1].channel] = std::sin(th);
        return;
    }
    vbap_gains(s.spk, az, spread, gains, s.ch);
}

// One-pole lowpass coefficient for a cutoff in Hz.
inline float lp_coef(float hz, float rate) { return 1.0f - std::exp(-2.0f * kPi * clampf(hz, 50.0f, 0.45f * rate) / rate); }

void render_pan(nue_spatial_s& s, nue_spatial_source_s& st, const nue_spatial_eval& e, const nue_spatial_source_params& p, const float* mono, uint32_t frames) {
    std::vector<float>& prev = st.gains;
    if (prev.size() != s.ch) { prev.assign(s.ch, 0.0f); st.primed = false; }
    float g[NUE_AUDIO_MAX_CHANNELS] = {0};
    panning_gains(s, e.azimuth_deg, p.spread_deg, g);
    for (uint32_t c = 0; c < s.ch; ++c) g[c] *= e.total_gain;
    if (!st.primed) { for (uint32_t c = 0; c < s.ch; ++c) prev[c] = g[c]; st.primed = true; }
    // tone: occlusion and air absorption share one low-pass
    float cutoff = 22000.0f;
    if (p.occlusion > 0.0f) cutoff = std::min(cutoff, 500.0f + 21500.0f * (1.0f - p.occlusion) * (1.0f - p.occlusion));
    if (p.flags & NUE_SPATIAL_AIR_ABSORPTION) cutoff = std::min(cutoff, clampf(22000.0f * std::exp(-e.distance / std::max(s.world.distance_factor, 1e-3f) / 150.0f), 1500.0f, 22000.0f));
    const bool filt = cutoff < 0.45f * float(s.rate);
    const float a = lp_coef(cutoff, float(s.rate));
    const float inv = 1.0f / float(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        float x = mono[f];
        if (filt) { st.lp += a * (x - st.lp); x = st.lp; } else st.lp = x;
        const float t = float(f + 1) * inv;
        for (uint32_t c = 0; c < s.ch; ++c) s.out[size_t(f) * s.ch + c] += x * (prev[c] + (g[c] - prev[c]) * t);
    }
    for (uint32_t c = 0; c < s.ch; ++c) prev[c] = g[c];
}

void render_binaural(nue_spatial_s& s, nue_spatial_source_s& st, const nue_spatial_eval& e, const nue_spatial_source_params& p, const float* mono, uint32_t frames) {
    const float rate = float(s.rate);
    const float sin_lat = clampf(e.direction.x, -1.0f, 1.0f);              // lateral component: sin(azimuth) * cos(elevation)
    const float abs_lat = std::fabs(sin_lat);
    const float theta = std::asin(abs_lat);
    float delay[2], gain[2], cut[2];
    const int near_ear = sin_lat >= 0 ? 1 : 0;                              // source on the right -> right ear is near
    const int far_ear = 1 - near_ear;
    const float itd = (0.0875f / 343.0f) * (theta + std::sin(theta));        // Woodworth
    delay[near_ear] = 0.0f; delay[far_ear] = itd * rate;
    const float ild_db = -6.0f * abs_lat;
    gain[near_ear] = 1.0f; gain[far_ear] = nuea::db_to_lin(ild_db);
    cut[near_ear] = 20000.0f; cut[far_ear] = 1800.0f + 18000.0f * (1.0f - abs_lat) * (1.0f - abs_lat);
    if (e.direction.z < 0.0f) { const float back = -e.direction.z; cut[0] = std::min(cut[0], 20000.0f - 13000.0f * back); cut[1] = std::min(cut[1], 20000.0f - 13000.0f * back); }
    if (p.occlusion > 0.0f) {
        const float oc = 500.0f + 21500.0f * (1.0f - p.occlusion) * (1.0f - p.occlusion);
        cut[0] = std::min(cut[0], oc); cut[1] = std::min(cut[1], oc);
    }
    if (p.flags & NUE_SPATIAL_AIR_ABSORPTION) {
        const float ac = clampf(22000.0f * std::exp(-e.distance / std::max(s.world.distance_factor, 1e-3f) / 150.0f), 1500.0f, 22000.0f);
        cut[0] = std::min(cut[0], ac); cut[1] = std::min(cut[1], ac);
    }
    for (int i = 0; i < 2; ++i) gain[i] *= e.total_gain * 0.9f;
    if (!st.primed) { for (int i = 0; i < 2; ++i) { st.prev_delay[i] = delay[i]; st.prev_gain[i] = gain[i]; st.prev_cut[i] = cut[i]; } st.primed = true; }
    const float inv = 1.0f / float(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        const float t = float(f + 1) * inv;
        st.delay_line[0][st.dpos & 255] = mono[f];
        for (int ear = 0; ear < 2; ++ear) {
            const float d = st.prev_delay[ear] + (delay[ear] - st.prev_delay[ear]) * t;
            const uint32_t di = uint32_t(d);
            const float fr = d - float(di);
            const float a = st.delay_line[0][(st.dpos - di) & 255], b = st.delay_line[0][(st.dpos - di - 1) & 255];
            float x = a + (b - a) * fr;
            const float cutoff = st.prev_cut[ear] + (cut[ear] - st.prev_cut[ear]) * t;
            st.lp2[ear] += lp_coef(cutoff, rate) * (x - st.lp2[ear]);
            x = st.lp2[ear];
            s.out[size_t(f) * s.ch + ear] += x * (st.prev_gain[ear] + (gain[ear] - st.prev_gain[ear]) * t);
        }
        ++st.dpos;
    }
    for (int i = 0; i < 2; ++i) { st.prev_delay[i] = delay[i]; st.prev_gain[i] = gain[i]; st.prev_cut[i] = cut[i]; }
}

void render_hrtf(nue_spatial_s& s, nue_spatial_source_s& st, const nue_spatial_eval& e, const nue_spatial_source_params& p, const float* mono, uint32_t frames) {
    const Hrtf& h = s.hrtf;
    const uint32_t n = h.frames;
    if (st.hist.size() != n - 1) { st.hist.assign(n - 1, 0.0f); st.hrtf_primed = false; }
    const uint32_t cur = h.nearest(e.azimuth_deg, e.elevation_deg);
    if (!st.hrtf_primed) { st.hrtf_prev = cur; st.hrtf_primed = true; }
    const float gain = e.total_gain;
    std::vector<float> buf(size_t(n - 1) + frames);
    std::copy(st.hist.begin(), st.hist.end(), buf.begin());
    std::copy(mono, mono + frames, buf.begin() + (n - 1));
    const float inv = 1.0f / float(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        const float t = st.hrtf_prev == cur ? 1.0f : float(f + 1) * inv;      // crossfade to the new direction over the block
        for (int ear = 0; ear < 2; ++ear) {
            const float* ic = &h.ir[(size_t(cur) * 2 + ear) * n];
            const float* ip = &h.ir[(size_t(st.hrtf_prev) * 2 + ear) * n];
            float ac = 0, ap = 0;
            for (uint32_t k = 0; k < n; ++k) { const float x = buf[f + (n - 1) - k]; ac += ic[k] * x; ap += ip[k] * x; }
            s.out[size_t(f) * s.ch + ear] += gain * (ap + (ac - ap) * t);
        }
    }
    std::copy(buf.end() - (n - 1), buf.end(), st.hist.begin());
    st.hrtf_prev = cur;
    (void)p;
}

void render_ambi_encode(nue_spatial_s& s, nue_spatial_source_s& st, const nue_spatial_eval& e, const float* mono, uint32_t frames) {
    float c[16];
    // ambisonic frame: x forward, y left, z up
    sh_coeffs(e.direction.z, -e.direction.x, e.direction.y, s.order, c);
    if (st.ambi_prev.size() != s.ambi_ch) { st.ambi_prev.assign(s.ambi_ch, 0.0f); st.primed = false; }
    for (uint32_t k = 0; k < s.ambi_ch; ++k) c[k] *= e.total_gain;
    if (!st.primed) { for (uint32_t k = 0; k < s.ambi_ch; ++k) st.ambi_prev[k] = c[k]; st.primed = true; }
    const float inv = 1.0f / float(frames);
    for (uint32_t f = 0; f < frames; ++f) {
        const float t = float(f + 1) * inv;
        for (uint32_t k = 0; k < s.ambi_ch; ++k) s.ambi[size_t(k) * s.max_block + f] += mono[f] * (st.ambi_prev[k] + (c[k] - st.ambi_prev[k]) * t);
    }
    for (uint32_t k = 0; k < s.ambi_ch; ++k) st.ambi_prev[k] = c[k];
}

void render_builtin(nue_spatial_s& s, nue_spatial_source_s& st, const nue_spatial_source_params& p, const float* mono, uint32_t frames) {
    nue_spatial_eval e;
    NUE_STRUCT_INIT(&e);
    eval_scene(s.world, p, e);
    if (s.mode == NUE_SPATIAL_AMBISONIC) { s.active = "ambisonic"; render_ambi_encode(s, st, e, mono, frames); }
    else if (s.mode == NUE_SPATIAL_BINAURAL && s.ch == 2) {
        if (s.hrtf.count) { s.active = "hrtf"; render_hrtf(s, st, e, p, mono, frames); }
        else { s.active = "binaural"; render_binaural(s, st, e, p, mono, frames); }
    } else { s.active = "pan"; render_pan(s, st, e, p, mono, frames); }
}

}  // namespace

extern "C" {

NUE_API void NUE_CALL nue_spatial_world_default(nue_spatial_world* w) {
    std::memset(w, 0, sizeof(*w));
    w->struct_size = sizeof(*w);
    w->listener.struct_size = sizeof(w->listener);
    w->listener.forward = {0, 0, 1}; w->listener.up = {0, 1, 0};
    w->speed_of_sound = 343.0f; w->doppler_factor = 1.0f; w->rolloff_factor = 1.0f; w->distance_factor = 1.0f;
}

NUE_API void NUE_CALL nue_spatial_source_params_default(nue_spatial_source_params* p) {
    std::memset(p, 0, sizeof(*p));
    p->struct_size = sizeof(*p);
    p->min_distance = 1.0f; p->max_distance = 1e9f; p->rolloff = 1.0f; p->model = NUE_ATTEN_INVERSE;
    p->cone_inner_deg = 360.0f; p->cone_outer_deg = 360.0f; p->cone_outer_gain = 1.0f; p->volume = 1.0f;
}

NUE_API nue_result NUE_CALL nue_spatial_evaluate(const nue_spatial_world* w, const nue_spatial_source_params* p, nue_spatial_eval* out) {
    if (!w || !p || !out) return NUE_ERR_INVALID_ARGUMENT;
    NUE_STRUCT_INIT(out);
    eval_scene(*w, *p, *out);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_spatial_create(const nue_spatial_desc* d, nue_spatial* out) {
    if (!d || !out || d->sample_rate < 8000 || d->mode < NUE_SPATIAL_PAN || d->mode > NUE_SPATIAL_AMBISONIC) return NUE_ERR_INVALID_ARGUMENT;
    nue_channel_mask mask = d->mask ? d->mask : nue_channel_mask(NUE_MASK_STEREO);
    const uint32_t ch = nue_channel_mask_count(mask);
    if (ch == 0 || ch > NUE_AUDIO_MAX_CHANNELS) return NUE_ERR_INVALID_ARGUMENT;
    if (d->mode == NUE_SPATIAL_BINAURAL && ch != 2) return NUE_ERR_INVALID_ARGUMENT;
    auto* s = new (std::nothrow) nue_spatial_s();
    if (!s) return NUE_ERR_OUT_OF_MEMORY;
    s->mode = d->mode; s->mask = mask; s->rate = d->sample_rate; s->ch = ch;
    s->max_block = d->max_block_frames ? d->max_block_frames : 4096;
    s->order = d->ambisonic_order ? std::min<uint32_t>(d->ambisonic_order, 3) : 1;
    s->ambi_ch = (s->order + 1) * (s->order + 1);
    uint32_t idx = 0;
    for (nue_channel_mask bit = 1; bit && bit <= NUE_SPK_TBR; bit <<= 1) {
        if (!(mask & bit)) continue;
        if (bit == NUE_SPK_LFE) s->lfe_index = int(idx); else s->spk.push_back({idx, speaker_azimuth(bit)});
        ++idx;
    }
    if (d->mode == NUE_SPATIAL_AMBISONIC) {
        s->ambi.assign(size_t(s->ambi_ch) * s->max_block, 0.0f);
        // Mode-matching decoder: the speaker gains that best reproduce the source's spherical harmonics (regularised least squares over the
        // speaker harmonics), with max-rE order weights to keep the side lobes down. Works for irregular layouts such as 5.1.
        const size_t L = s->spk.size();
        const uint32_t K = s->ambi_ch;
        static const bool horizontal[16] = {true, true, false, true, true, false, false, false, true, true, false, false, false, false, false, true};
        std::vector<std::vector<double>> Y(L, std::vector<double>(K, 0.0));          // Y[speaker][k]
        for (size_t i = 0; i < L; ++i) {
            const float a = s->spk[i].az * kPi / 180.0f;
            float c[16];
            sh_coeffs(std::cos(a), -std::sin(a), 0.0f, s->order, c);
            for (uint32_t k = 0; k < K; ++k) Y[i][k] = horizontal[k] ? c[k] : 0.0;           // horizontal speakers only constrain the horizontal harmonics
        }
        std::vector<std::vector<double>> M(L, std::vector<double>(L, 0.0));
        double trace = 0;
        for (size_t i = 0; i < L; ++i) for (size_t j = 0; j < L; ++j) { for (uint32_t k = 0; k < K; ++k) M[i][j] += Y[i][k] * Y[j][k]; if (i == j) trace += M[i][j]; }
        for (size_t i = 0; i < L; ++i) M[i][i] += 0.1 * trace / double(L ? L : 1);
        // solve M * D = Y (L x K) by Gauss-Jordan
        std::vector<std::vector<double>> A(L, std::vector<double>(L + K, 0.0));
        for (size_t i = 0; i < L; ++i) { for (size_t j = 0; j < L; ++j) A[i][j] = M[i][j]; for (uint32_t k = 0; k < K; ++k) A[i][L + k] = Y[i][k]; }
        for (size_t col = 0; col < L; ++col) {
            size_t piv = col;
            for (size_t r = col + 1; r < L; ++r) if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
            std::swap(A[col], A[piv]);
            const double d = A[col][col];
            if (std::fabs(d) < 1e-12) continue;
            for (size_t j = 0; j < L + K; ++j) A[col][j] /= d;
            for (size_t r = 0; r < L; ++r) if (r != col) { const double f = A[r][col]; if (f != 0.0) for (size_t j = 0; j < L + K; ++j) A[r][j] -= f * A[col][j]; }
        }
        static const float kMaxRe[4][4] = {{1, 0, 0, 0}, {1, 0.577f, 0, 0}, {1, 0.775f, 0.4f, 0}, {1, 0.861f, 0.612f, 0.305f}};
        s->decode.assign(L, std::vector<float>(K, 0.0f));
        for (size_t i = 0; i < L; ++i)
            for (uint32_t k = 0; k < K; ++k) {
                const uint32_t n = k < 1 ? 0 : k < 4 ? 1 : k < 9 ? 2 : 3;
                s->decode[i][k] = horizontal[k] ? float(A[i][L + k]) * kMaxRe[s->order][n] : 0.0f;
            }
        float c0[16];
        sh_coeffs(1.0f, 0.0f, 0.0f, s->order, c0);
        float p2 = 0;
        for (size_t i = 0; i < s->spk.size(); ++i) { float g = 0; for (uint32_t k = 0; k < s->ambi_ch; ++k) g += s->decode[i][k] * c0[k]; p2 += g * g; }
        const float scale = p2 > 1e-9f ? 1.0f / std::sqrt(p2) : 1.0f;
        for (auto& row : s->decode) for (float& v : row) v *= scale;
    }
    if (d->backend && d->backend->render) { s->backend = *d->backend; s->has_backend = true; s->active = d->backend->name ? d->backend->name : "backend"; }
    s->pending.reserve(256);
    *out = s;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_spatial_destroy(nue_spatial s) {
    if (!s) return;
    if (s->has_backend && s->backend.destroy) s->backend.destroy(s->backend.userdata);
    delete s;
}

NUE_API uint32_t NUE_CALL nue_spatial_output_channels(nue_spatial s) { return s ? s->ch : 0; }

NUE_API nue_result NUE_CALL nue_spatial_source_create(nue_spatial s, nue_spatial_source* out) {
    if (!s || !out) return NUE_ERR_INVALID_ARGUMENT;
    auto* src = new (std::nothrow) nue_spatial_source_s();
    if (!src) return NUE_ERR_OUT_OF_MEMORY;
    src->owner = s;
    *out = src;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_spatial_source_destroy(nue_spatial_source src) { delete src; }

NUE_API nue_result NUE_CALL nue_spatial_begin_block(nue_spatial s, const nue_spatial_world* w, float* out, uint32_t frames) {
    if (!s || !w || !out || !frames || frames > s->max_block) return NUE_ERR_INVALID_ARGUMENT;
    if (s->in_block) return NUE_ERR_BAD_STATE;
    s->world = *w;
    s->out = out; s->frames = frames; s->in_block = true;
    std::memset(out, 0, size_t(frames) * s->ch * sizeof(float));
    if (!s->ambi.empty()) std::fill(s->ambi.begin(), s->ambi.end(), 0.0f);
    s->pending.clear();
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_spatial_render_source(nue_spatial_source src, const nue_spatial_source_params* p, const float* mono) {
    if (!src || !p || !mono) return NUE_ERR_INVALID_ARGUMENT;
    nue_spatial_s* s = src->owner;
    if (!s->in_block) return NUE_ERR_BAD_STATE;
    s->pending.push_back({src, *p, mono});
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_spatial_end_block(nue_spatial s) {
    if (!s || !s->in_block) return NUE_ERR_BAD_STATE;
    bool done = false;
    if (s->has_backend) {
        std::vector<nue_spatial_backend_source> list(s->pending.size());
        for (size_t i = 0; i < s->pending.size(); ++i) {
            list[i].mono = s->pending[i].mono;
            list[i].params = s->pending[i].params;
            NUE_STRUCT_INIT(&list[i].eval);
            eval_scene(s->world, s->pending[i].params, list[i].eval);
        }
        // the backend accumulates into a scratch copy so that a failure leaves the output clean for the fallback
        std::vector<float> scratch(size_t(s->frames) * s->ch, 0.0f);
        if (!NUE_FAILED(s->backend.render(s->backend.userdata, &s->world, list.data(), uint32_t(list.size()), s->frames, scratch.data()))) {
            for (size_t i = 0; i < scratch.size(); ++i) s->out[i] += scratch[i];
            s->active = s->backend.name ? s->backend.name : "backend";
            done = true;
        }
    }
    if (!done) {
        for (auto& p : s->pending) render_builtin(*s, *p.src, p.params, p.mono, s->frames);
        if (s->mode == NUE_SPATIAL_AMBISONIC) {
            for (uint32_t f = 0; f < s->frames; ++f)
                for (size_t i = 0; i < s->spk.size(); ++i) {
                    float acc = 0;
                    for (uint32_t k = 0; k < s->ambi_ch; ++k) acc += s->decode[i][k] * s->ambi[size_t(k) * s->max_block + f];
                    s->out[size_t(f) * s->ch + s->spk[i].channel] += acc;
                }
        }
    }
    s->in_block = false;
    return NUE_OK;
}

NUE_API const char* NUE_CALL nue_spatial_active_renderer(nue_spatial s) { return s ? s->active : ""; }

NUE_API nue_result NUE_CALL nue_spatial_set_hrtf(nue_spatial s, const float* az, const float* el, const float* irs, uint32_t count, uint32_t frames, uint32_t ir_rate) {
    if (!s || !az || !el || !irs || !count || frames < 2 || frames > 1024 || ir_rate < 8000) return NUE_ERR_INVALID_ARGUMENT;
    if (s->ch != 2) return NUE_ERR_BAD_STATE;
    Hrtf h;
    h.count = count;
    h.az.assign(az, az + count); h.el.assign(el, el + count);
    uint32_t out_frames = frames;
    if (ir_rate != s->rate) {
        out_frames = std::min<uint32_t>(1024, uint32_t(std::ceil(double(frames) * s->rate / ir_rate)) + 1);
        h.ir.assign(size_t(count) * 2 * out_frames, 0.0f);
        for (size_t i = 0; i < size_t(count) * 2; ++i) {
            const uint32_t got = nue_resample_buffer(irs + i * frames, frames, 1, ir_rate, s->rate, NUE_RESAMPLE_HIGH, &h.ir[i * out_frames], out_frames);
            (void)got;
        }
    } else {
        h.ir.assign(irs, irs + size_t(count) * 2 * frames);
    }
    h.frames = out_frames;
    s->hrtf = std::move(h);
    return NUE_OK;
}

}  // extern "C"
