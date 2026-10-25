// SIMD equivalence: the codecs are built per instruction set (SSE2/SSE4.1/AVX2 on x86, NEON on ARM, or plain C++). This test checks the build it is linked into against a golden file
// written by another build, so a scalar build, an AVX2 build and an ARM NEON build (host, or the Android emulator) are all measured against the same numbers.
//
//   nue_texture_test_simd                      compare with tests/texture/data/simd_golden.bin (path compiled in; override with NUE_TEX_SIMD_GOLDEN or argv[1])
//   nue_texture_test_simd --write <file>       (re)write the golden file from this build
//
// What is compared:
//   * decoders: fixed pseudo-random blocks (no encoder involved) decoded to RGBA8 / RGBA32F. Must match within 1 level (ASTC uses float math: the reference is not bit-identical across ISAs
//     because the library is built without bit-exact invariance); the share of bit-identical bytes is printed.
//   * encoders: a fixed synthetic image encoded at a fixed effort, decoded again: block count identical and the PSNR close to the golden one. astc-encoder, PVRTC and BC4/BC5 are deterministic
//     across instruction sets (bytes identical, 0.4 dB allowed); the etcpak based encoders (BC1, BC3, BC7, ETC, EAC) use different heuristics in their SSE/AVX/NEON/scalar code paths and differ
//     by up to ~1.5 dB (measured: scalar BC1 +0.7 dB, AVX2 BC7 -1.2 dB, AVX2 ETC2 +1.5 dB against SSE4.1), so they get 2.5 dB. Whether the bytes are identical is printed.
// Everything is synthetic: no game data.
#include "tex_common.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace texttest;

#ifndef NUE_TEX_SIMD_GOLDEN_PATH
#define NUE_TEX_SIMD_GOLDEN_PATH "data/simd_golden.bin"
#endif

struct Rec { uint32_t kind, fmt, size; std::vector<uint8_t> data; double psnr = 0; };      // kind 0 = decode (RGBA8), 1 = decode (RGBA32F), 2 = encode (data = encoded bytes, psnr)

static uint64_t xs(uint64_t& s) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }

struct E { nue_tex_format f; nue_tex_quality q; uint32_t w, h; };
static const E kEnc[] = {{NUE_TEX_BC1_UNORM, NUE_TEX_QUALITY_NORMAL, 64, 64}, {NUE_TEX_BC3_UNORM, NUE_TEX_QUALITY_NORMAL, 64, 64}, {NUE_TEX_BC4_UNORM, NUE_TEX_QUALITY_NORMAL, 64, 64},
                     {NUE_TEX_BC5_UNORM, NUE_TEX_QUALITY_NORMAL, 64, 64}, {NUE_TEX_BC7_UNORM, NUE_TEX_QUALITY_FAST, 64, 64}, {NUE_TEX_ETC1_RGB8, NUE_TEX_QUALITY_NORMAL, 64, 64},
                     {NUE_TEX_ETC2_RGB8, NUE_TEX_QUALITY_FAST, 64, 64}, {NUE_TEX_ETC2_RGB8, NUE_TEX_QUALITY_NORMAL, 64, 64}, {NUE_TEX_ETC2_RGBA8, NUE_TEX_QUALITY_NORMAL, 64, 64},
                     {NUE_TEX_EAC_R11_UNORM, NUE_TEX_QUALITY_NORMAL, 64, 64}, {NUE_TEX_EAC_RG11_UNORM, NUE_TEX_QUALITY_NORMAL, 64, 64},
                     {NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_QUALITY_FAST, 64, 64}, {NUE_TEX_ASTC_UNORM(3), NUE_TEX_QUALITY_FAST, 64, 64}, {NUE_TEX_ASTC_UNORM(4), NUE_TEX_QUALITY_NORMAL, 64, 64},
                     {NUE_TEX_ASTC_UNORM(7), NUE_TEX_QUALITY_FASTEST, 64, 64}, {NUE_TEX_ASTC_4x4_UNORM, NUE_TEX_QUALITY_FAST, 37, 29}, {NUE_TEX_PVRTC1_4BPP, NUE_TEX_QUALITY_NORMAL, 64, 64}};

static std::vector<Rec> build() {
    std::vector<Rec> out;
    // ---- decoders: random blocks of a 32x32 image
    struct D { nue_tex_format f; bool hdr; };
    const D dec[] = {{NUE_TEX_BC1_UNORM, false}, {NUE_TEX_BC2_UNORM, false}, {NUE_TEX_BC3_UNORM, false}, {NUE_TEX_BC4_UNORM, false}, {NUE_TEX_BC5_UNORM, false}, {NUE_TEX_BC7_UNORM, false},
                     {NUE_TEX_BC6H_UFLOAT, true}, {NUE_TEX_ETC1_RGB8, false}, {NUE_TEX_ETC2_RGB8, false}, {NUE_TEX_ETC2_RGBA8, false}, {NUE_TEX_EAC_R11_UNORM, false},
                     {NUE_TEX_EAC_RG11_UNORM, false}, {NUE_TEX_ASTC_4x4_UNORM, false}, {NUE_TEX_ASTC_UNORM(4), false}, {NUE_TEX_ASTC_UNORM(7), false}, {NUE_TEX_ASTC_4x4_SRGB, false},
                     {NUE_TEX_ASTC_4x4_HDR, true}, {NUE_TEX_PVRTC1_4BPP, false}};
    uint64_t seed = 0x1234567ull;
    for (const D& d : dec) {
        const uint32_t w = 32, h = 32;
        std::vector<uint8_t> blocks(size_t(nue_tex_level_size(d.f, w, h, 1)));
        for (auto& b : blocks) b = uint8_t(xs(seed) >> 24);
        if (d.f >= NUE_TEX_ASTC_4x4_UNORM && d.f <= NUE_TEX_ASTC_4x4_HDR + 14) {
            // make most ASTC blocks valid: clear the reserved/void-extent patterns by forcing a legal block mode (single partition, 0 bits set above)
            for (size_t i = 0; i + 16 <= blocks.size(); i += 16) { blocks[i] = uint8_t((blocks[i] & 0xE0) | 0x42); blocks[i + 1] &= 0x1F; }
        }
        Rec r; r.kind = d.hdr ? 1 : 0; r.fmt = d.f;
        std::vector<uint8_t> px(size_t(w) * h * (d.hdr ? 16 : 4));
        if (nue_tex_decode_level(d.f, w, h, blocks.data(), blocks.size(), d.hdr ? NUE_TEX_RGBA32_FLOAT : NUE_TEX_RGBA8_UNORM, px.data(), px.size(), nullptr) < 0) px.clear();
        r.data = px; r.size = uint32_t(px.size());
        out.push_back(std::move(r));
    }
    // ---- encoders
    for (const E& e : kEnc) {
        const auto src = synth_rgba(e.w, e.h, 3, true);
        nue_tex_encode_params p; nue_tex_encode_params_default(&p); p.quality = e.q;
        Rec r; r.kind = 2; r.fmt = e.f;
        r.data.resize(size_t(nue_tex_level_size(e.f, e.w, e.h, 1)));
        std::vector<uint8_t> dec_px(size_t(e.w) * e.h * 4);
        if (nue_tex_encode_level(e.f, e.w, e.h, NUE_TEX_RGBA8_UNORM, src.data(), size_t(e.w) * 4, r.data.data(), r.data.size(), &p) >= 0 &&
            nue_tex_decode_level(e.f, e.w, e.h, r.data.data(), r.data.size(), NUE_TEX_RGBA8_UNORM, dec_px.data(), dec_px.size(), nullptr) >= 0) {
            const int ch = (e.f == NUE_TEX_BC4_UNORM || e.f == NUE_TEX_EAC_R11_UNORM) ? 1 : (e.f == NUE_TEX_BC5_UNORM || e.f == NUE_TEX_EAC_RG11_UNORM) ? 2 : 3;
            r.psnr = psnr_u8(src.data(), dec_px.data(), size_t(e.w) * e.h, ch);
        } else r.data.clear();
        r.size = uint32_t(r.data.size());
        // an encoder record also remembers the quality and size in `fmt` high bits so two records of one format stay distinct
        r.fmt |= (uint32_t(e.q) << 16) | (uint32_t(e.w == 37) << 20);
        out.push_back(std::move(r));
    }
    return out;
}

static bool write_file(const char* path, const std::vector<Rec>& recs) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite("NUEG1\0\0\0", 1, 8, f);
    const uint32_t n = uint32_t(recs.size());
    std::fwrite(&n, 4, 1, f);
    for (const Rec& r : recs) {
        std::fwrite(&r.kind, 4, 1, f); std::fwrite(&r.fmt, 4, 1, f); std::fwrite(&r.size, 4, 1, f); std::fwrite(&r.psnr, 8, 1, f);
        if (r.size) std::fwrite(r.data.data(), 1, r.size, f);
    }
    std::fclose(f);
    return true;
}

static bool read_file(const char* path, std::vector<Rec>& recs) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    char magic[8]; uint32_t n = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, "NUEG1", 5) != 0 || std::fread(&n, 4, 1, f) != 1) { std::fclose(f); return false; }
    for (uint32_t i = 0; i < n; ++i) {
        Rec r;
        if (std::fread(&r.kind, 4, 1, f) != 1 || std::fread(&r.fmt, 4, 1, f) != 1 || std::fread(&r.size, 4, 1, f) != 1 || std::fread(&r.psnr, 8, 1, f) != 1) { std::fclose(f); return false; }
        r.data.resize(r.size);
        if (r.size && std::fread(r.data.data(), 1, r.size, f) != r.size) { std::fclose(f); return false; }
        recs.push_back(std::move(r));
    }
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    const std::vector<Rec> mine = build();
    if (argc >= 3 && std::strcmp(argv[1], "--write") == 0) {
        if (!write_file(argv[2], mine)) { std::printf("cannot write %s\n", argv[2]); return 1; }
        std::printf("wrote %zu records to %s (codecs: %s)\n", mine.size(), argv[2], nue_tex_codec_info());
        return 0;
    }
    const char* path = argc >= 2 ? argv[1] : std::getenv("NUE_TEX_SIMD_GOLDEN") ? std::getenv("NUE_TEX_SIMD_GOLDEN") : NUE_TEX_SIMD_GOLDEN_PATH;
    std::vector<Rec> gold;
    if (!read_file(path, gold)) { std::printf("cannot read the golden file %s\n", path); return 1; }
    std::printf("codecs: %s\ngolden: %s (%zu records)\n", nue_tex_codec_info(), path, gold.size());
    int failures = 0;
    if (gold.size() != mine.size()) { std::printf("  FAIL: %zu records here, %zu in the golden file\n", mine.size(), gold.size()); return 1; }
    for (size_t i = 0; i < mine.size(); ++i) {
        const Rec &a = mine[i], &g = gold[i];
        const char* name = nue_tex_format_name(nue_tex_format(a.fmt & 0xFFFF));
        if (a.kind != g.kind || a.fmt != g.fmt || a.size != g.size) { std::printf("  FAIL %-22s record layout differs (%u vs %u bytes)\n", name, a.size, g.size); ++failures; continue; }
        if (a.kind == 2) {
            const bool same = a.data == g.data;
            const double d = std::fabs(a.psnr - g.psnr);
            // the blocks the GOLDEN build produced, decoded by THIS build: real, valid blocks through this build's decoder
            const E& e = kEnc[i - (mine.size() - sizeof kEnc / sizeof kEnc[0])];
            const auto src = synth_rgba(e.w, e.h, 3, true);
            std::vector<uint8_t> px(size_t(e.w) * e.h * 4);
            double cross = -1;
            if (g.size && nue_tex_decode_level(e.f, e.w, e.h, g.data.data(), g.data.size(), NUE_TEX_RGBA8_UNORM, px.data(), px.size(), nullptr) >= 0) {
                const int ch = (e.f == NUE_TEX_BC4_UNORM || e.f == NUE_TEX_EAC_R11_UNORM) ? 1 : (e.f == NUE_TEX_BC5_UNORM || e.f == NUE_TEX_EAC_RG11_UNORM) ? 2 : 3;
                cross = psnr_u8(src.data(), px.data(), size_t(e.w) * e.h, ch);
            }
            const nue_tex_format base = nue_tex_format(a.fmt & 0xFFFF);
            const bool deterministic = (base >= NUE_TEX_ASTC_4x4_UNORM && base <= NUE_TEX_ASTC_12x12_SRGB) || base == NUE_TEX_PVRTC1_4BPP || base == NUE_TEX_BC4_UNORM || base == NUE_TEX_BC5_UNORM;
            const bool ok = a.size && d <= (deterministic ? 0.4 : 2.5) && cross >= 0 && std::fabs(cross - g.psnr) <= 0.05;
            std::printf("  %-4s %-22s q%u  encode psnr %.2f (golden %.2f), golden blocks decoded here %.2f, bytes %s\n", ok ? "ok" : "FAIL", name, (a.fmt >> 16) & 15, a.psnr, g.psnr, cross, same ? "identical" : "differ");
            if (!ok) ++failures;
        } else if (a.kind == 0) {
            size_t exact = 0; int maxd = 0;
            for (size_t k = 0; k < a.data.size(); ++k) { const int d = std::abs(int(a.data[k]) - int(g.data[k])); exact += d == 0; maxd = std::max(maxd, d); }
            const bool ok = a.size && maxd <= 1;
            std::printf("  %-4s %-22s decode  %.2f%% bytes identical, max difference %d\n", ok ? "ok" : "FAIL", name, 100.0 * double(exact) / double(a.data.size() ? a.data.size() : 1), maxd);
            if (!ok) ++failures;
        } else {
            size_t bad = 0; double maxd = 0;
            for (size_t k = 0; k + 4 <= a.data.size(); k += 4) {
                float x, y; std::memcpy(&x, &a.data[k], 4); std::memcpy(&y, &g.data[k], 4);
                if (std::isnan(x) && std::isnan(y)) continue;                  // random blocks can decode to NaN on both sides
                const double d = std::fabs(double(x) - double(y)), tol = 1e-3 + 1e-3 * std::fabs(double(y));
                if (!(d <= tol)) ++bad;
                maxd = std::max(maxd, d);
            }
            const bool ok = a.size && bad == 0;
            std::printf("  %-4s %-22s decode (float)  %zu values outside tolerance, max difference %g\n", ok ? "ok" : "FAIL", name, bad, maxd);
            if (!ok) ++failures;
        }
    }
    std::printf("%s\n", failures ? "SIMD EQUIVALENCE FAILED" : "all builds agree with the golden file");
    return failures ? 1 : 0;
}
