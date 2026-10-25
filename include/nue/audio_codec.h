/*
 * NUE engine - audio codec framework (module `nue_audio`, library nue::audio, header not included by nue.h).
 *
 * Two layers:
 *
 *   1. Block decoders (nue_adpcm_*): pure functions that decode the compressed frames of the game audio formats used by the consoles
 *      in docs/ROADMAP.md. No allocation, no state beyond the caller's context struct, bit exact, usable on the audio thread and by
 *      tools (nue_assets converting game audio, legacy layers decoding sample banks on load).
 *
 *   2. Streaming decoder (nue_adec): one object per sound. Opens a container (RIFF WAVE with PCM / float / IMA / MS ADPCM / Xbox
 *      ADPCM, FLAC, Ogg Vorbis, Ogg Opus) or a RAW stream described by the caller (PlayStation VAG, GameCube/Wii DSP, EA XAS,
 *      headerless IMA / MS / Xbox ADPCM, headerless PCM), and delivers PCM frames with seek and loop points.
 *
 * Threading: a decoder object is used by one thread at a time. nue_adec_read_* on a MEMORY source never allocates, never blocks and
 * is safe on the audio thread (e.g. as the `pull` of a NUE_VOICE_CALLBACK voice, see nue_adec_pull_s16). A decoder over a callback or
 * file source does I/O inside read; feed those to the mixer through nue_adec_stream (prefetch thread + ring) instead.
 *
 * Determinism: the integer codecs (PCM, IMA, MS, Xbox, XAS, VAG, DSP) are bit exact on every platform. Vorbis, Opus and FLAC follow
 * their reference decoders (FLAC is exact; Vorbis and Opus are float and may differ in the last bit between compilers).
 */
#ifndef NUE_AUDIO_CODEC_H
#define NUE_AUDIO_CODEC_H

#include "nue/base.h"
#include "nue/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================================ block decoders */

/* IMA ADPCM (4 bit). The state of one channel is the predictor and the step table index. */
typedef struct nue_ima_state {
    int32_t predictor;                  /* -32768..32767 */
    int32_t step_index;                 /* 0..88 */
} nue_ima_state;

/* Decode one 4-bit code with the standard IMA algorithm and update the state. Returns the new sample (the predictor). */
NUE_API int16_t NUE_CALL nue_ima_decode_nibble(nue_ima_state* state, uint32_t code);

/* RIFF "IMA ADPCM" (WAVE_FORMAT_IMA_ADPCM 0x0011) block. `block` holds block_size bytes: per channel a 4-byte header (int16 predictor,
 * uint8 index, uint8 reserved) followed by groups of 4 bytes per channel in turn (8 samples per group, low nibble first).
 * Writes samples_per_block interleaved frames = 1 + (block_size - 4*channels) * 2 / channels. Returns the frames written (0 on error).
 * Mono and stereo are the common cases; 1..8 channels are accepted. */
NUE_API uint32_t NUE_CALL nue_adpcm_ima_wav_decode_block(const uint8_t* block, uint32_t block_size, uint32_t channels, int16_t* out_interleaved);
NUE_API uint32_t NUE_CALL nue_adpcm_ima_wav_samples_per_block(uint32_t block_size, uint32_t channels);

/* Xbox ADPCM (WAVE_FORMAT_XBOX_ADPCM 0x0069): same algorithm and layout as the RIFF IMA block, but fixed at 36 bytes per channel
 * (block_size = 36 * channels, 64 sample frames: the header predictor is the first sample, the 32 data bytes give 63 more and the last
 * nibble is dropped). Returns the frames written, always 64 for a full block. */
NUE_API uint32_t NUE_CALL nue_adpcm_xbox_decode_block(const uint8_t* block, uint32_t channels, int16_t* out_interleaved);
#define NUE_XBOX_ADPCM_FRAMES_PER_BLOCK 64u
#define NUE_XBOX_ADPCM_BYTES_PER_CHANNEL 36u

/* Microsoft ADPCM (WAVE_FORMAT_ADPCM 0x0002). The coefficient table comes from the WAVEFORMATEX extra data; pass NULL for the 7 standard
 * pairs. Writes samples_per_block frames = 2 + (block_size - 7*channels) * 2 / channels. */
typedef struct nue_ms_adpcm_coef { int16_t c1, c2; } nue_ms_adpcm_coef;
NUE_API uint32_t NUE_CALL nue_adpcm_ms_decode_block(const uint8_t* block, uint32_t block_size, uint32_t channels,
                                                    const nue_ms_adpcm_coef* coefs, uint32_t coef_count, int16_t* out_interleaved);
NUE_API uint32_t NUE_CALL nue_adpcm_ms_samples_per_block(uint32_t block_size, uint32_t channels);

/* Sony PlayStation SPU / VAG ADPCM: 16-byte frame = [shift:4 | filter:4][flags][14 bytes = 28 nibbles, low nibble first] -> 28 samples.
 * flags: bit0 = end, bit1 = repeat (loop) marker, bit2 = loop start; a frame whose flags byte is >= 7 decodes to silence unless
 * state.ignore_flags is set. Filters use the five fixed (k1,k2) pairs of the SPU (index > 4 -> 0, shift > 12 -> 9). History is kept
 * clamped to 16 bits like the hardware. */
typedef struct nue_psx_state {
    int32_t hist1, hist2;
    int32_t ignore_flags;               /* non-zero: treat every flags byte as 0 (games that store garbage there) */
} nue_psx_state;
enum { NUE_PSX_FLAG_END = 1, NUE_PSX_FLAG_LOOP_REPEAT = 2, NUE_PSX_FLAG_LOOP_START = 4 };
#define NUE_PSX_FRAME_BYTES 16u
#define NUE_PSX_FRAME_SAMPLES 28u
/* Returns the frame's flags byte. */
NUE_API uint8_t NUE_CALL nue_adpcm_psx_decode_frame(nue_psx_state* state, const uint8_t* frame, int16_t* out28);

/* Nintendo GameCube / Wii (and DS-less DSP) ADPCM: 8-byte frame = header byte [coef index:4 | scale exponent:4] + 7 bytes = 14 nibbles,
 * high nibble first -> 14 samples. The 16 coefficients (8 pairs, 2.11 fixed point) come from the file header (DSP/DSPADPCM). */
typedef struct nue_dsp_state { int32_t hist1, hist2; } nue_dsp_state;
#define NUE_DSP_FRAME_BYTES 8u
#define NUE_DSP_FRAME_SAMPLES 14u
NUE_API void NUE_CALL nue_adpcm_dsp_decode_frame(nue_dsp_state* state, const int16_t coefs[16], const uint8_t* frame, int16_t* out14);
/* Number of sample frames in `bytes` of DSP ADPCM data (header-style nibble count) and the nibble address <-> sample conversions the
 * GameCube headers use. */
NUE_API uint32_t NUE_CALL nue_dsp_nibbles_to_samples(uint32_t nibbles);
NUE_API uint32_t NUE_CALL nue_dsp_samples_to_nibbles(uint32_t samples);

/* Electronic Arts XAS version 1 (Xbox 360, PS3, Wii and later EA games): one 0x4c-byte block per channel, channels concatenated, decoded to
 * 128 samples per channel (4 groups of 32). `out_interleaved` receives 128 * channels samples. Returns 128. */
#define NUE_EA_XAS_BYTES_PER_CHANNEL 0x4cu
#define NUE_EA_XAS_FRAMES_PER_BLOCK 128u
NUE_API uint32_t NUE_CALL nue_adpcm_ea_xas_decode_block(const uint8_t* block, uint32_t channels, int16_t* out_interleaved);

/* Electronic Arts "EA-XA" (EA ADPCM, descended from CD-XA): 15-byte mono frame = [coef index:4 | shift:4] + 14 bytes of nibbles (high
 * nibble first) -> 28 samples. V1 (PS1/PS2/PC era) rounds with +128, V2 (later revisions) does not. The stereo V1 frame is 30 bytes: byte 0 =
 * coef indices (left high nibble, right low), byte 1 = shifts, then 28 bytes where the left channel is the high nibble and the right the low.
 * The PCM frames (0xEE marker) some versions interleave belong to the EA block parser (container level), not here. */
typedef enum nue_ea_xa_variant { NUE_EA_XA_V1 = 1, NUE_EA_XA_V2 = 2 } nue_ea_xa_variant;
typedef struct nue_ea_xa_state { int32_t hist1, hist2; } nue_ea_xa_state;
#define NUE_EA_XA_FRAME_BYTES 15u
#define NUE_EA_XA_STEREO_FRAME_BYTES 30u
#define NUE_EA_XA_FRAME_SAMPLES 28u
NUE_API void NUE_CALL nue_adpcm_ea_xa_decode_frame(nue_ea_xa_state* state, nue_ea_xa_variant variant, const uint8_t* frame, int16_t* out28);
NUE_API void NUE_CALL nue_adpcm_ea_xa_decode_stereo_frame(nue_ea_xa_state* left, nue_ea_xa_state* right, const uint8_t* frame,
                                                          int16_t* out_left28, int16_t* out_right28);

/* ============================================================================================================ streaming decoder */

NUE_DEFINE_HANDLE(nue_adec);

typedef enum nue_acodec {
    NUE_ACODEC_UNKNOWN = 0,
    NUE_ACODEC_PCM_U8,
    NUE_ACODEC_PCM_S8,
    NUE_ACODEC_PCM_S16LE,
    NUE_ACODEC_PCM_S16BE,
    NUE_ACODEC_PCM_S24LE,
    NUE_ACODEC_PCM_S32LE,
    NUE_ACODEC_PCM_F32LE,
    NUE_ACODEC_IMA_WAV,                 /* block based; raw_desc.block_size */
    NUE_ACODEC_IMA_XBOX,
    NUE_ACODEC_MS_ADPCM,                /* block based; raw_desc.block_size, optional coefficient table */
    NUE_ACODEC_PSX_ADPCM,               /* VAG: raw_desc.interleave bytes per channel (0 = 16 for mono, 0x1000 typical for multichannel) */
    NUE_ACODEC_DSP_ADPCM,               /* raw_desc.dsp[channel] carries coefficients and history */
    NUE_ACODEC_EA_XAS,
    NUE_ACODEC_EA_XA,
    NUE_ACODEC_FLAC,
    NUE_ACODEC_VORBIS,
    NUE_ACODEC_OPUS
} nue_acodec;

/* A random access byte source. `read` fills up to `size` bytes at `offset` and stores how many were read (short reads only at the end).
 * `close` runs from nue_adec_destroy (may be NULL). */
typedef struct nue_adec_source {
    uint32_t struct_size;
    nue_result (NUE_CALL *read)(void* userdata, uint64_t offset, void* dst, size_t size, size_t* out_read);
    void (NUE_CALL *close)(void* userdata);
    void* userdata;
    uint64_t size;                      /* total bytes */
} nue_adec_source;

typedef struct nue_adec_dsp_channel {
    int16_t coef[16];
    int16_t hist1, hist2;               /* initial history (header hist1/hist2, usually 0) */
} nue_adec_dsp_channel;

/* Description of a headerless stream. Fields not used by the codec are ignored. */
typedef struct nue_adec_raw_desc {
    uint32_t struct_size;
    nue_acodec codec;
    uint32_t channels;
    uint32_t sample_rate;
    uint64_t start_offset;              /* bytes to skip */
    uint64_t data_size;                 /* bytes of payload, 0 = to the end of the source */
    uint32_t block_size;                /* IMA_WAV / MS_ADPCM: bytes per block (all channels); IMA_XBOX/XAS: ignored */
    uint32_t interleave;                /* PSX/DSP/EA_XA: bytes per channel before switching to the next channel (0 = whole stream per channel, channels stored one after another) */
    uint64_t frame_count;               /* 0 = derive from data_size */
    uint64_t loop_start, loop_end;      /* in sample frames; loop_end 0 = no loop information (loop_end is the exclusive end) */
    const nue_ms_adpcm_coef* ms_coefs;  /* MS_ADPCM, NULL = the 7 standard pairs */
    uint32_t ms_coef_count;
    const nue_adec_dsp_channel* dsp;    /* DSP_ADPCM: one entry per channel */
} nue_adec_raw_desc;

typedef struct nue_adec_info {
    uint32_t struct_size;
    nue_acodec codec;
    nue_audio_spec spec;                /* format = F32 or S16 as natural for the codec; channels and rate of the stream */
    uint64_t frame_count;               /* total sample frames, UINT64_MAX when unknown */
    nue_bool has_loop;
    uint64_t loop_start, loop_end;      /* sample frames, valid when has_loop (loop_end exclusive) */
    uint32_t bits_per_sample;           /* of the source PCM, 0 for compressed formats */
    uint32_t bitrate;                   /* bits/s, 0 when not meaningful */
    uint32_t pre_skip_frames;           /* Opus/encoder delay already removed from frame_count and position */
    char title[64];                     /* from tags when the container has them (empty otherwise) */
} nue_adec_info;

NUE_API const char* NUE_CALL nue_acodec_name(nue_acodec codec);

/* Open a container and detect it (RIFF WAVE, fLaC, OggS with Vorbis or Opus). The memory is NOT copied: it must stay valid until
 * nue_adec_destroy. */
NUE_API nue_result NUE_CALL nue_adec_open_memory(const void* data, size_t size, nue_adec* out);
/* Same over a source (file, pack entry...); takes ownership of the source (close runs on destroy or on failure). */
NUE_API nue_result NUE_CALL nue_adec_open_source(const nue_adec_source* source, nue_adec* out);
/* Convenience: opens the file through the engine VFS (nue_file_*). Needs the engine initialised. */
NUE_API nue_result NUE_CALL nue_adec_open_file(const char* path, nue_adec* out);
/* A headerless stream; `data` is not copied. */
NUE_API nue_result NUE_CALL nue_adec_open_raw_memory(const void* data, size_t size, const nue_adec_raw_desc* desc, nue_adec* out);
NUE_API nue_result NUE_CALL nue_adec_open_raw_source(const nue_adec_source* source, const nue_adec_raw_desc* desc, nue_adec* out);
NUE_API void NUE_CALL nue_adec_destroy(nue_adec dec);

NUE_API nue_result NUE_CALL nue_adec_get_info(nue_adec dec, nue_adec_info* out);

/* Loops: with count != 0 the decoder wraps from loop_end to loop_start by itself (info.has_loop must be true, or use
 * nue_adec_set_loop_points). count = -1 loops forever, N plays the loop N more times then continues to the end. 0 (default) plays straight through. */
NUE_API nue_result NUE_CALL nue_adec_set_loop(nue_adec dec, int32_t count);
NUE_API nue_result NUE_CALL nue_adec_set_loop_points(nue_adec dec, uint64_t loop_start, uint64_t loop_end);

/* Read up to `frames` interleaved sample frames. *out_frames gets the number delivered; result is NUE_OK, or NUE_ERR_END_OF_DATA when
 * nothing was left (out_frames = 0), NUE_ERR_CORRUPT on a damaged stream (what was decoded is still delivered). A short count with
 * NUE_OK means the end was reached. */
NUE_API nue_result NUE_CALL nue_adec_read_f32(nue_adec dec, float* out, uint32_t frames, uint32_t* out_frames);
NUE_API nue_result NUE_CALL nue_adec_read_s16(nue_adec dec, int16_t* out, uint32_t frames, uint32_t* out_frames);
/* Position in sample frames (after pre-skip). Seeking is sample exact for every codec; ADPCM seeks decode from the nearest checkpoint
 * (start, loop start, or the last seek point), Vorbis/Opus/FLAC use their seek tables / bisection. */
NUE_API nue_result NUE_CALL nue_adec_seek(nue_adec dec, uint64_t frame);
NUE_API uint64_t NUE_CALL nue_adec_tell(nue_adec dec);

/* Mixer adapters: signature of nue_voice_desc.pull (userdata = the nue_adec). Fills `out` with S16 / F32 frames of the stream format
 * (info.spec.channels) and returns the frames produced; a short return ends the voice (with looping enabled it never ends). They never fail
 * loudly: errors end the stream. Memory sources only on the audio thread. */
NUE_API uint32_t NUE_CALL nue_adec_pull_s16(void* userdata, void* out, uint32_t frame_count);
NUE_API uint32_t NUE_CALL nue_adec_pull_f32(void* userdata, void* out, uint32_t frame_count);

/* Decode a whole stream to memory (tools, small sounds loaded into nue_buffer). Allocates with nue_alloc; release with nue_free.
 * Loops are not applied; info gets the loop points. */
NUE_API nue_result NUE_CALL nue_adec_decode_all_s16(nue_adec dec, int16_t** out_samples, uint64_t* out_frames);
NUE_API nue_result NUE_CALL nue_adec_decode_all_f32(nue_adec dec, float** out_samples, uint64_t* out_frames);

/* ---------------------------------------------------------------------------- streaming with prefetch */

NUE_DEFINE_HANDLE(nue_adec_stream);

/* Moves decoding to a worker thread that keeps a ring of `ring_frames` decoded frames (0 = 1 second) ahead of the audio thread. Takes
 * ownership of the decoder. Reads from the audio thread (nue_adec_stream_pull_f32) are wait free; underruns deliver silence and are
 * counted. Seek/loop commands are processed by the worker and the ring is flushed. */
NUE_API nue_result NUE_CALL nue_adec_stream_create(nue_adec dec, uint32_t ring_frames, nue_adec_stream* out);
NUE_API void NUE_CALL nue_adec_stream_destroy(nue_adec_stream s);                      /* destroys the decoder too */
NUE_API nue_result NUE_CALL nue_adec_stream_seek(nue_adec_stream s, uint64_t frame);
NUE_API nue_result NUE_CALL nue_adec_stream_set_loop(nue_adec_stream s, int32_t count);
NUE_API nue_result NUE_CALL nue_adec_stream_get_info(nue_adec_stream s, nue_adec_info* out);
NUE_API uint64_t NUE_CALL nue_adec_stream_underruns(nue_adec_stream s);
/* Wait free; returns frames delivered; delivers fewer only when the stream ended. If the ring is momentarily empty it fills with silence
 * and returns the full count (underrun counted). */
NUE_API uint32_t NUE_CALL nue_adec_stream_pull_f32(void* stream, void* out, uint32_t frame_count);
NUE_API uint32_t NUE_CALL nue_adec_stream_pull_s16(void* stream, void* out, uint32_t frame_count);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_AUDIO_CODEC_H */
