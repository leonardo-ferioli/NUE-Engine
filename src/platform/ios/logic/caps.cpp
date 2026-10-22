#include "caps.hpp"

#include <algorithm>

namespace nue::ios {

namespace {
constexpr size_t idx(TexFormat f) { return static_cast<size_t>(f); }

uint64_t blocks(uint32_t w, uint32_t h, uint32_t bw, uint32_t bh) {
    return static_cast<uint64_t>((w + bw - 1) / bw) * ((h + bh - 1) / bh);
}
}  // namespace

DeviceCaps derive_caps(const MetalProbe& p) {
    DeviceCaps c;
    const int fam = p.highest_apple_family;
    const bool apple_gpu = fam >= 1 || p.is_simulator;
    auto set = [&](TexFormat f, bool v = true) { c.texture_format[idx(f)] = v; };

    set(TexFormat::rgba8); set(TexFormat::bgra8); set(TexFormat::r8); set(TexFormat::a8); set(TexFormat::rg8);
    set(TexFormat::rgba16f); set(TexFormat::rgba32f); set(TexFormat::r16f); set(TexFormat::r32f);
    set(TexFormat::rg16f); set(TexFormat::rg32f); set(TexFormat::rgba8s);
    set(TexFormat::d16); set(TexFormat::d32f);
    // 16-bit packed colour formats exist on Apple GPUs (MTLPixelFormatB5G6R5Unorm, ABGR4Unorm, A1BGR5Unorm) from the A8; keep them
    // off for unknown devices so the layer converts to RGBA8 instead of failing.
    const bool packed16 = fam >= 2;
    set(TexFormat::r5g6b5, packed16); set(TexFormat::a4r4g4b4, packed16); set(TexFormat::a1r5g5b5, packed16);

    // Depth24Unorm_Stencil8 is a Mac-only pixel format. Report d24s8 as unavailable natively; the layer substitutes d32f (stencil kept).
    c.depth24_stencil8 = !apple_gpu || p.supports_bc;   // Mac-class device
    set(TexFormat::d24s8, c.depth24_stencil8);

    set(TexFormat::bc1, p.supports_bc); set(TexFormat::bc2, p.supports_bc); set(TexFormat::bc3, p.supports_bc);
    // ASTC LDR and ETC2/EAC: Apple2 (A8) and later, and every Apple-silicon Mac.
    const bool astc = fam >= 2;
    set(TexFormat::astc4x4, astc);
    set(TexFormat::etc2_rgba8, astc);

    c.max_texture_size = fam >= 3 ? 16384 : 8192;   // Apple1/2: 8192, Apple3+: 16384 (Metal feature set tables)
    c.max_anisotropy = 16;
    c.max_samples = fam >= 1 ? 4 : 1;
    c.instancing = true;
    c.mrt = true;
    c.origin_bottom_left = false;
    c.homogeneous_depth = false;
    c.shader_cache_binary_archive = fam >= 3 || p.supports_bc;
    return c;
}

TexturePolicy choose_texture_policy(const DeviceCaps& caps, uint64_t budget, uint64_t est_bc) {
    TexturePolicy t;
    if (caps.texture_format[idx(TexFormat::bc1)]) { t.preferred = TextureStorage::native_bc; return t; }
    if (!caps.texture_format[idx(TexFormat::astc4x4)]) { t.preferred = TextureStorage::rgba8; t.allow_astc6x6 = false; return t; }
    t.preferred = TextureStorage::astc4x4;
    // ASTC 4x4 is 8 bpp; BC is 4 bpp (BC1) or 8 bpp (BC2/3). Assume the worst mix (all BC1) when unknown share is not known: 2x.
    if (budget != 0 && est_bc != 0 && est_bc * 2 > budget && t.allow_astc6x6) t.preferred = TextureStorage::astc6x6;
    return t;
}

TextureStorage storage_for_texture(const DeviceCaps& caps, const TexturePolicy& pol, TexFormat f, uint32_t w, uint32_t h) {
    if (f != TexFormat::bc1 && f != TexFormat::bc2 && f != TexFormat::bc3) return TextureStorage::rgba8;
    if (pol.preferred == TextureStorage::native_bc && caps.texture_format[idx(f)]) return TextureStorage::native_bc;
    if (static_cast<uint64_t>(w) * h <= pol.exact_below_pixels) return TextureStorage::rgba8;
    return pol.preferred == TextureStorage::native_bc ? TextureStorage::rgba8 : pol.preferred;
}

uint64_t storage_bytes(TextureStorage s, uint32_t w, uint32_t h) {
    switch (s) {
    case TextureStorage::native_bc: return blocks(w, h, 4, 4) * 8;   // BC1 size; BC2/BC3 double it (callers that care multiply)
    case TextureStorage::astc4x4: return blocks(w, h, 4, 4) * 16;
    case TextureStorage::astc6x6: return blocks(w, h, 6, 6) * 16;
    case TextureStorage::rgba8: return static_cast<uint64_t>(w) * h * 4;
    }
    return 0;
}

const char* texture_storage_name(TextureStorage s) {
    switch (s) {
    case TextureStorage::native_bc: return "native_bc";
    case TextureStorage::astc4x4: return "astc4x4";
    case TextureStorage::astc6x6: return "astc6x6";
    case TextureStorage::rgba8: return "rgba8";
    }
    return "?";
}

MemoryBudget plan_memory_budget(uint64_t physical, uint64_t os_available, uint64_t goal) {
    MemoryBudget b{};
    uint64_t hard = goal;
    if (physical != 0) hard = std::min<uint64_t>(hard, physical * 55 / 100);   // jetsam kills well before physical RAM; ~55 % is a safe ceiling
    if (os_available != 0) hard = std::min<uint64_t>(hard, os_available);       // what the OS says we can still allocate right now
    b.hard_limit = hard;
    b.soft_limit = hard * 85 / 100;
    b.texture_share = hard * 25 / 100;
    return b;
}

}  // namespace nue::ios
