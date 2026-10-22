// Metal capability flags -> what the D3D9 layer (nue_d3d9, gpu::GpuCaps) needs, plus the DXT texture policy.
//
// Apple GPUs do not sample BC/DXT textures on iOS (MTLDevice.supportsBCTextureCompression is true only on Mac). NFSU2 ships DXT1/DXT3/DXT5.
// The policy below decides, per texture, how a BC texture is stored on the device. It is pure logic; the transcoder itself belongs to
// the graphics layer (see REQUESTS_ios.md).
#pragma once
#include <array>
#include <cstdint>

namespace nue::ios {

// Same names and order as nue::gpu::Format in nue_d3d9/include/nue/gpu.hpp (the adapter maps by index and a test pins the count).
enum class TexFormat : uint8_t {
    rgba8, bgra8, r5g6b5, a4r4g4b4, a1r5g5b5, r8, a8, rg8, rgba16f, rgba32f, r16f, r32f, rg16f, rg32f,
    d16, d24s8, d32f, bc1, bc2, bc3, astc4x4, etc2_rgba8, rgba8s, count
};
constexpr size_t kTexFormatCount = static_cast<size_t>(TexFormat::count);

// Measured on the device by the glue (MTLDevice queries); everything else is derived.
struct MetalProbe {
    int highest_apple_family = 0;        // N such that supportsFamily(MTLGPUFamilyAppleN) is the highest true (A7 = 1 ... A17/M3 = 9); 0 = unknown
    bool supports_bc = false;            // device.supportsBCTextureCompression
    bool unified_memory = true;          // device.hasUnifiedMemory
    bool is_simulator = false;           // iOS Simulator: Metal runs on the host GPU, caps differ
    uint64_t recommended_max_working_set = 0;   // device.recommendedMaxWorkingSetSize (0 = unknown)
    uint64_t physical_memory = 0;        // NSProcessInfo.physicalMemory
};

struct DeviceCaps {
    std::array<bool, kTexFormatCount> texture_format{};
    uint32_t max_texture_size = 0;
    uint32_t max_anisotropy = 1;
    uint32_t max_samples = 1;            // MSAA sample count supported on render targets
    bool instancing = false;
    bool mrt = false;
    bool origin_bottom_left = false;     // Metal: top-left origin
    bool homogeneous_depth = false;      // Metal clip depth is [0, 1]
    bool depth24_stencil8 = false;       // false on iOS: D24S8 must be emulated with D32F_S8
    bool shader_cache_binary_archive = false;   // MTLBinaryArchive (iOS 14+)
};

DeviceCaps derive_caps(const MetalProbe&);

// How a Direct3D block-compressed texture is stored on the GPU.
enum class TextureStorage : uint8_t {
    native_bc,       // uploaded as is (Mac / Simulator-on-Mac only)
    astc4x4,         // transcoded to ASTC 4x4 LDR: 8 bits/pixel, 2x the memory of BC1, same as BC3
    astc6x6,         // fallback under memory pressure: 3.56 bits/pixel, visibly softer
    rgba8,           // decoded: exact, 8x BC1 memory; only for small textures
};

struct TexturePolicy {
    TextureStorage preferred = TextureStorage::astc4x4;
    uint32_t exact_below_pixels = 32 * 32;   // textures this small or smaller are decoded to RGBA8 (cost negligible, no block artefacts)
    bool allow_astc6x6 = true;
};

// Memory-aware choice. `budget_bytes` is the share of the process budget available to textures, `estimated_bc_bytes` the sum of all BC
// texture sizes the game is expected to hold (0 = unknown, assume it fits). Pure arithmetic: ASTC 4x4 = 2x BC1 / 1x BC3, 6x6 = 0.89x BC1.
TexturePolicy choose_texture_policy(const DeviceCaps&, uint64_t budget_bytes, uint64_t estimated_bc_bytes);

// Per-texture decision. `format` must be bc1, bc2 or bc3; anything else returns rgba8.
TextureStorage storage_for_texture(const DeviceCaps&, const TexturePolicy&, TexFormat format, uint32_t width, uint32_t height);

// Bytes the stored texture will take (one mip level, block-rounded).
uint64_t storage_bytes(TextureStorage, uint32_t width, uint32_t height);

const char* texture_storage_name(TextureStorage);

// Process memory budget the engine should plan against.
struct MemoryBudget {
    uint64_t hard_limit;      // do not exceed (the 2 GB goal, or less on small devices)
    uint64_t texture_share;   // bytes available for textures
    uint64_t soft_limit;      // start shedding caches
};
// physical_memory: NSProcessInfo.physicalMemory; os_available: os_proc_available_memory() at start-up (0 = unknown).
MemoryBudget plan_memory_budget(uint64_t physical_memory, uint64_t os_available, uint64_t goal_bytes = 2048ull << 20);

}  // namespace nue::ios
