// Image quality metrics: PSNR (RGB / alpha), SSIM (luma), CIEDE2000 delta E, max error.
#include "image.hpp"

namespace nuetex {
namespace {

struct Lab { double L, a, b; };

Lab to_lab(const uint8_t* p, bool srgb) {
    double c[3];
    for (int k = 0; k < 3; ++k) { double v = p[k] / 255.0; c[k] = srgb ? srgb_to_linear(float(v)) : v; }
    const double X = 0.4124564 * c[0] + 0.3575761 * c[1] + 0.1804375 * c[2];
    const double Y = 0.2126729 * c[0] + 0.7151522 * c[1] + 0.0721750 * c[2];
    const double Z = 0.0193339 * c[0] + 0.1191920 * c[1] + 0.9503041 * c[2];
    auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
    const double fx = f(X / 0.95047), fy = f(Y), fz = f(Z / 1.08883);
    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double deg(double r) { return r * 180.0 / 3.14159265358979323846; }
double rad(double d) { return d * 3.14159265358979323846 / 180.0; }

double ciede2000(const Lab& p, const Lab& q) {
    const double C1 = std::hypot(p.a, p.b), C2 = std::hypot(q.a, q.b), Cb = 0.5 * (C1 + C2);
    const double G = 0.5 * (1 - std::sqrt(std::pow(Cb, 7) / (std::pow(Cb, 7) + std::pow(25.0, 7))));
    const double a1 = (1 + G) * p.a, a2 = (1 + G) * q.a;
    const double Cp1 = std::hypot(a1, p.b), Cp2 = std::hypot(a2, q.b);
    double h1 = deg(std::atan2(p.b, a1)), h2 = deg(std::atan2(q.b, a2));
    if (h1 < 0) h1 += 360; if (h2 < 0) h2 += 360;
    const double dL = q.L - p.L, dC = Cp2 - Cp1;
    double dh = 0;
    if (Cp1 * Cp2 != 0) { dh = h2 - h1; if (dh > 180) dh -= 360; else if (dh < -180) dh += 360; }
    const double dH = 2 * std::sqrt(Cp1 * Cp2) * std::sin(rad(dh / 2));
    const double Lb = 0.5 * (p.L + q.L), Cpb = 0.5 * (Cp1 + Cp2);
    double hb = h1 + h2;
    if (Cp1 * Cp2 == 0) hb = h1 + h2;
    else if (std::fabs(h1 - h2) <= 180) hb = 0.5 * (h1 + h2);
    else hb = (h1 + h2 < 360) ? 0.5 * (h1 + h2 + 360) : 0.5 * (h1 + h2 - 360);
    const double T = 1 - 0.17 * std::cos(rad(hb - 30)) + 0.24 * std::cos(rad(2 * hb)) + 0.32 * std::cos(rad(3 * hb + 6)) - 0.20 * std::cos(rad(4 * hb - 63));
    const double dTheta = 30 * std::exp(-std::pow((hb - 275) / 25, 2));
    const double Rc = 2 * std::sqrt(std::pow(Cpb, 7) / (std::pow(Cpb, 7) + std::pow(25.0, 7)));
    const double Sl = 1 + 0.015 * std::pow(Lb - 50, 2) / std::sqrt(20 + std::pow(Lb - 50, 2));
    const double Sc = 1 + 0.045 * Cpb, Sh = 1 + 0.015 * Cpb * T;
    const double Rt = -std::sin(rad(2 * dTheta)) * Rc;
    return std::sqrt(std::pow(dL / Sl, 2) + std::pow(dC / Sc, 2) + std::pow(dH / Sh, 2) + Rt * (dC / Sc) * (dH / Sh));
}

double psnr_from_mse(double mse) { return mse <= 1e-12 ? 99.0 : std::min(99.0, 10.0 * std::log10(255.0 * 255.0 / mse)); }

}  // namespace
}  // namespace nuetex

using namespace nuetex;

extern "C" {

nue_result NUE_CALL nue_tex_compare_level(nue_tex_format fa, const void* a, nue_tex_format fb, const void* b, uint32_t w, uint32_t h, nue_bool srgb, nue_tex_metrics* out) {
    if (!a || !b || !out || !w || !h) return NUE_ERR_INVALID_ARGUMENT;
    std::vector<uint8_t> pa(size_t(w) * h * 4), pb(size_t(w) * h * 4);
    nue_result r = nue_tex_decode_level(fa, w, h, a, size_t(nue_tex_level_size(fa, w, h, 1)), NUE_TEX_RGBA8_UNORM, pa.data(), pa.size(), nullptr);
    if (r < 0) return r;
    r = nue_tex_decode_level(fb, w, h, b, size_t(nue_tex_level_size(fb, w, h, 1)), NUE_TEX_RGBA8_UNORM, pb.data(), pb.size(), nullptr);
    if (r < 0) return r;
    const uint32_t keep = out->struct_size;
    std::memset(out, 0, sizeof *out);
    out->struct_size = keep ? keep : (uint32_t)sizeof *out;
    const size_t n = size_t(w) * h;
    double se_rgb = 0, se_a = 0, maxe = 0;
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) { const double d = double(pa[i * 4 + k]) - pb[i * 4 + k]; se_rgb += d * d; maxe = std::max(maxe, std::fabs(d)); }
        const double d = double(pa[i * 4 + 3]) - pb[i * 4 + 3];
        se_a += d * d; maxe = std::max(maxe, std::fabs(d));
    }
    const bool alpha = info(fa)->alpha > NUE_TEX_ALPHA_PADDING || info(fb)->alpha > NUE_TEX_ALPHA_PADDING;
    out->psnr_rgb_db = psnr_from_mse(se_rgb / (3.0 * n));
    out->psnr_alpha_db = psnr_from_mse(se_a / n);
    out->psnr_db = alpha ? psnr_from_mse((se_rgb + se_a) / (4.0 * n)) : out->psnr_rgb_db;
    out->max_abs_error = maxe;
    // SSIM over luma
    std::vector<float> ya(n), yb(n);
    for (size_t i = 0; i < n; ++i) {
        ya[i] = 0.299f * pa[i * 4] + 0.587f * pa[i * 4 + 1] + 0.114f * pa[i * 4 + 2];
        yb[i] = 0.299f * pb[i * 4] + 0.587f * pb[i * 4 + 1] + 0.114f * pb[i * 4 + 2];
    }
    const double C1 = 6.5025, C2 = 58.5225;
    const uint32_t win = std::min(8u, std::min(w, h)), stride = std::max(1u, win / 2);
    double ssum = 0; size_t cnt = 0;
    for (uint32_t y = 0; y + win <= h; y += stride)
        for (uint32_t x = 0; x + win <= w; x += stride) {
            double ma = 0, mb = 0, saa = 0, sbb = 0, sab = 0;
            for (uint32_t j = 0; j < win; ++j)
                for (uint32_t i = 0; i < win; ++i) { const double u = ya[size_t(y + j) * w + x + i], v = yb[size_t(y + j) * w + x + i]; ma += u; mb += v; saa += u * u; sbb += v * v; sab += u * v; }
            const double N = double(win) * win;
            ma /= N; mb /= N;
            const double va = saa / N - ma * ma, vb = sbb / N - mb * mb, cov = sab / N - ma * mb;
            ssum += ((2 * ma * mb + C1) * (2 * cov + C2)) / ((ma * ma + mb * mb + C1) * (va + vb + C2));
            ++cnt;
        }
    out->ssim = cnt ? ssum / double(cnt) : 1.0;
    // delta E (sampled to at most ~256k pixels)
    const size_t step = std::max<size_t>(1, n / 262144);
    double de_sum = 0, de_max = 0; size_t de_n = 0;
    for (size_t i = 0; i < n; i += step) {
        const double de = ciede2000(to_lab(&pa[i * 4], srgb != 0), to_lab(&pb[i * 4], srgb != 0));
        de_sum += de; de_max = std::max(de_max, de); ++de_n;
    }
    out->delta_e_mean = de_n ? de_sum / double(de_n) : 0;
    out->delta_e_max = de_max;
    return NUE_OK;
}

nue_result NUE_CALL nue_tex_compare_images(nue_tex_image a, nue_tex_image b, nue_tex_metrics* out) {
    if (!a || !b || !out) return NUE_ERR_INVALID_HANDLE;
    const Image &ia = *img(a), &ib = *img(b);
    if (ia.desc.width != ib.desc.width || ia.desc.height != ib.desc.height) return NUE_ERR_INVALID_ARGUMENT;
    return nue_tex_compare_level(ia.desc.format, ia.level_ptr(0, 0, 0), ib.desc.format, ib.level_ptr(0, 0, 0), ia.desc.width, ia.desc.height, is_srgb(ia.desc.format) ? NUE_TRUE : NUE_FALSE, out);
}

}  // extern "C"
