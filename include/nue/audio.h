/*
 * NUE engine - audio devices and output streams. The software mixer (mixer.h) is built on these.
 *
 * Stream models:
 *   PUSH      the application writes interleaved frames with nue_audio_stream_push() whenever it wants; the engine keeps them in a
 *             queue and the device drains it. Underruns play silence and are counted.
 *   CALLBACK  the device asks for frames: the stream_desc.callback runs on the engine's audio thread. A callback must be
 *             real-time safe: no allocation, no locks that other threads hold while calling the engine, no I/O, no logging, no
 *             nue_* calls except nue_time_*, nue_mixer_render, nue_crc32 and the lock-free voice calls documented as [audio].
 *
 * Streams opened on the default device (device id 0) follow the system default when it changes (the engine reopens the
 * device and keeps the queue; NUE_EVENT_AUDIO_DEFAULT_CHANGED is informational). Streams are paused automatically while the
 * app is in the background unless NUE_AUDIO_KEEP_RUNNING_IN_BACKGROUND is set.
 *
 * Capture streams: the API (is_capture) is reserved; the first implementations return NUE_ERR_UNSUPPORTED.
 */
#ifndef NUE_AUDIO_H
#define NUE_AUDIO_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum nue_audio_format {
    NUE_AUDIO_FORMAT_UNKNOWN = 0,
    NUE_AUDIO_S16,                      /* signed 16-bit little endian, interleaved */
    NUE_AUDIO_F32,                      /* 32-bit float -1..1, interleaved */
    NUE_AUDIO_U8                        /* unsigned 8-bit (mixer buffers only; streams convert) */
} nue_audio_format;

#define NUE_AUDIO_MAX_CHANNELS 8

typedef struct nue_audio_spec {
    nue_audio_format format;
    uint32_t channels;                  /* 1..8; channel order: FL FR FC LFE BL BR SL SR (WAVEFORMATEXTENSIBLE order) */
    uint32_t sample_rate;               /* Hz, 8000..384000 */
} nue_audio_spec;

NUE_API uint32_t NUE_CALL nue_audio_bytes_per_sample(nue_audio_format format);
NUE_API uint32_t NUE_CALL nue_audio_frame_size(const nue_audio_spec* spec);                    /* channels * bytes per sample */

/* ---------------------------------------------------------------------------- devices ---- */

typedef uint32_t nue_audio_device_id;   /* 0 = the default device (follows the system default) */

typedef struct nue_audio_device_info {
    uint32_t struct_size;
    nue_audio_device_id id;
    char name[128];
    nue_bool is_capture;
    nue_bool is_default;
    nue_audio_spec preferred;           /* the device's native format */
    uint32_t preferred_buffer_frames;
    uint32_t reserved[4];
} nue_audio_device_info;

/* [any] Fills up to `capacity` infos; *out_count is the total. */
NUE_API nue_result NUE_CALL nue_audio_list_devices(nue_bool capture, nue_audio_device_info* infos, uint32_t capacity, uint32_t* out_count);
NUE_API nue_result NUE_CALL nue_audio_get_device_info(nue_audio_device_id id, nue_audio_device_info* out);   /* id 0 = current default output */

/* ---------------------------------------------------------------------------- streams ---- */

NUE_DEFINE_HANDLE(nue_audio_stream);

/* [audio] Fill `frame_count` frames of the stream format into `out`. Always fill the whole block (silence if nothing to play). */
typedef void (NUE_CALL *nue_audio_callback)(void* userdata, void* out, uint32_t frame_count);

typedef uint32_t nue_audio_stream_flags;
enum {
    NUE_AUDIO_KEEP_RUNNING_IN_BACKGROUND = 1u << 0,
    NUE_AUDIO_START_PAUSED               = 1u << 1,   /* created stopped; call nue_audio_stream_start */
    NUE_AUDIO_EXACT_FORMAT               = 1u << 2,   /* fail instead of letting the device/engine convert (for tests and bit-exact paths) */
    NUE_AUDIO_LOW_LATENCY                = 1u << 3,   /* prefer the smallest stable buffer (WASAPI exclusive/shared low latency, AAudio low-latency, ...) */
    NUE_AUDIO_CAPTURE                    = 1u << 4    /* reserved */
};

typedef struct nue_audio_stream_desc {
    uint32_t struct_size;
    nue_audio_device_id device;         /* 0 = default */
    nue_audio_spec spec;                /* the format the application writes/receives; the engine converts to the device format */
    nue_audio_stream_flags flags;
    nue_audio_callback callback;        /* NULL = push model */
    void* userdata;
    uint32_t latency_ms;                /* target total latency (device buffer + queue), 0 = engine default (~40 ms; 10 ms with LOW_LATENCY) */
    uint32_t queue_frames;              /* push model: capacity of the queue, 0 = 250 ms worth */
    const char* name;                   /* shown in OS mixers */
    uint32_t reserved[4];
} nue_audio_stream_desc;

typedef struct nue_audio_latency {
    uint32_t struct_size;
    uint32_t device_buffer_frames;      /* what the device asks per callback after negotiation */
    uint64_t device_latency_ns;         /* estimated time from handing a frame to the device to hearing it */
    uint32_t queued_frames;             /* push model: frames waiting */
    uint64_t total_latency_ns;          /* device latency + queued */
    uint32_t reserved[4];
} nue_audio_latency;

typedef struct nue_audio_stream_stats {
    uint32_t struct_size;
    uint64_t frames_played;             /* total frames the device consumed since start (survives device reroutes) */
    uint64_t underruns;                 /* times the device found the queue empty / callback late */
    uint64_t dropped_frames;            /* push model: frames refused or discarded by flush */
    float peak_level;                   /* peak |sample| over the last block, 0..1+ */
    float cpu_load;                     /* callback time / available time over the last second, 0..1 */
    uint32_t reserved[4];
} nue_audio_stream_stats;

/* [any] unless noted. */
NUE_API nue_result NUE_CALL nue_audio_stream_create(const nue_audio_stream_desc* desc, nue_audio_stream* out_stream);
NUE_API void NUE_CALL nue_audio_stream_destroy(nue_audio_stream stream);                     /* stops and waits for the audio thread to leave the callback */
NUE_API nue_result NUE_CALL nue_audio_stream_start(nue_audio_stream stream);
NUE_API nue_result NUE_CALL nue_audio_stream_stop(nue_audio_stream stream);                  /* pause: keeps the queue and position */
NUE_API nue_bool NUE_CALL nue_audio_stream_is_running(nue_audio_stream stream);
/* Push model: drop everything queued (the next push starts a new continuous run). */
NUE_API nue_result NUE_CALL nue_audio_stream_flush(nue_audio_stream stream);
/* Push model: copy up to frame_count frames in without blocking. *accepted = frames taken (NUE_OK_PARTIAL if fewer). */
NUE_API nue_result NUE_CALL nue_audio_stream_push(nue_audio_stream stream, const void* frames, uint32_t frame_count, uint32_t* accepted);
/* Same, but waits up to timeout_ns (< 0 forever) for room. Never call from the audio callback. */
NUE_API nue_result NUE_CALL nue_audio_stream_push_wait(nue_audio_stream stream, const void* frames, uint32_t frame_count, int64_t timeout_ns, uint32_t* accepted);
NUE_API uint32_t NUE_CALL nue_audio_stream_queued_frames(nue_audio_stream stream);
NUE_API uint32_t NUE_CALL nue_audio_stream_free_frames(nue_audio_stream stream);
/* Device clock: frames the device has consumed, and the nue_time_ns() time that count was sampled (for A/V sync). */
NUE_API nue_result NUE_CALL nue_audio_stream_get_position(nue_audio_stream stream, uint64_t* out_frames_played, int64_t* out_sample_time_ns);
NUE_API nue_result NUE_CALL nue_audio_stream_get_latency(nue_audio_stream stream, nue_audio_latency* out);
/* Ask for a different target latency; the engine may reopen the device. Returns NUE_OK_PARTIAL if it could only get close
 * (read the result with get_latency), NUE_ERR_UNSUPPORTED when the device cannot change. */
NUE_API nue_result NUE_CALL nue_audio_stream_set_latency(nue_audio_stream stream, uint32_t latency_ms);
NUE_API nue_result NUE_CALL nue_audio_stream_set_volume(nue_audio_stream stream, float volume);   /* linear gain 0..4, default 1; applied by the engine */
NUE_API nue_result NUE_CALL nue_audio_stream_get_stats(nue_audio_stream stream, nue_audio_stream_stats* out);
NUE_API nue_result NUE_CALL nue_audio_stream_get_spec(nue_audio_stream stream, nue_audio_spec* app_spec, nue_audio_spec* device_spec);

/* Sample conversion helpers (any thread, any time): format and channel conversion (up/downmix by standard matrices) and
 * linear resampling, for tools and tests. Returns the number of output frames written. */
NUE_API uint32_t NUE_CALL nue_audio_convert(const nue_audio_spec* src_spec, const void* src, uint32_t src_frames,
                                            const nue_audio_spec* dst_spec, void* dst, uint32_t dst_capacity_frames);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_AUDIO_H */
