/*
 * nue_ext_android.h - Android-only extension table of the NUE engine ("nue.android.v1").
 *
 *   const nue_ext_android_v1* ext = (const nue_ext_android_v1*)nue_get_extension(NUE_EXT_ANDROID_V1);
 *   if (ext) { NueAndroidGpuInfo g = {sizeof g}; ext->get_gpu_info(&g); ... }
 *
 * Everything the portable API (the headers in include/nue) cannot express: GPU probe and DXT policy, process memory budget, game-mode
 * and display details, rotation vector, audio focus state, the raw Android capability report.  The functions are the same
 * nue_android_* symbols declared in nue_android.h, exposed as a table so games do not link against platform symbols.
 * The first member is struct_size, new members are only appended.
 */
#ifndef NUE_EXT_ANDROID_H
#define NUE_EXT_ANDROID_H

#include "nue_android.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NUE_EXT_ANDROID_V1 "nue.android.v1"

typedef struct nue_ext_android_v1 {
    uint32_t struct_size;
    int (*get_device_info)(NueAndroidDeviceInfo *out);
    int (*get_display_info)(NueAndroidDisplayInfo *out);
    void (*probe_gpu)(void);
    int (*get_gpu_info)(NueAndroidGpuInfo *out);
    int (*recommend_dxt)(NueAndroidDxtStrategy *out_strategy);
    int (*get_memory_budget)(NueAndroidMemoryBudget *out);
    void (*set_memory_cap)(uint64_t bytes);
    void (*set_preferred_refresh)(float target_fps);
    void (*set_immersive)(int on);
    int (*rotation_available)(void);
    void (*enable_rotation)(int on);
    int (*get_rotation)(float quat_xyzw[4], int64_t *timestamp_ns);
    int (*get_audio_info)(int desired_rate, int target_latency_ms, NueAndroidAudioInfo *out);
    void (*audio_focus)(int request);
    int (*audio_focus_state)(void); /* 0 full volume, 1 ducked, 2 paused by a focus loss */
    int (*get_storage_paths)(NueAndroidStoragePaths *out);
    int (*get_power_info)(NueAndroidPowerInfo *out);
    void (*trace_begin)(const char *name);
    void (*trace_end)(void);
    void (*trace_counter)(const char *name, int64_t value);
    void (*dump_capabilities)(void);
} nue_ext_android_v1;

#ifdef __cplusplus
}
#endif

#endif /* NUE_EXT_ANDROID_H */
