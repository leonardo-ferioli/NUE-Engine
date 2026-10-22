/*
 * NUE engine - software mixer: voices, buses, listener. Designed so that a DirectSound compatibility layer (primary +
 * secondary buffers, play/write cursors, SetFrequency/SetVolume/SetPan, 3D buffers) maps onto it one to one, but nothing here
 * is DirectSound specific.
 *
 * Model
 *   mixer     owns the mix thread/callback, the output stream (or is pulled manually with nue_mixer_render for tests/offline),
 *             a master bus and any number of sub buses. Internal format: float32, the mixer's channel count and sample rate.
 *   buffer    immutable sample data (nue_buffer_create) that any number of STATIC voices can play.
 *   voice     one playing sound. Kinds:
 *               STATIC    plays a buffer (one-shot or looping, optional loop sub-range)
 *               RING      owns a circular buffer the application writes into at arbitrary positions while it plays (the streaming
 *                         pattern of DirectSound secondary buffers); play cursor and write cursor are readable
 *               CALLBACK  pulls frames from an application callback [audio]
 *   bus       a group: volume, mute, optional effect chain; voices are routed to a bus; buses route to a parent (0 = master)
 *   listener  position/orientation/velocity for 3D voices (attenuation, panning, Doppler).
 *
 * Gain model: voice gain = voice volume * (3D attenuation if 3D) * bus volume(s) * master. Volume is linear amplitude; helper
 * nue_db_to_linear/nue_linear_to_db convert from the 1/100 dB units DirectSound uses (see nue_centibels_to_linear).
 * Pan -1 (left) .. 1 (right) with a constant-power law (-3 dB at centre per channel); for >2 channel output the pan acts on the
 * front pair. Pitch: playback rate ratio (1.0 = native) or an absolute sample rate (nue_voice_set_frequency); resampling is
 * linear interpolation, with the ratio smoothed per block (no zipper noise).
 *
 * Thread: everything is [any]; commands are applied at the next mix block boundary (<= block_frames) in call order.
 * Functions marked [audio] are lock free and may be called from the mix thread / from a stream callback.
 * Ids (voice, buffer, bus) are 32-bit index+generation values; stale ids return NUE_ERR_INVALID_HANDLE.
 */
#ifndef NUE_MIXER_H
#define NUE_MIXER_H

#include "nue/base.h"
#include "nue/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

NUE_DEFINE_HANDLE(nue_mixer);
typedef uint32_t nue_voice_id;          /* 0 = invalid */
typedef uint32_t nue_buffer_id;         /* 0 = invalid */
typedef uint32_t nue_bus_id;            /* 0 = the master bus */

/* ---------------------------------------------------------------------------- mixer ---- */

typedef uint32_t nue_mixer_flags;
enum {
    NUE_MIXER_MANUAL = 1u << 0,         /* no output stream: the app (or a test) calls nue_mixer_render() */
    NUE_MIXER_START_PAUSED = 1u << 1
};

typedef struct nue_mixer_desc {
    uint32_t struct_size;
    nue_mixer_flags flags;
    uint32_t sample_rate;               /* 0 = device preferred (48000 in manual mode) */
    uint32_t channels;                  /* 1..8, 0 = 2 */
    uint32_t max_voices;                /* 0 = 128 */
    uint32_t block_frames;              /* mixing granularity in frames, 0 = 256; also the command latency */
    nue_audio_device_id device;         /* output device (0 = default); ignored in manual mode */
    uint32_t latency_ms;                /* passed to the output stream, 0 = default */
    uint32_t reserved[4];
} nue_mixer_desc;

typedef struct nue_mixer_stats {
    uint32_t struct_size;
    uint32_t active_voices;
    uint32_t virtualized_voices;        /* voices over max_voices that were created inactive and skipped (lowest priority first) */
    uint64_t blocks_mixed;
    uint64_t clipped_samples;           /* output samples beyond +-1 since reset */
    float peak[NUE_AUDIO_MAX_CHANNELS]; /* peak per output channel over the last block */
    float cpu_load;                     /* mix time / block duration (rolling), 0..1 */
    uint32_t reserved[4];
} nue_mixer_stats;

NUE_API nue_result NUE_CALL nue_mixer_create(const nue_mixer_desc* desc, nue_mixer* out_mixer);
NUE_API void NUE_CALL nue_mixer_destroy(nue_mixer mixer);
NUE_API nue_result NUE_CALL nue_mixer_get_spec(nue_mixer mixer, nue_audio_spec* out_spec);      /* the float format of nue_mixer_render */
NUE_API nue_result NUE_CALL nue_mixer_start(nue_mixer mixer);
NUE_API nue_result NUE_CALL nue_mixer_stop(nue_mixer mixer);                                    /* pause the output; voices keep their state */
/* Mix `frame_count` frames of interleaved float32 into `out_f32` (mixer channels). [audio] Safe from any one thread at a time;
 * in non-manual mode it fails with NUE_ERR_BAD_STATE. Deterministic: same commands + same calls => bit-identical output. */
NUE_API nue_result NUE_CALL nue_mixer_render(nue_mixer mixer, float* out_f32, uint32_t frame_count);
NUE_API nue_result NUE_CALL nue_mixer_set_master_volume(nue_mixer mixer, float volume);
NUE_API float NUE_CALL nue_mixer_get_master_volume(nue_mixer mixer);
NUE_API nue_result NUE_CALL nue_mixer_get_stats(nue_mixer mixer, nue_mixer_stats* out);
NUE_API nue_result NUE_CALL nue_mixer_reset_stats(nue_mixer mixer);
/* The output stream underneath (NULL in manual mode), e.g. for latency queries. */
NUE_API nue_audio_stream NUE_CALL nue_mixer_get_stream(nue_mixer mixer);

/* ---------------------------------------------------------------------------- units ---- */

NUE_API float NUE_CALL nue_db_to_linear(float db);
NUE_API float NUE_CALL nue_linear_to_db(float linear);                  /* -inf clamps to -200 dB */
NUE_API float NUE_CALL nue_centibels_to_linear(int32_t hundredths_of_db);   /* DirectSound volume units: 0 = full, -10000 = silent */
NUE_API int32_t NUE_CALL nue_linear_to_centibels(float linear);
NUE_API float NUE_CALL nue_pan_from_centibels(int32_t pan);             /* DirectSound pan -10000..10000 -> -1..1 (logarithmic attenuation of the opposite side) */

/* ---------------------------------------------------------------------------- buses and effects ---- */

typedef enum nue_effect_kind {
    NUE_EFFECT_NONE = 0,
    NUE_EFFECT_LOWPASS,                 /* params[0] = cutoff Hz, params[1] = Q (0.707) */
    NUE_EFFECT_HIGHPASS,                /* same */
    NUE_EFFECT_ECHO,                    /* params: delay ms, feedback 0..0.95, wet 0..1 */
    NUE_EFFECT_REVERB,                  /* params: room size 0..1, damping 0..1, wet 0..1 */
    NUE_EFFECT_COMPRESSOR,              /* params: threshold dB, ratio, attack ms, release ms, makeup dB */
    NUE_EFFECT_CALLBACK                 /* application DSP */
} nue_effect_kind;

/* [audio] In-place processing of interleaved float32 frames. */
typedef void (NUE_CALL *nue_dsp_callback)(void* userdata, float* samples, uint32_t frame_count, uint32_t channels, uint32_t sample_rate);

typedef struct nue_effect_desc {
    uint32_t struct_size;
    nue_effect_kind kind;
    float params[8];
    nue_dsp_callback callback;          /* CALLBACK */
    void* userdata;
    float wet_dry;                      /* 0..1 mix of processed signal; 1 = fully processed (default when 0 is not meaningful: use 1.0f) */
} nue_effect_desc;

#define NUE_BUS_MAX_EFFECTS 4

NUE_API nue_result NUE_CALL nue_bus_create(nue_mixer mixer, nue_bus_id parent, nue_bus_id* out_bus);
NUE_API nue_result NUE_CALL nue_bus_destroy(nue_mixer mixer, nue_bus_id bus);                   /* voices on it move to the parent */
NUE_API nue_result NUE_CALL nue_bus_set_volume(nue_mixer mixer, nue_bus_id bus, float volume);
NUE_API nue_result NUE_CALL nue_bus_set_mute(nue_mixer mixer, nue_bus_id bus, nue_bool mute);
NUE_API nue_result NUE_CALL nue_bus_set_effect(nue_mixer mixer, nue_bus_id bus, uint32_t slot, const nue_effect_desc* effect_or_null);

/* ---------------------------------------------------------------------------- buffers ---- */

typedef struct nue_buffer_desc {
    uint32_t struct_size;
    nue_audio_spec spec;                /* S16, F32 or U8; any channel count 1..8 */
    const void* data;                   /* frames * frame_size bytes */
    uint32_t frame_count;
    nue_bool copy_data;                 /* NUE_TRUE: the engine copies; NUE_FALSE: data must stay valid until the buffer is destroyed */
    uint32_t loop_start, loop_end;      /* frames; loop_end == 0 -> whole buffer */
    const char* debug_name;
} nue_buffer_desc;

NUE_API nue_result NUE_CALL nue_buffer_create(nue_mixer mixer, const nue_buffer_desc* desc, nue_buffer_id* out_buffer);
/* Voices already playing the buffer keep it alive until they stop. */
NUE_API nue_result NUE_CALL nue_buffer_destroy(nue_mixer mixer, nue_buffer_id buffer);

/* ---------------------------------------------------------------------------- voices ---- */

typedef enum nue_voice_kind { NUE_VOICE_STATIC = 1, NUE_VOICE_RING, NUE_VOICE_CALLBACK } nue_voice_kind;

typedef uint32_t nue_voice_flags;
enum {
    NUE_VOICE_LOOP          = 1u << 0,   /* STATIC: loop the loop range (or whole buffer). RING: always loops; flag ignored */
    NUE_VOICE_AUTO_RELEASE  = 1u << 1,   /* the voice id becomes invalid when playback ends (fire and forget) */
    NUE_VOICE_3D            = 1u << 2,   /* positioned relative to the listener; pan comes from position, nue_voice_set_pan is added on top */
    NUE_VOICE_HEAD_RELATIVE = 1u << 3,   /* 3D position is relative to the listener (position (0,0,0) = on the head) */
    NUE_VOICE_NO_PITCH      = 1u << 4,   /* disallows frequency changes (lets the mixer skip the resampler) */
    NUE_VOICE_START_PLAYING = 1u << 5
};

typedef void (NUE_CALL *nue_voice_end_callback)(void* userdata, nue_voice_id voice);   /* [audio] playback finished; do not call the engine except to start another voice */

typedef struct nue_voice_desc {
    uint32_t struct_size;
    nue_voice_kind kind;
    nue_voice_flags flags;
    nue_bus_id bus;
    int32_t priority;                    /* higher survives voice stealing/virtualisation; default 0 */
    /* STATIC */
    nue_buffer_id buffer;
    /* RING and CALLBACK: the format of the data the application provides */
    nue_audio_spec spec;
    uint32_t ring_frames;                /* RING: size of the circular buffer in frames */
    /* CALLBACK: [audio] fill frame_count frames of `spec`; return the frames actually produced (fewer = the voice ends) */
    uint32_t (NUE_CALL *pull)(void* userdata, void* out, uint32_t frame_count);
    void* userdata;
    nue_voice_end_callback on_end;
    float volume;                        /* initial; 0 is treated as unset -> 1.0. Use nue_voice_set_volume(0) afterwards to mute */
    float pan;
    float pitch;                         /* 0 = 1.0 */
    const char* debug_name;
} nue_voice_desc;

typedef struct nue_voice_state {
    uint32_t struct_size;
    nue_bool playing;
    nue_bool paused;
    nue_bool looping;
    uint64_t position_frames;            /* STATIC: frame in the buffer; RING: ring play cursor; CALLBACK: frames pulled */
    uint64_t frames_played_total;        /* source frames consumed since start (monotonic, counts loops) */
    float volume;
    float pan;
    float pitch;
    float effective_gain;                /* last mixed gain incl. 3D and buses */
} nue_voice_state;

NUE_API nue_result NUE_CALL nue_voice_create(nue_mixer mixer, const nue_voice_desc* desc, nue_voice_id* out_voice);
NUE_API nue_result NUE_CALL nue_voice_destroy(nue_mixer mixer, nue_voice_id voice);
NUE_API nue_result NUE_CALL nue_voice_play(nue_mixer mixer, nue_voice_id voice);                  /* from the current position; restarts a stopped STATIC voice at 0 (or loop start) */
NUE_API nue_result NUE_CALL nue_voice_stop(nue_mixer mixer, nue_voice_id voice);                  /* STATIC: position reset to 0. RING: position kept (DirectSound Stop semantics) */
NUE_API nue_result NUE_CALL nue_voice_pause(nue_mixer mixer, nue_voice_id voice, nue_bool paused);
NUE_API nue_result NUE_CALL nue_voice_set_looping(nue_mixer mixer, nue_voice_id voice, nue_bool looping);
NUE_API nue_result NUE_CALL nue_voice_set_volume(nue_mixer mixer, nue_voice_id voice, float volume);   /* [audio] */
NUE_API nue_result NUE_CALL nue_voice_set_pan(nue_mixer mixer, nue_voice_id voice, float pan);         /* [audio] */
NUE_API nue_result NUE_CALL nue_voice_set_pitch(nue_mixer mixer, nue_voice_id voice, float ratio);     /* [audio] 0.01 .. 100 */
NUE_API nue_result NUE_CALL nue_voice_set_frequency(nue_mixer mixer, nue_voice_id voice, uint32_t hz); /* 0 = native rate of the source */
NUE_API nue_result NUE_CALL nue_voice_set_bus(nue_mixer mixer, nue_voice_id voice, nue_bus_id bus);
/* Seek (STATIC: frame; RING: play cursor). Takes effect at the next block. */
NUE_API nue_result NUE_CALL nue_voice_set_position(nue_mixer mixer, nue_voice_id voice, uint64_t frame);
NUE_API nue_result NUE_CALL nue_voice_get_state(nue_mixer mixer, nue_voice_id voice, nue_voice_state* out);
NUE_API nue_bool NUE_CALL nue_voice_is_playing(nue_mixer mixer, nue_voice_id voice);               /* NUE_FALSE for stale ids */

/* RING voices ------------------------------------------------------------------------------------------------------------
 * The application writes frames at an offset into the ring while the voice plays. Writes wrap around the end. A write that
 * overlaps the region currently being played is allowed (the app is responsible, as with DirectSound Lock). */
NUE_API nue_result NUE_CALL nue_voice_ring_write(nue_mixer mixer, nue_voice_id voice, uint32_t offset_frames, const void* data, uint32_t frame_count);
NUE_API nue_result NUE_CALL nue_voice_ring_fill_silence(nue_mixer mixer, nue_voice_id voice, uint32_t offset_frames, uint32_t frame_count);
/* Cursors in frames of the ring. play = the frame being heard now (compensating output latency when known); write = the first
 * frame that is safe to overwrite (play + the mixer's pending block + device latency). Both refresh once per mixed block. */
NUE_API nue_result NUE_CALL nue_voice_ring_get_cursors(nue_mixer mixer, nue_voice_id voice, uint32_t* out_play_frames, uint32_t* out_write_frames);
NUE_API nue_result NUE_CALL nue_voice_ring_get_size(nue_mixer mixer, nue_voice_id voice, uint32_t* out_frames);

/* 3D --------------------------------------------------------------------------------------------------------------------- */

typedef struct nue_listener {
    uint32_t struct_size;
    nue_vec3 position;
    nue_vec3 velocity;
    nue_vec3 forward;                    /* unit */
    nue_vec3 up;                         /* unit */
} nue_listener;

typedef struct nue_world_3d {
    uint32_t struct_size;
    float doppler_factor;                /* 0 off, 1 = real; default 1 */
    float speed_of_sound;                /* world units / s, default 343 */
    float rolloff_factor;                /* global multiplier of per-voice rolloff, default 1 */
    float distance_factor;               /* world units per meter, default 1 */
} nue_world_3d;

typedef enum nue_attenuation_model { NUE_ATTEN_INVERSE = 0, NUE_ATTEN_LINEAR, NUE_ATTEN_EXPONENTIAL, NUE_ATTEN_NONE } nue_attenuation_model;

typedef struct nue_voice_3d {
    uint32_t struct_size;
    nue_vec3 position;
    nue_vec3 velocity;
    float min_distance;                  /* full volume inside; default 1 */
    float max_distance;                  /* no further attenuation (and linear model reaches silence) beyond; default 1e9 */
    float rolloff;                       /* per-voice factor, default 1 */
    nue_attenuation_model model;
    nue_vec3 cone_direction;             /* zero vector = omnidirectional */
    float cone_inner_angle_deg, cone_outer_angle_deg, cone_outer_gain;
} nue_voice_3d;

NUE_API nue_result NUE_CALL nue_mixer_set_listener(nue_mixer mixer, const nue_listener* listener);
NUE_API nue_result NUE_CALL nue_mixer_set_world_3d(nue_mixer mixer, const nue_world_3d* world);
NUE_API nue_result NUE_CALL nue_voice_set_3d(nue_mixer mixer, nue_voice_id voice, const nue_voice_3d* params);   /* [audio] */

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_MIXER_H */
