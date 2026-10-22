// Shared helpers for the texture tests: synthetic images, PSNR.
#pragma once
#include "nue/texture.h"
#include "nue_test.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace texttest {

inline uint32_t rng_next(uint32_t& s) { s = s * 1664525u + 1013904223u; return s >> 8; }

// A photo-like RGBA8 image: smooth colour fields, sharp-edged shapes, noise, a soft alpha gradient plus a hard-edged alpha disc.
inline std::vector<uint8_t> synth_rgba(uint32_t w, uint32_t h, uint32_t seed = 1, bool with_alpha = true) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    uint32_t s = seed * 7919u + 17;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const double u = double(x) / w, v = double(y) / h;
            double r = 0.5 + 0.45 * std::sin(u * 9.0 + seed) * std::cos(v * 5.0);
            double g = 0.5 + 0.40 * std::sin(v * 11.0 + 1.0 + u * 3.0);
            double b = 0.5 + 0.35 * std::cos(u * 6.0 - v * 7.0 + seed);
            const double dx = u - 0.5, dy = v - 0.5;
            if (dx * dx + dy * dy < 0.04) { r = 0.9; g = 0.2; b = 0.1; }                      // hard-edged disc
            if (std::fabs(dx + dy) < 0.02) { r = g = b = 0.05; }                                // dark line
            const double n = (double(rng_next(s) & 0xFF) / 255.0 - 0.5) * 0.06;                // mild noise
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            auto q = [](double c) { return uint8_t(std::min(1.0, std::max(0.0, c)) * 255.0 + 0.5); };
            p[0] = q(r + n); p[1] = q(g + n); p[2] = q(b + n);
            p[3] = with_alpha ? q((dx * dx + dy * dy < 0.09) ? 0.4 + 0.6 * u : 0.0) : 255;
            if (with_alpha && dx * dx + dy * dy < 0.01) p[3] = 255;
        }
    return px;
}

inline double psnr_u8(const uint8_t* a, const uint8_t* b, size_t count, int channels /*of 4*/) {
    double se = 0; size_t n = 0;
    for (size_t i = 0; i < count; ++i)
        for (int k = 0; k < channels; ++k) { const double d = double(a[i * 4 + k]) - b[i * 4 + k]; se += d * d; ++n; }
    if (se == 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / (se / double(n)));
}

}  // namespace texttest
