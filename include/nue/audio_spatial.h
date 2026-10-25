/*
 * NUE engine - spatial rendering stage (module `nue_audio`, library nue::audio, header not included by nue.h).
 *
 * A spatial renderer takes a 3D scene (listener pose, per-source position, velocity, cone, distance model, occlusion) and writes the mix
 * of its mono sources into a speaker layout:
 *
 *   NUE_SPATIAL_PAN        amplitude panning: sine-law stereo, pairwise (2D VBAP) panning on 4 / 5.1 / 7.1 / any mask with side and rear speakers
 *   NUE_SPATIAL_BINAURAL   stereo headphones: spherical head model (Woodworth interaural delay, head shadow, rear cue). A measured HRTF set
 *                          (SOFA) plugs in through nue_spatial_set_hrtf; the built-in model is the fallback
 *   NUE_SPATIAL_AMBISONIC  encodes into ambisonics (ACN order, SN3D, orders 1..3) and decodes to the layout with a sampling decoder
 *   backend                an operating system spatializer (Windows Sonic / Dolby Atmos for Headphones, Android 13 Spatializer, Apple PHASE) or
 *                          an object-audio renderer (ADM, IAMF) registered through nue_spatial_backend_ops
 *
 * Coordinates are left-handed like the mixer (mixer.h): +x right, +y up, +z forward for the default listener. The scene maths is also
 * available alone (nue_spatial_evaluate) for legacy layers that keep driving the mixer's own voices (DirectSound 3D buffers map to it
 * field by field: min/max distance, cone angles and outer volume, Doppler).
 *
 * Threading: like the DSP module. Rendering functions never allocate or lock; the scene is passed by value at every call, so there is no
 * shared state to protect. A source object is used by one thread at a time.
 */
#ifndef NUE_AUDIO_SPATIAL_H
#define NUE_AUDIO_SPATIAL_H

#include "nue/base.h"
#include "nue/audio.h"
#include "nue/mixer.h"
#include "nue/audio_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nue_spatial_mode {
    NUE_SPATIAL_PAN = 0,
    NUE_SPATIAL_BINAURAL,
    NUE_SPATIAL_AMBISONIC
} nue_spatial_mode;

typedef struct nue_spatial_world {
    uint32_t struct_size;
    nue_listener listener;              /* mixer.h: position, velocity, forward, up (unit) */
    float speed_of_sound;               /* world units per second, default 343 */
    float doppler_factor;               /* 0 off, 1 real */
    float rolloff_factor;               /* global multiplier of per-source rolloff */
    float distance_factor;              /* world units per meter (air absorption works in meters) */
} nue_spatial_world;

typedef struct nue_spatial_source_params {
    uint32_t struct_size;
    nue_vec3 position, velocity;
    nue_vec3 cone_direction;            /* zero = omnidirectional */
    float cone_inner_deg, cone_outer_deg, cone_outer_gain;     /* inside inner: gain 1; outside outer: cone_outer_gain; linear in dB between */
    float min_distance, max_distance, rolloff;
    nue_attenuation_model model;        /* mixer.h */
    float volume;                       /* linear, 0 is treated as 1 */
    float occlusion;                    /* 0 open .. 1 fully blocked: gain drops up to 40 dB and high frequencies up to 100 % */
    float spread_deg;                   /* 0 point source .. 180 fills all speakers equally */
    uint32_t flags;
} nue_spatial_source_params;
enum { NUE_SPATIAL_HEAD_RELATIVE = 1u << 0, NUE_SPATIAL_AIR_ABSORPTION = 1u << 1 };

/* What the scene maths says about one source. */
typedef struct nue_spatial_eval {
    uint32_t struct_size;
    float distance;                     /* meters-or-units between listener and source */
    float azimuth_deg;                  /* -180..180, 0 front, + right */
    float elevation_deg;                /* -90..90 */
    float distance_gain;                /* distance model */
    float cone_gain;
    float occlusion_gain;
    float total_gain;                   /* volume * distance * cone * occlusion */
    float doppler_ratio;                /* playback rate multiplier (1 = none) */
    nue_vec3 direction;                 /* unit vector to the source in listener space (x right, y up, z forward) */
} nue_spatial_eval;

NUE_API void NUE_CALL nue_spatial_world_default(nue_spatial_world* w);
NUE_API void NUE_CALL nue_spatial_source_params_default(nue_spatial_source_params* p);
NUE_API nue_result NUE_CALL nue_spatial_evaluate(const nue_spatial_world* world, const nue_spatial_source_params* src, nue_spatial_eval* out);

/* ---------------------------------------------------------------------------- renderer */

NUE_DEFINE_HANDLE(nue_spatial);
NUE_DEFINE_HANDLE(nue_spatial_source);

/* An external renderer. `render` receives the evaluated sources of one block and writes the mix into out (interleaved float32, the layout
 * the renderer was created with, ACCUMULATING). Sources arrive as mono blocks. Return NUE_OK, or an error to fall back to the built-in mode. */
typedef struct nue_spatial_backend_source {
    const float* mono;                  /* frames samples */
    nue_spatial_eval eval;
    nue_spatial_source_params params;
} nue_spatial_backend_source;
typedef struct nue_spatial_backend_ops {
    uint32_t struct_size;
    const char* name;                   /* e.g. "windows-sonic", "phase", "adm" */
    nue_result (NUE_CALL *render)(void* userdata, const nue_spatial_world* world, const nue_spatial_backend_source* sources, uint32_t count,
                                  uint32_t frames, float* out);
    void (NUE_CALL *destroy)(void* userdata);
    void* userdata;
} nue_spatial_backend_ops;

typedef struct nue_spatial_desc {
    uint32_t struct_size;
    nue_spatial_mode mode;
    nue_channel_mask mask;              /* output layout, 0 = stereo */
    uint32_t sample_rate;
    uint32_t ambisonic_order;           /* 1..3 (AMBISONIC), default 1 */
    uint32_t max_block_frames;          /* 0 = 4096 */
    const nue_spatial_backend_ops* backend;   /* optional, takes precedence over mode while it succeeds */
} nue_spatial_desc;

NUE_API nue_result NUE_CALL nue_spatial_create(const nue_spatial_desc* desc, nue_spatial* out);
NUE_API void NUE_CALL nue_spatial_destroy(nue_spatial s);
NUE_API uint32_t NUE_CALL nue_spatial_output_channels(nue_spatial s);

/* Per-source state (delay lines, smoothed gains). */
NUE_API nue_result NUE_CALL nue_spatial_source_create(nue_spatial s, nue_spatial_source* out);
NUE_API void NUE_CALL nue_spatial_source_destroy(nue_spatial_source src);

/* Start of a block: clears the output. Then call nue_spatial_render_source for every source and nue_spatial_end_block. With a backend the
 * sources are collected and handed over at the end. `out` must hold frames * output_channels floats and stay valid until end_block. */
NUE_API nue_result NUE_CALL nue_spatial_begin_block(nue_spatial s, const nue_spatial_world* world, float* out, uint32_t frames);
NUE_API nue_result NUE_CALL nue_spatial_render_source(nue_spatial_source src, const nue_spatial_source_params* params, const float* mono);
NUE_API nue_result NUE_CALL nue_spatial_end_block(nue_spatial s);
/* Which mode actually rendered the last block (a failing backend falls back). */
NUE_API const char* NUE_CALL nue_spatial_active_renderer(nue_spatial s);

/* Measured HRTFs: pairs of impulse responses per direction (left, right), as extracted from a SOFA file by a loader (libmysofa or the tool).
 * azimuths / elevations in degrees, irs = count * 2 * ir_frames floats (left then right per direction). Replaces the spherical head model. */
NUE_API nue_result NUE_CALL nue_spatial_set_hrtf(nue_spatial s, const float* azimuth_deg, const float* elevation_deg, const float* irs,
                                                 uint32_t direction_count, uint32_t ir_frames, uint32_t ir_sample_rate);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_AUDIO_SPATIAL_H */
