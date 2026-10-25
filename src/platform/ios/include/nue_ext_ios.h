/*
 * NUE iOS extension "nue.ios.v1": what only iOS offers and the portable API does not carry.
 *   const nue_ext_ios_v1* ios = (const nue_ext_ios_v1*)nue_get_extension("nue.ios.v1");   // NULL on other platforms
 * Never compiled yet (written without macOS/Xcode access): see docs/PLATFORM_IOS.md.
 */
#ifndef NUE_EXT_IOS_H
#define NUE_EXT_IOS_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Metal capabilities for the Direct3D 9 layer. texture_format[] is indexed like nue::gpu::Format (nue_d3d9/include/nue/gpu.hpp). */
typedef struct nue_ios_gpu_info {
    uint32_t struct_size;
    char name[64];
    int32_t apple_family;                   /* highest MTLGPUFamilyAppleN supported, 0 unknown */
    int32_t supports_bc;                    /* MTLDevice.supportsBCTextureCompression: false on iPhone/iPad */
    int32_t unified_memory;
    int32_t is_simulator;
    uint64_t recommended_max_working_set;
    uint64_t physical_memory;
    uint8_t texture_format[32];
    uint32_t texture_format_count;
    uint32_t max_texture_size, max_anisotropy, max_samples;
    int32_t depth24_stencil8;               /* false: emulate with D32F + stencil */
    int32_t dxt_storage;                    /* how Direct3D BC textures should be stored: 0 native BC, 1 ASTC 4x4, 2 ASTC 6x6, 3 RGBA8 */
    uint64_t hard_memory_limit, soft_memory_limit, texture_budget;   /* process memory plan (bytes) */
} nue_ios_gpu_info;

typedef struct nue_ext_ios_v1 {
    uint32_t struct_size;
    nue_result (NUE_CALL *probe_gpu)(nue_ios_gpu_info* out);
    /* Prepares the CAMetalLayer bgfx will draw to (BGRA8, 3 drawables, vsync, opaque, native scale) and returns its size in pixels. */
    nue_result (NUE_CALL *configure_metal_layer)(void* ca_metal_layer, nue_bool framebuffer_only, uint32_t* out_width_px, uint32_t* out_height_px);
    /* What the app declares for Game Mode: bit 0 GCSupportsGameMode, bit 1 games category, bit 2 running on iOS 18+. The OS decides at run
     * time and offers no public query of the active state. */
    uint32_t (NUE_CALL *game_mode_declared)(void);
    int64_t (NUE_CALL *available_memory)(void);   /* os_proc_available_memory(); -1 unknown */
} nue_ext_ios_v1;

#ifdef __cplusplus
}
#endif
#endif
