/*
 * NUE engine - audio DSP (module `nue_audio`, library nue::audio, header not included by nue.h).
 *
 *   resampler        windowed-sinc polyphase sample-rate converter, four quality levels, streaming, time-varying ratio
 *   channel mixer    layout conversion by speaker mask (BS.775 style down mixes, constant power up mixes)
 *   convert_hq       format + layout + rate conversion in one call (nue_audio_convert is the engine's cheap converter)
 *   dynamics         compressor (soft knee, linked), look-ahead brick-wall limiter, dynamic range modes for speakers / headphones / TV
 *   loudness         EBU R128 / ITU-R BS.1770 meter (momentary, short term, integrated, range, sample and true peak) and normalisation
 *   reverb           8 line feedback delay network + early reflections with the EFX / EAX / I3DL2 parameter model, and a partitioned
 *                    FFT convolution reverb for measured impulse responses
 *
 * Every processor works on interleaved float32 frames and can be used in three ways: directly (…_process), as a mixer bus effect (…_dsp
 * is a `nue_dsp_callback`: set NUE_EFFECT_CALLBACK with callback = nue_xxx_dsp, userdata = the handle), or offline from a tool.
 *
 * Threading: create / destroy / reset are [any] but not concurrent with process on the same object. Parameter setters are real-time safe
 * (lock free, take effect at the next process call) and may be called from any thread while another thread processes. process / *_dsp
 * never allocate, lock or call the engine.
 */
#ifndef NUE_AUDIO_DSP_H
#define NUE_AUDIO_DSP_H

/* Version of the audio API group (audio_codec.h, audio_dsp.h, audio_spatial.h share it); see docs/ABI.md 3.2. */
#ifndef NUE_AUDIO_API_VERSION
#define NUE_AUDIO_API_VERSION 1u
#endif

#include "nue/base.h"
#include "nue/audio.h"
#include "nue/mixer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================================ channel masks */

/* Speaker bits as in WAVEFORMATEXTENSIBLE.dwChannelMask. Channels are interleaved in ascending bit order, so mask 0x3F (5.1) is
 * FL FR FC LFE BL BR - the order of nue_audio_spec. */
typedef uint32_t nue_channel_mask;
enum {
    NUE_SPK_FL = 0x1, NUE_SPK_FR = 0x2, NUE_SPK_FC = 0x4, NUE_SPK_LFE = 0x8, NUE_SPK_BL = 0x10, NUE_SPK_BR = 0x20,
    NUE_SPK_FLC = 0x40, NUE_SPK_FRC = 0x80, NUE_SPK_BC = 0x100, NUE_SPK_SL = 0x200, NUE_SPK_SR = 0x400,
    NUE_SPK_TFL = 0x1000, NUE_SPK_TFC = 0x2000, NUE_SPK_TFR = 0x4000, NUE_SPK_TBL = 0x8000, NUE_SPK_TBC = 0x10000, NUE_SPK_TBR = 0x20000
};
#define NUE_MASK_MONO     (NUE_SPK_FC)
#define NUE_MASK_STEREO   (NUE_SPK_FL | NUE_SPK_FR)
#define NUE_MASK_QUAD     (NUE_SPK_FL | NUE_SPK_FR | NUE_SPK_BL | NUE_SPK_BR)
#define NUE_MASK_5_0      (NUE_SPK_FL | NUE_SPK_FR | NUE_SPK_FC | NUE_SPK_BL | NUE_SPK_BR)
#define NUE_MASK_5_1      (NUE_MASK_5_0 | NUE_SPK_LFE)
#define NUE_MASK_6_1      (NUE_SPK_FL | NUE_SPK_FR | NUE_SPK_FC | NUE_SPK_LFE | NUE_SPK_BC | NUE_SPK_SL | NUE_SPK_SR)
#define NUE_MASK_7_1      (NUE_MASK_5_1 | NUE_SPK_SL | NUE_SPK_SR)

NUE_API uint32_t NUE_CALL nue_channel_mask_count(nue_channel_mask mask);
/* The layout the engine assumes for a plain channel count: 1 FC, 2 FL FR, 3 FL FR FC, 4 quad, 5 5.0, 6 5.1, 7 6.1, 8 7.1. */
NUE_API nue_channel_mask NUE_CALL nue_channel_mask_default(uint32_t channels);

/* ============================================================================================================ resampler */

typedef enum nue_resample_quality {
    NUE_RESAMPLE_FAST = 0,              /* 36 taps, 60 dB stop band, flat to 0.80 of Nyquist; for pitch shifting many voices */
    NUE_RESAMPLE_MEDIUM,                /* 68 taps, 80 dB, 0.85 */
    NUE_RESAMPLE_HIGH,                  /* 128 taps, 100 dB, 0.90 (default for loading assets) */
    NUE_RESAMPLE_BEST                   /* 260 taps, 120 dB, 0.94 */
} nue_resample_quality;

NUE_DEFINE_HANDLE(nue_resampler);

/* Taps are counted at the lower of the two rates, so the filter is longer in input samples when decimating. The ratio may be changed
 * while streaming (nue_resampler_set_rates): small changes glide, larger ones re-design the filter (history is kept). */
NUE_API nue_result NUE_CALL nue_resampler_create(uint32_t channels, double in_rate, double out_rate, nue_resample_quality quality, nue_resampler* out);
NUE_API void NUE_CALL nue_resampler_destroy(nue_resampler r);
NUE_API void NUE_CALL nue_resampler_reset(nue_resampler r);                                 /* clears history, back to the start of a stream */
NUE_API nue_result NUE_CALL nue_resampler_set_rates(nue_resampler r, double in_rate, double out_rate);
/* Streaming: consumes up to in_frames and produces up to out_capacity frames; *in_used / *out_frames report what happened. Call again
 * with the unused input when the output is full. Deterministic for the same call sequence. */
NUE_API nue_result NUE_CALL nue_resampler_process(nue_resampler r, const float* in, uint32_t in_frames, uint32_t* in_used,
                                                  float* out, uint32_t out_capacity, uint32_t* out_frames);
/* Pushes silence until the filter tail has been delivered (end of a stream). Returns frames written. */
NUE_API uint32_t NUE_CALL nue_resampler_flush(nue_resampler r, float* out, uint32_t out_capacity);
NUE_API uint32_t NUE_CALL nue_resampler_latency_frames(nue_resampler r);                    /* in INPUT frames: group delay of the filter */
NUE_API uint32_t NUE_CALL nue_resampler_output_frames_for(nue_resampler r, uint32_t in_frames);   /* estimate (+- 1) for buffer sizing */
/* One shot: converts a whole buffer. Compensates the filter delay, so the result is time aligned with the input and has
 * round(in_frames * out_rate / in_rate) frames. Returns frames written (<= out_capacity), 0 on error. */
NUE_API uint32_t NUE_CALL nue_resample_buffer(const float* in, uint32_t in_frames, uint32_t channels, double in_rate, double out_rate,
                                              nue_resample_quality quality, float* out, uint32_t out_capacity);

/* ============================================================================================================ channel mixer */

typedef uint32_t nue_chanmix_flags;
enum {
    NUE_CHANMIX_NORMALIZE   = 1u << 0,  /* scale each down mix so that no output can exceed full scale when all inputs do (default off: BS.775 gains as they are) */
    NUE_CHANMIX_INCLUDE_LFE = 1u << 1,  /* mix the LFE into the front channels at -10 dB instead of dropping it */
    NUE_CHANMIX_SURROUND_FILL = 1u << 2 /* up mix: derive rear speakers missing in the source from the fronts (-6 dB, de-correlated by polarity) */
};

NUE_DEFINE_HANDLE(nue_chanmix);

/* src/dst masks of 0 mean nue_channel_mask_default(channels) - pass the channel counts through *_channels in that case. */
NUE_API nue_result NUE_CALL nue_chanmix_create(nue_channel_mask src_mask, nue_channel_mask dst_mask, nue_chanmix_flags flags, nue_chanmix* out);
NUE_API void NUE_CALL nue_chanmix_destroy(nue_chanmix m);
NUE_API uint32_t NUE_CALL nue_chanmix_src_channels(nue_chanmix m);
NUE_API uint32_t NUE_CALL nue_chanmix_dst_channels(nue_chanmix m);
/* The dst x src matrix, row major (matrix[dst * src_channels + src]). */
NUE_API nue_result NUE_CALL nue_chanmix_get_matrix(nue_chanmix m, float* out, uint32_t capacity);
NUE_API void NUE_CALL nue_chanmix_process(nue_chanmix m, const float* in, uint32_t frames, float* out);   /* in and out must not overlap */

/* Format + layout + rate conversion. Masks of 0 = default layout of the spec's channel count. quality is used when the rates differ.
 * Returns frames written, 0 on error. Compare nue_audio_convert (audio.h): linear interpolation, fixed matrices. */
NUE_API uint32_t NUE_CALL nue_audio_convert_hq(const nue_audio_spec* src_spec, nue_channel_mask src_mask, const void* src, uint32_t src_frames,
                                               const nue_audio_spec* dst_spec, nue_channel_mask dst_mask, void* dst, uint32_t dst_capacity_frames,
                                               nue_resample_quality quality, nue_chanmix_flags mix_flags);

/* ============================================================================================================ dynamics */

NUE_DEFINE_HANDLE(nue_compressor);

typedef struct nue_compressor_desc {
    uint32_t struct_size;
    uint32_t channels;
    uint32_t sample_rate;
    float threshold_db;                 /* -60..0, default -18 */
    float ratio;                        /* 1..100 (100 ~ limiting), default 3 */
    float knee_db;                      /* 0 = hard knee, default 6 */
    float attack_ms;                    /* 0.1..500, default 10 */
    float release_ms;                   /* 1..5000, default 120 */
    float makeup_db;                    /* -24..48, default 0; NUE_COMPRESSOR_AUTO_MAKEUP overrides */
    float link;                         /* 0..1: how much every channel follows the loudest one (default 1: one gain for all channels) */
    uint32_t flags;
} nue_compressor_desc;
enum { NUE_COMPRESSOR_AUTO_MAKEUP = 1u << 0, NUE_COMPRESSOR_RMS_DETECT = 1u << 1 };

NUE_API void NUE_CALL nue_compressor_desc_default(nue_compressor_desc* desc, uint32_t channels, uint32_t sample_rate);
NUE_API nue_result NUE_CALL nue_compressor_create(const nue_compressor_desc* desc, nue_compressor* out);
NUE_API void NUE_CALL nue_compressor_destroy(nue_compressor c);
NUE_API void NUE_CALL nue_compressor_reset(nue_compressor c);
NUE_API nue_result NUE_CALL nue_compressor_set_params(nue_compressor c, const nue_compressor_desc* desc);   /* real-time safe; channels / rate cannot change */
NUE_API void NUE_CALL nue_compressor_process(nue_compressor c, float* samples, uint32_t frames);            /* in place, interleaved */
NUE_API float NUE_CALL nue_compressor_gain_reduction_db(nue_compressor c);                                   /* last block, <= 0 */
NUE_API void NUE_CALL nue_compressor_dsp(void* handle, float* samples, uint32_t frames, uint32_t channels, uint32_t sample_rate);   /* nue_dsp_callback */

NUE_DEFINE_HANDLE(nue_limiter);

typedef struct nue_limiter_desc {
    uint32_t struct_size;
    uint32_t channels;
    uint32_t sample_rate;
    float ceiling_db;                   /* output never exceeds this (sample peak, or true peak with the flag), default -1 */
    float lookahead_ms;                 /* 0.5..20, default 5; the limiter delays the signal by this much */
    float release_ms;                   /* 1..2000, default 80 */
    uint32_t flags;
} nue_limiter_desc;
enum { NUE_LIMITER_TRUE_PEAK = 1u << 0 };   /* detect inter-sample peaks (4x interpolation); the ceiling then holds for the reconstructed signal within ~0.2 dB */

NUE_API void NUE_CALL nue_limiter_desc_default(nue_limiter_desc* desc, uint32_t channels, uint32_t sample_rate);
NUE_API nue_result NUE_CALL nue_limiter_create(const nue_limiter_desc* desc, nue_limiter* out);
NUE_API void NUE_CALL nue_limiter_destroy(nue_limiter l);
NUE_API void NUE_CALL nue_limiter_reset(nue_limiter l);
NUE_API nue_result NUE_CALL nue_limiter_set_params(nue_limiter l, const nue_limiter_desc* desc);           /* ceiling and release only; real-time safe */
NUE_API void NUE_CALL nue_limiter_process(nue_limiter l, float* samples, uint32_t frames);                  /* in place */
NUE_API uint32_t NUE_CALL nue_limiter_latency_frames(nue_limiter l);
NUE_API float NUE_CALL nue_limiter_gain_reduction_db(nue_limiter l);
NUE_API void NUE_CALL nue_limiter_dsp(void* handle, float* samples, uint32_t frames, uint32_t channels, uint32_t sample_rate);

/* Dynamic range modes: a compressor + limiter pair tuned for where the sound ends up. Modes follow the usual broadcast / game practice:
 * speakers keep the full range, headphones add a light safety limiter, TV (late night) squeezes the range for quiet living rooms. */
typedef enum nue_drc_mode {
    NUE_DRC_OFF = 0,
    NUE_DRC_SPEAKERS,                   /* threshold -20 dB, 2:1, limiter -1 dBTP */
    NUE_DRC_HEADPHONES,                 /* threshold -16 dB, 1.5:1, limiter -2 dBTP, true peak (listening up close) */
    NUE_DRC_TV,                         /* threshold -30 dB, 4:1, 8 dB knee, limiter -3 dBTP: quiet scenes stay audible, explosions do not wake the house */
    NUE_DRC_NIGHT                       /* threshold -38 dB, 6:1, auto make-up, limiter -6 dBTP */
} nue_drc_mode;

NUE_DEFINE_HANDLE(nue_drc);
NUE_API nue_result NUE_CALL nue_drc_create(uint32_t channels, uint32_t sample_rate, nue_drc_mode mode, nue_drc* out);
NUE_API void NUE_CALL nue_drc_destroy(nue_drc d);
NUE_API nue_result NUE_CALL nue_drc_set_mode(nue_drc d, nue_drc_mode mode);        /* real-time safe */
NUE_API void NUE_CALL nue_drc_process(nue_drc d, float* samples, uint32_t frames);
NUE_API uint32_t NUE_CALL nue_drc_latency_frames(nue_drc d);
NUE_API void NUE_CALL nue_drc_dsp(void* handle, float* samples, uint32_t frames, uint32_t channels, uint32_t sample_rate);

/* ============================================================================================================ loudness */

NUE_DEFINE_HANDLE(nue_loudness);

typedef uint32_t nue_loudness_modes;
enum {
    NUE_LOUDNESS_MOMENTARY  = 1u << 0,  /* 400 ms */
    NUE_LOUDNESS_SHORT_TERM = 1u << 1,  /* 3 s */
    NUE_LOUDNESS_INTEGRATED = 1u << 2,  /* gated programme loudness */
    NUE_LOUDNESS_RANGE      = 1u << 3,  /* LRA */
    NUE_LOUDNESS_SAMPLE_PEAK= 1u << 4,
    NUE_LOUDNESS_TRUE_PEAK  = 1u << 5,
    NUE_LOUDNESS_ALL        = 0x3F
};

/* mask 0 = default layout. Surround, back and side channels use the BS.1770 surround weighting, LFE is excluded. */
NUE_API nue_result NUE_CALL nue_loudness_create(uint32_t channels, nue_channel_mask mask, uint32_t sample_rate, nue_loudness_modes modes, nue_loudness* out);
NUE_API void NUE_CALL nue_loudness_destroy(nue_loudness m);
NUE_API void NUE_CALL nue_loudness_reset(nue_loudness m);
NUE_API nue_result NUE_CALL nue_loudness_add(nue_loudness m, const float* interleaved, uint32_t frames);
/* Values in LUFS (-HUGE_VAL when there is not enough signal yet) and dB; peaks are linear amplitudes (max over channels, since reset). */
NUE_API double NUE_CALL nue_loudness_momentary(nue_loudness m);
NUE_API double NUE_CALL nue_loudness_short_term(nue_loudness m);
NUE_API double NUE_CALL nue_loudness_integrated(nue_loudness m);
NUE_API double NUE_CALL nue_loudness_range(nue_loudness m);                          /* LU */
NUE_API double NUE_CALL nue_loudness_sample_peak(nue_loudness m);
NUE_API double NUE_CALL nue_loudness_true_peak(nue_loudness m);

typedef struct nue_loudness_result {
    uint32_t struct_size;
    double integrated_lufs, range_lu, sample_peak, true_peak;       /* peaks linear */
} nue_loudness_result;
/* Offline analysis of a whole buffer. */
NUE_API nue_result NUE_CALL nue_loudness_analyze(const float* interleaved, uint32_t frames, uint32_t channels, nue_channel_mask mask, uint32_t sample_rate, nue_loudness_result* out);
/* Linear gain that brings a measured programme to target_lufs, reduced if needed so that the true peak stays under ceiling_dbtp
 * (e.g. -1). Typical targets: -23 (broadcast, EBU R128), -16 (mobile / headphones), -14 (streaming). */
NUE_API float NUE_CALL nue_loudness_normalization_gain(double integrated_lufs, double true_peak_linear, double target_lufs, double ceiling_dbtp);

/* ============================================================================================================ reverb */

/* The EFX / EAX reverb model (EAX Reverb properties). Gains are linear amplitudes, times in seconds, frequencies in Hz.
 * nue_reverb_params_from_* convert the units of the older interfaces. */
typedef struct nue_reverb_params {
    float density;                      /* 0..1 echo density of the late field */
    float diffusion;                    /* 0..1 */
    float gain;                         /* 0..1 master (room) */
    float gain_hf;                      /* 0..1 high frequency level of the whole effect (above hf_reference) */
    float gain_lf;                      /* 0..1 */
    float decay_time;                   /* 0.1..20 s: RT60 at mid frequencies */
    float decay_hf_ratio;               /* 0.1..2: RT60(hf) / RT60 */
    float decay_lf_ratio;               /* 0.1..2 */
    float reflections_gain;             /* 0..3.16 */
    float reflections_delay;            /* 0..0.3 s */
    float reflections_pan[3];           /* direction of the early reflections; only the left/right part (x) is used by stereo output */
    float late_gain;                    /* 0..10 */
    float late_delay;                   /* 0..0.1 s after the reflections */
    float late_pan[3];
    float echo_time;                    /* 0.075..0.25 s */
    float echo_depth;                   /* 0..1 */
    float modulation_time;              /* 0.04..4 s */
    float modulation_depth;             /* 0..1 */
    float air_absorption_gain_hf;       /* 0.892..1 per meter-equivalent of decay; applied as extra HF decay */
    float hf_reference;                 /* 1000..20000 */
    float lf_reference;                 /* 20..1000 */
    float room_rolloff_factor;          /* 0..10, informational for the 3D layer (distance dependent send level) */
    nue_bool decay_hf_limit;            /* limit HF decay by air absorption */
} nue_reverb_params;

NUE_API void NUE_CALL nue_reverb_params_default(nue_reverb_params* p);                       /* the EFX "generic" preset */

/* I3DL2 / DirectSound (DSFXI3DL2Reverb): levels in 1/100 dB (mB). */
typedef struct nue_i3dl2_reverb {
    int32_t room;                       /* -10000..0 */
    int32_t room_hf;                    /* -10000..0 */
    float room_rolloff_factor;          /* 0..10 */
    float decay_time;                   /* 0.1..20 */
    float decay_hf_ratio;               /* 0.1..2 */
    int32_t reflections;                /* -10000..1000 */
    float reflections_delay;            /* 0..0.3 */
    int32_t reverb;                     /* -10000..2000 */
    float reverb_delay;                 /* 0..0.1 */
    float diffusion;                    /* 0..100 % */
    float density;                      /* 0..100 % */
    float hf_reference;                 /* 20..20000 Hz */
} nue_i3dl2_reverb;
/* EAX 2.0 / 3.0 listener properties (EAX20LISTENERPROPERTIES / EAXREVERBPROPERTIES minus the environment fields), mB levels. */
typedef struct nue_eax_reverb {
    int32_t room, room_hf, room_lf;
    float decay_time, decay_hf_ratio, decay_lf_ratio;
    int32_t reflections;
    float reflections_delay;
    float reflections_pan[3];
    int32_t reverb;
    float reverb_delay;
    float reverb_pan[3];
    float echo_time, echo_depth, modulation_time, modulation_depth;
    float air_absorption_hf;            /* mB per meter (EAX: -100..0) */
    float hf_reference, lf_reference;
    float room_rolloff_factor;
    float diffusion, density;           /* 0..100 % in EAX 2.0 terms (EAX 3.0 uses 0..1: pass percent here) */
    uint32_t flags;                     /* EAXREVERBFLAGS_DECAYHFLIMIT = 0x20 */
} nue_eax_reverb;

NUE_API void NUE_CALL nue_reverb_params_from_i3dl2(nue_reverb_params* out, const nue_i3dl2_reverb* in);
NUE_API void NUE_CALL nue_reverb_params_from_eax(nue_reverb_params* out, const nue_eax_reverb* in);
NUE_API float NUE_CALL nue_millibel_to_gain(int32_t mb);                                    /* 10^(mB/2000) */

/* The 30 I3DL2 environment presets of DirectSound (DSFX_I3DL2_ENVIRONMENT_PRESET_*), index in the order of dsound.h. */
#define NUE_I3DL2_PRESET_COUNT 30
NUE_API const char* NUE_CALL nue_i3dl2_preset_name(uint32_t index);
NUE_API nue_result NUE_CALL nue_i3dl2_preset(uint32_t index, nue_i3dl2_reverb* out);
NUE_API nue_result NUE_CALL nue_i3dl2_preset_by_name(const char* name, nue_i3dl2_reverb* out);   /* case-insensitive, spaces and underscores ignored */

NUE_DEFINE_HANDLE(nue_reverb);

NUE_API nue_result NUE_CALL nue_reverb_create(uint32_t channels, nue_channel_mask mask, uint32_t sample_rate, nue_reverb* out);
NUE_API void NUE_CALL nue_reverb_destroy(nue_reverb r);
NUE_API void NUE_CALL nue_reverb_reset(nue_reverb r);
NUE_API nue_result NUE_CALL nue_reverb_set_params(nue_reverb r, const nue_reverb_params* p);   /* real-time safe; delay lengths glide, never click */
/* In place on a bus signal: the input (all channels but the LFE summed to mono) is replaced by the WET reverb only. Pair it with the
 * bus effect's wet_dry (nue_effect_desc.wet_dry) or a send bus to control the level. */
NUE_API void NUE_CALL nue_reverb_process(nue_reverb r, float* samples, uint32_t frames);
NUE_API void NUE_CALL nue_reverb_dsp(void* handle, float* samples, uint32_t frames, uint32_t channels, uint32_t sample_rate);

/* Convolution reverb: uniformly partitioned FFT convolution (block size 256..4096) of a mono or stereo impulse response with
 * a mono-summed input; true stereo IRs convolve L and R separately. The IR is resampled to the stream rate with the HQ resampler. */
NUE_DEFINE_HANDLE(nue_convolver);
NUE_API nue_result NUE_CALL nue_convolver_create(uint32_t channels, uint32_t sample_rate, const float* ir, uint32_t ir_frames, uint32_t ir_channels,
                                                 uint32_t ir_sample_rate, uint32_t block_frames, nue_convolver* out);
NUE_API void NUE_CALL nue_convolver_destroy(nue_convolver c);
NUE_API void NUE_CALL nue_convolver_reset(nue_convolver c);
NUE_API uint32_t NUE_CALL nue_convolver_latency_frames(nue_convolver c);                    /* = block_frames */
NUE_API void NUE_CALL nue_convolver_process(nue_convolver c, float* samples, uint32_t frames);   /* wet only, like nue_reverb_process */
NUE_API void NUE_CALL nue_convolver_dsp(void* handle, float* samples, uint32_t frames, uint32_t channels, uint32_t sample_rate);
/* A synthetic exponentially decaying noise impulse response (for tests and as a default room): frames written (stereo interleaved). */
NUE_API uint32_t NUE_CALL nue_ir_generate_room(float decay_time_s, float damping, uint32_t sample_rate, uint32_t seed, float* out_stereo, uint32_t capacity_frames);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_AUDIO_DSP_H */
