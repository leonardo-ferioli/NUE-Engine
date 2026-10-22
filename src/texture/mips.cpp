// Mip chain generation on RGBA32F data: box (exact halving), triangle, Kaiser and Lanczos separable filters, linear-light filtering,
// normal map renormalisation, alpha coverage preservation, wrap/clamp, premultiplied filtering, 3D (box in depth).
#include "image.hpp"

namespace nuetex {
namespace {

constexpr double PI = 3.14159265358979323846;

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 30; ++k) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); sum += term; }
    return sum;
}
double sinc(double x) { if (std::fabs(x) < 1e-8) return 1.0; x *= PI; return std::sin(x) / x; }

double kernel(nue_tex_mip_filter f, double x) {          // x in source texels at unit scale
    x = std::fabs(x);
    switch (f) {
    case NUE_TEX_MIP_BOX: return x < 0.5 ? 1.0 : x == 0.5 ? 0.5 : 0.0;
    case NUE_TEX_MIP_TRIANGLE: return x < 1.0 ? 1.0 - x : 0.0;
    case NUE_TEX_MIP_LANCZOS: return x < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    default: {      // Kaiser-windowed sinc, radius 3, alpha 4
        if (x >= 3.0) return 0.0;
        const double a = 4.0, t = x / 3.0;
        return sinc(x) * bessel_i0(a * std::sqrt(1.0 - t * t)) / bessel_i0(a);
    }
    }
}
double radius(nue_tex_mip_filter f) { return f == NUE_TEX_MIP_BOX ? 0.5 : f == NUE_TEX_MIP_TRIANGLE ? 1.0 : 3.0; }

struct Contrib { int32_t first; std::vector<float> w; };

std::vector<Contrib> make_contribs(uint32_t n_in, uint32_t n_out, nue_tex_mip_filter filter, bool wrap) {
    std::vector<Contrib> out(n_out);
    const double scale = double(n_in) / n_out;
    const double support = radius(filter) * std::max(1.0, scale);
    const double fscale = std::max(1.0, scale);       // widen the kernel when shrinking
    for (uint32_t i = 0; i < n_out; ++i) {
        const double centre = (i + 0.5) * scale;
        int32_t lo = int32_t(std::floor(centre - support)), hi = int32_t(std::ceil(centre + support));
        Contrib c; c.first = lo;
        double sum = 0;
        for (int32_t j = lo; j <= hi; ++j) {
            const double w = kernel(filter, (j + 0.5 - centre) / fscale);
            c.w.push_back(float(w)); sum += w;
        }
        if (sum == 0) { c.w.assign(c.w.size(), 0.f); c.w[c.w.size() / 2] = 1.f; sum = 1; }
        for (auto& w : c.w) w = float(w / sum);
        out[i] = std::move(c);
    }
    (void)wrap;
    return out;
}

inline int32_t addr(int32_t i, int32_t n, bool wrap) {
    if (wrap) { i %= n; return i < 0 ? i + n : i; }
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

}  // namespace

float alpha_coverage(const FloatLevel& l, float threshold) {
    const size_t n = size_t(l.w) * l.h * l.d;
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) if (l.px[i * 4 + 3] > threshold) ++c;
    return n ? float(c) / float(n) : 0.f;
}

void mip_fix_alpha_coverage(FloatLevel& l, float target) {
    const size_t n = size_t(l.w) * l.h * l.d;
    float lo = 0.f, hi = 4.f, best = 1.f;
    for (int it = 0; it < 24; ++it) {
        const float mid = 0.5f * (lo + hi);
        size_t c = 0;
        for (size_t i = 0; i < n; ++i) if (std::min(1.f, l.px[i * 4 + 3] * mid) > 0.5f) ++c;
        const float cov = n ? float(c) / float(n) : 0.f;
        if (cov < target) lo = mid; else hi = mid;
    }
    best = hi * 1.002f;       // smallest scale that reaches the coverage, nudged off the threshold so rounding to 8 bits keeps it
    for (size_t i = 0; i < n; ++i) l.px[i * 4 + 3] = std::min(1.f, l.px[i * 4 + 3] * best);
}

void mip_downsample(const FloatLevel& src, uint32_t nw, uint32_t nh, uint32_t nd, const nue_tex_mip_params& p, bool srgb, FloatLevel& out) {
    out.w = nw; out.h = nh; out.d = nd;
    out.px.assign(size_t(nw) * nh * nd * 4, 0.f);
    const bool wrap = (p.flags & NUE_TEX_MIP_WRAP) != 0, premul = (p.flags & NUE_TEX_MIP_PREMULTIPLY) != 0;
    const size_t srcn = size_t(src.w) * src.h * src.d;
    // working copy of the source in linear light, optionally premultiplied
    std::vector<float> work(src.px);
    if (srgb) for (size_t i = 0; i < srcn; ++i) for (int k = 0; k < 3; ++k) work[i * 4 + k] = srgb_to_linear(std::min(1.f, std::max(0.f, work[i * 4 + k])));
    if (premul) for (size_t i = 0; i < srcn; ++i) for (int k = 0; k < 3; ++k) work[i * 4 + k] *= work[i * 4 + 3];

    const bool exact_box = p.filter == NUE_TEX_MIP_BOX && (src.w == nw * 2 || (src.w == 1 && nw == 1)) && (src.h == nh * 2 || (src.h == 1 && nh == 1));
    std::vector<float> tmp;              // after the horizontal pass: nw x src.h x src.d
    const uint32_t sw = src.w, sh = src.h, sd = src.d;
    tmp.assign(size_t(nw) * sh * sd * 4, 0.f);
    if (exact_box) {
        tmp.assign(size_t(nw) * nh * sd * 4, 0.f);
        // one 2x2 (or 2x1, 1x2) average per output texel
        for (uint32_t z = 0; z < sd; ++z)
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    int cnt = 0;
                    for (uint32_t dy = 0; dy < (sh > nh ? 2u : 1u); ++dy)
                        for (uint32_t dx = 0; dx < (sw > nw ? 2u : 1u); ++dx) {
                            const float* s = &work[((size_t(z) * sh + y * (sh > nh ? 2 : 1) + dy) * sw + x * (sw > nw ? 2 : 1) + dx) * 4];
                            for (int k = 0; k < 4; ++k) acc[k] += s[k];
                            ++cnt;
                        }
                    float* o = &tmp[((size_t(z) * nh + y) * nw + x) * 4];
                    for (int k = 0; k < 4; ++k) o[k] = acc[k] / float(cnt);
                }
    } else {
        const auto cx = make_contribs(sw, nw, p.filter, wrap), cy = make_contribs(sh, nh, p.filter, wrap);
        parallel_for(p.executor, sh * sd, 16, 0, [&](uint32_t b, uint32_t e) {
            for (uint32_t row = b; row < e; ++row) {
                const uint32_t z = row / sh, y = row % sh;
                const float* srow = &work[(size_t(z) * sh + y) * sw * 4];
                float* orow = &tmp[(size_t(z) * sh + y) * nw * 4];
                for (uint32_t x = 0; x < nw; ++x) {
                    float acc[4] = {0, 0, 0, 0};
                    const Contrib& c = cx[x];
                    for (size_t t = 0; t < c.w.size(); ++t) {
                        const float* s = srow + size_t(addr(c.first + int32_t(t), int32_t(sw), wrap)) * 4;
                        for (int k = 0; k < 4; ++k) acc[k] += c.w[t] * s[k];
                    }
                    for (int k = 0; k < 4; ++k) orow[x * 4 + k] = acc[k];
                }
            }
        });
        std::vector<float> vert(size_t(nw) * nh * sd * 4, 0.f);
        parallel_for(p.executor, nh * sd, 16, 0, [&](uint32_t b, uint32_t e) {
            for (uint32_t row = b; row < e; ++row) {
                const uint32_t z = row / nh, y = row % nh;
                const Contrib& c = cy[y];
                float* orow = &vert[(size_t(z) * nh + y) * nw * 4];
                for (size_t t = 0; t < c.w.size(); ++t) {
                    const float* s = &tmp[(size_t(z) * sh + addr(c.first + int32_t(t), int32_t(sh), wrap)) * nw * 4];
                    for (uint32_t i = 0; i < nw * 4; ++i) orow[i] += c.w[t] * s[i];
                }
            }
        });
        tmp.swap(vert);
    }
    // depth: box over pairs (3D textures)
    if (nd == sd) out.px = tmp;
    else {
        for (uint32_t z = 0; z < nd; ++z)
            for (size_t i = 0; i < size_t(nw) * nh * 4; ++i) {
                const uint32_t z0 = std::min(sd - 1, z * 2), z1 = std::min(sd - 1, z * 2 + 1);
                out.px[size_t(z) * nw * nh * 4 + i] = 0.5f * (tmp[size_t(z0) * nw * nh * 4 + i] + tmp[size_t(z1) * nw * nh * 4 + i]);
            }
    }
    const size_t n = size_t(nw) * nh * nd;
    if (premul) for (size_t i = 0; i < n; ++i) { const float a = out.px[i * 4 + 3]; if (a > 1e-6f) for (int k = 0; k < 3; ++k) out.px[i * 4 + k] /= a; }
    if (p.flags & NUE_TEX_MIP_NORMAL_MAP) {
        for (size_t i = 0; i < n; ++i) {
            float v[3] = {out.px[i * 4] * 2 - 1, out.px[i * 4 + 1] * 2 - 1, out.px[i * 4 + 2] * 2 - 1};
            if (srgb) { /* normals are never sRGB */ }
            const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len > 1e-6f) { for (int k = 0; k < 3; ++k) out.px[i * 4 + k] = (v[k] / len) * 0.5f + 0.5f; }
        }
    }
    if (srgb) for (size_t i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) out.px[i * 4 + k] = linear_to_srgb(std::min(1.f, std::max(0.f, out.px[i * 4 + k])));
}

}  // namespace nuetex

extern "C" {
void NUE_CALL nue_tex_mip_params_default(nue_tex_mip_params* p) {
    if (!p) return;
    std::memset(p, 0, sizeof *p);
    p->struct_size = sizeof *p;
    p->filter = NUE_TEX_MIP_BOX;
}
}
