// Public streaming-decoder API: opening, detection, loops, read/seek, mixer adapters, decode-all and the prefetch stream.
#include "internal.hpp"

#include "nue/core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

using namespace nuea;

struct nue_adec_s {
    std::unique_ptr<ByteSource> src;
    std::unique_ptr<Backend> be;
    int32_t loop_count = 0;                 // 0 = off, -1 = forever, N = loops left
    uint64_t loop_start = 0, loop_end = 0;
    bool loop_valid = false;
    std::vector<float> scratch;             // S16 conversion
    bool corrupt = false;
};

namespace {

nue_result finish_open(std::unique_ptr<ByteSource> src, std::unique_ptr<Backend> be, nue_adec* out) {
    if (!be) return NUE_ERR_UNSUPPORTED;
    auto* d = new (std::nothrow) nue_adec_s();
    if (!d) return NUE_ERR_OUT_OF_MEMORY;
    d->src = std::move(src);
    d->be = std::move(be);
    d->be->info.struct_size = sizeof(nue_adec_info);
    if (d->be->info.has_loop) { d->loop_start = d->be->info.loop_start; d->loop_end = d->be->info.loop_end; d->loop_valid = d->loop_end > d->loop_start; }
    d->scratch.resize(size_t(2048) * d->be->info.spec.channels);
    *out = d;
    return NUE_OK;
}

std::unique_ptr<Backend> detect(ByteSource* src) {
    uint8_t h[64];
    const size_t n = src->read(0, h, sizeof(h));
    if (n < 12) return nullptr;
    if (!std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVE", 4)) return open_wav(src, nullptr);
    if (!std::memcmp(h, "fLaC", 4)) return open_flac(src);
    if (!std::memcmp(h, "OggS", 4)) {
        const uint32_t segs = h[26];
        const size_t body = 27 + segs;
        if (body + 8 <= n) {
            if (h[body] == 0x01 && !std::memcmp(h + body + 1, "vorbis", 6)) return open_vorbis(src);
            if (!std::memcmp(h + body, "OpusHead", 8)) return open_opus(src);
        }
    }
    return nullptr;
}

uint32_t read_loop(nue_adec_s* d, float* out, uint32_t frames, bool* eof) {
    Backend* be = d->be.get();
    const uint32_t ch = be->info.spec.channels;
    uint32_t total = 0;
    *eof = false;
    while (frames) {
        const bool looping = d->loop_count != 0 && d->loop_valid;
        uint32_t n = frames;
        if (looping && be->pos < d->loop_end) n = uint32_t(std::min<uint64_t>(n, d->loop_end - be->pos));
        bool corrupt = false;
        const uint32_t got = be->read(out + size_t(total) * ch, n, &corrupt);
        if (corrupt) d->corrupt = true;
        total += got; frames -= got;
        if (looping && be->pos >= d->loop_end) {                 // wrapped past the loop end
            if (d->loop_count > 0) --d->loop_count;
            if (!be->seek(d->loop_start)) { *eof = true; break; }
            continue;
        }
        if (got < n) {
            if (looping && got == 0 && be->pos > d->loop_start) {   // the file ends before the declared loop end: treat the end as the loop end
                if (d->loop_count > 0) --d->loop_count;
                if (!be->seek(d->loop_start)) { *eof = true; break; }
                continue;
            }
            *eof = true;
            break;
        }
    }
    return total;
}

}  // namespace

extern "C" {

NUE_API const char* NUE_CALL nue_acodec_name(nue_acodec c) {
    switch (c) {
    case NUE_ACODEC_PCM_U8: return "pcm_u8";
    case NUE_ACODEC_PCM_S8: return "pcm_s8";
    case NUE_ACODEC_PCM_S16LE: return "pcm_s16le";
    case NUE_ACODEC_PCM_S16BE: return "pcm_s16be";
    case NUE_ACODEC_PCM_S24LE: return "pcm_s24le";
    case NUE_ACODEC_PCM_S32LE: return "pcm_s32le";
    case NUE_ACODEC_PCM_F32LE: return "pcm_f32le";
    case NUE_ACODEC_IMA_WAV: return "ima_adpcm";
    case NUE_ACODEC_IMA_XBOX: return "xbox_adpcm";
    case NUE_ACODEC_MS_ADPCM: return "ms_adpcm";
    case NUE_ACODEC_PSX_ADPCM: return "psx_adpcm";
    case NUE_ACODEC_DSP_ADPCM: return "dsp_adpcm";
    case NUE_ACODEC_EA_XAS: return "ea_xas";
    case NUE_ACODEC_EA_XA: return "ea_xa";
    case NUE_ACODEC_EA_XAS_V0: return "ea_xas_v0";
    case NUE_ACODEC_FLAC: return "flac";
    case NUE_ACODEC_VORBIS: return "vorbis";
    case NUE_ACODEC_OPUS: return "opus";
    default: return "unknown";
    }
}

NUE_API nue_result NUE_CALL nue_adec_open_memory(const void* data, size_t size, nue_adec* out) {
    if (!data || !size || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto src = std::make_unique<ByteSource>(data, size);
    auto be = detect(src.get());
    return finish_open(std::move(src), std::move(be), out);
}

NUE_API nue_result NUE_CALL nue_adec_open_source(const nue_adec_source* source, nue_adec* out) {
    if (!source || !source->read || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto src = std::make_unique<ByteSource>(*source);
    auto be = detect(src.get());
    return finish_open(std::move(src), std::move(be), out);
}

NUE_API nue_result NUE_CALL nue_adec_open_raw_memory(const void* data, size_t size, const nue_adec_raw_desc* desc, nue_adec* out) {
    if (!data || !desc || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto src = std::make_unique<ByteSource>(data, size);
    auto be = open_raw(src.get(), *desc);
    return finish_open(std::move(src), std::move(be), out);
}

NUE_API nue_result NUE_CALL nue_adec_open_raw_source(const nue_adec_source* source, const nue_adec_raw_desc* desc, nue_adec* out) {
    if (!source || !source->read || !desc || !out) return NUE_ERR_INVALID_ARGUMENT;
    *out = nullptr;
    auto src = std::make_unique<ByteSource>(*source);
    auto be = open_raw(src.get(), *desc);
    return finish_open(std::move(src), std::move(be), out);
}

NUE_API void NUE_CALL nue_adec_destroy(nue_adec d) { delete d; }

NUE_API nue_result NUE_CALL nue_adec_get_info(nue_adec d, nue_adec_info* out) {
    if (!d || !out) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t want = out->struct_size ? out->struct_size : uint32_t(sizeof(nue_adec_info));
    nue_adec_info tmp = d->be->info;
    tmp.struct_size = want;
    std::memcpy(out, &tmp, std::min<size_t>(want, sizeof(tmp)));
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_set_loop(nue_adec d, int32_t count) {
    if (!d) return NUE_ERR_INVALID_ARGUMENT;
    if (count != 0 && !d->loop_valid) return NUE_ERR_BAD_STATE;
    d->loop_count = count;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_set_loop_points(nue_adec d, uint64_t start, uint64_t end) {
    if (!d || end <= start) return NUE_ERR_INVALID_ARGUMENT;
    d->loop_start = start; d->loop_end = end; d->loop_valid = true;
    d->be->info.has_loop = NUE_TRUE; d->be->info.loop_start = start; d->be->info.loop_end = end;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_read_f32(nue_adec d, float* out, uint32_t frames, uint32_t* out_frames) {
    if (!d || (!out && frames)) return NUE_ERR_INVALID_ARGUMENT;
    d->corrupt = false;
    bool eof = false;
    const uint32_t got = read_loop(d, out, frames, &eof);
    if (out_frames) *out_frames = got;
    if (got == 0 && frames) return d->corrupt ? NUE_ERR_CORRUPT : NUE_ERR_END_OF_DATA;
    return d->corrupt ? NUE_ERR_CORRUPT : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_read_s16(nue_adec d, int16_t* out, uint32_t frames, uint32_t* out_frames) {
    if (!d || (!out && frames)) return NUE_ERR_INVALID_ARGUMENT;
    const uint32_t ch = d->be->info.spec.channels;
    const uint32_t chunk = uint32_t(d->scratch.size() / ch);
    d->corrupt = false;
    uint32_t total = 0;
    while (total < frames) {
        bool eof = false;
        const uint32_t want = std::min(chunk, frames - total);
        const uint32_t got = read_loop(d, d->scratch.data(), want, &eof);
        for (size_t i = 0, n = size_t(got) * ch; i < n; ++i) {
            const float v = d->scratch[i] * 32768.0f;
            const float r = v >= 0 ? std::floor(v + 0.5f) : std::ceil(v - 0.5f);
            out[size_t(total) * ch + i] = clamp16(int32_t(r < -32768.0f ? -32768.0f : (r > 32767.0f ? 32767.0f : r)));
        }
        total += got;
        if (eof || got < want) break;
    }
    if (out_frames) *out_frames = total;
    if (total == 0 && frames) return d->corrupt ? NUE_ERR_CORRUPT : NUE_ERR_END_OF_DATA;
    return d->corrupt ? NUE_ERR_CORRUPT : NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_seek(nue_adec d, uint64_t frame) {
    if (!d) return NUE_ERR_INVALID_ARGUMENT;
    const uint64_t total = d->be->info.frame_count;
    if (total != UINT64_MAX && frame > total) return NUE_ERR_INVALID_ARGUMENT;
    return d->be->seek(frame) ? NUE_OK : NUE_ERR_FAILED;
}

NUE_API uint64_t NUE_CALL nue_adec_tell(nue_adec d) { return d ? d->be->pos : 0; }

NUE_API uint32_t NUE_CALL nue_adec_pull_s16(void* ud, void* out, uint32_t frames) {
    nue_adec d = static_cast<nue_adec>(ud);
    uint32_t got = 0;
    return NUE_FAILED(nue_adec_read_s16(d, static_cast<int16_t*>(out), frames, &got)) && got == 0 ? 0 : got;
}
NUE_API uint32_t NUE_CALL nue_adec_pull_f32(void* ud, void* out, uint32_t frames) {
    nue_adec d = static_cast<nue_adec>(ud);
    uint32_t got = 0;
    return NUE_FAILED(nue_adec_read_f32(d, static_cast<float*>(out), frames, &got)) && got == 0 ? 0 : got;
}

NUE_API nue_result NUE_CALL nue_adec_decode_all_f32(nue_adec d, float** out_samples, uint64_t* out_frames) {
    if (!d || !out_samples || !out_frames) return NUE_ERR_INVALID_ARGUMENT;
    *out_samples = nullptr; *out_frames = 0;
    const uint32_t ch = d->be->info.spec.channels;
    std::vector<float> all;
    const uint64_t hint = d->be->info.frame_count;
    if (hint != UINT64_MAX && hint < (1ull << 31)) all.reserve(size_t(hint) * ch);
    const int32_t saved_loop = d->loop_count;
    d->loop_count = 0;
    nue_result r = d->be->seek(0) ? NUE_OK : NUE_ERR_FAILED;
    std::vector<float> chunk(size_t(4096) * ch);
    while (r == NUE_OK) {
        uint32_t got = 0;
        nue_result rr = nue_adec_read_f32(d, chunk.data(), 4096, &got);
        all.insert(all.end(), chunk.begin(), chunk.begin() + size_t(got) * ch);
        if (rr == NUE_ERR_END_OF_DATA || got < 4096) break;
        if (NUE_FAILED(rr)) { r = rr; break; }
    }
    d->loop_count = saved_loop;
    if (r != NUE_OK && all.empty()) return r;
    float* mem = static_cast<float*>(nue_alloc(std::max<size_t>(all.size(), 1) * sizeof(float), 16));
    if (!mem) return NUE_ERR_OUT_OF_MEMORY;
    std::memcpy(mem, all.data(), all.size() * sizeof(float));
    *out_samples = mem; *out_frames = all.size() / ch;
    return r;
}

NUE_API nue_result NUE_CALL nue_adec_decode_all_s16(nue_adec d, int16_t** out_samples, uint64_t* out_frames) {
    float* f = nullptr;
    uint64_t n = 0;
    nue_result r = nue_adec_decode_all_f32(d, &f, &n);
    if (!f) return r;
    const uint32_t ch = d->be->info.spec.channels;
    int16_t* mem = static_cast<int16_t*>(nue_alloc(std::max<size_t>(size_t(n) * ch, 1) * sizeof(int16_t), 16));
    if (!mem) { nue_free(f); return NUE_ERR_OUT_OF_MEMORY; }
    for (size_t i = 0, m = size_t(n) * ch; i < m; ++i) {
        const float v = f[i] * 32768.0f;
        const float rr = v >= 0 ? std::floor(v + 0.5f) : std::ceil(v - 0.5f);
        mem[i] = clamp16(int32_t(rr < -32768.0f ? -32768.0f : (rr > 32767.0f ? 32767.0f : rr)));
    }
    nue_free(f);
    *out_samples = mem; *out_frames = n;
    return r;
}

}  // extern "C"

// ------------------------------------------------------------------------------------------------------------------------- prefetch stream

struct nue_adec_stream_s {
    nue_adec dec = nullptr;
    uint32_t ch = 0;
    std::vector<float> ring;                // capacity frames * ch
    uint64_t cap = 0;
    std::atomic<uint64_t> w{0}, r{0};       // frame counters (monotonic)
    std::atomic<uint64_t> gen{0}, gen_start{0}, underruns{0};
    uint64_t gen_seen = 0;
    std::atomic<bool> eof{false}, quit{false};
    std::mutex mu;
    std::condition_variable cv;
    bool has_seek = false, has_loop = false;
    uint64_t seek_to = 0;
    int32_t loop_to = 0;
    std::thread th;

    void run() {
        std::vector<float> tmp(size_t(1024) * ch);
        std::unique_lock<std::mutex> lk(mu);
        while (!quit.load()) {
            if (has_seek || has_loop) {
                if (has_loop) { nue_adec_set_loop(dec, loop_to); has_loop = false; }
                if (has_seek) {
                    nue_adec_seek(dec, seek_to); has_seek = false;
                    eof.store(false);
                    gen_start.store(w.load(std::memory_order_acquire), std::memory_order_release);   // everything before this index is stale
                    gen.fetch_add(1, std::memory_order_release);
                }
            }
            const uint64_t start = gen_start.load(std::memory_order_acquire);
            const uint64_t rd = std::max(r.load(std::memory_order_acquire), start);
            const uint64_t used = w.load(std::memory_order_relaxed) - rd;
            if (eof.load() || cap - used < 1024) { cv.wait_for(lk, std::chrono::milliseconds(5)); continue; }
            lk.unlock();
            uint32_t got = 0;
            nue_result rr = nue_adec_read_f32(dec, tmp.data(), 1024, &got);
            if (got) {
                uint64_t wi = w.load(std::memory_order_relaxed);
                for (uint32_t i = 0; i < got; ++i) std::memcpy(&ring[size_t((wi + i) % cap) * ch], &tmp[size_t(i) * ch], ch * sizeof(float));
                w.store(wi + got, std::memory_order_release);
            }
            if (got < 1024 || NUE_FAILED(rr)) eof.store(true);
            lk.lock();
        }
    }
};

extern "C" {

NUE_API nue_result NUE_CALL nue_adec_stream_create(nue_adec dec, uint32_t ring_frames, nue_adec_stream* out) {
    if (!dec || !out) return NUE_ERR_INVALID_ARGUMENT;
    auto* s = new (std::nothrow) nue_adec_stream_s();
    if (!s) return NUE_ERR_OUT_OF_MEMORY;
    s->dec = dec;
    s->ch = dec->be->info.spec.channels;
    s->cap = ring_frames ? ring_frames : std::max<uint32_t>(dec->be->info.spec.sample_rate, 4096);
    s->ring.assign(size_t(s->cap) * s->ch, 0.0f);
    s->th = std::thread([s] { s->run(); });
    *out = s;
    return NUE_OK;
}

NUE_API void NUE_CALL nue_adec_stream_destroy(nue_adec_stream s) {
    if (!s) return;
    { std::lock_guard<std::mutex> lk(s->mu); s->quit.store(true); }
    s->cv.notify_all();
    if (s->th.joinable()) s->th.join();
    nue_adec_destroy(s->dec);
    delete s;
}

NUE_API nue_result NUE_CALL nue_adec_stream_seek(nue_adec_stream s, uint64_t frame) {
    if (!s) return NUE_ERR_INVALID_ARGUMENT;
    { std::lock_guard<std::mutex> lk(s->mu); s->seek_to = frame; s->has_seek = true; }
    s->cv.notify_all();
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_stream_set_loop(nue_adec_stream s, int32_t count) {
    if (!s) return NUE_ERR_INVALID_ARGUMENT;
    { std::lock_guard<std::mutex> lk(s->mu); s->loop_to = count; s->has_loop = true; }
    s->cv.notify_all();
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_adec_stream_get_info(nue_adec_stream s, nue_adec_info* out) {
    if (!s) return NUE_ERR_INVALID_ARGUMENT;
    return nue_adec_get_info(s->dec, out);
}

NUE_API uint64_t NUE_CALL nue_adec_stream_underruns(nue_adec_stream s) { return s ? s->underruns.load() : 0; }

static uint32_t stream_pull(nue_adec_stream s, float* out, uint32_t frames) {
    const uint64_t g = s->gen.load(std::memory_order_acquire);
    if (g != s->gen_seen) {                                  // a seek happened: drop what was queued before it
        s->gen_seen = g;
        const uint64_t start = s->gen_start.load(std::memory_order_acquire);
        if (s->r.load(std::memory_order_relaxed) < start) s->r.store(start, std::memory_order_release);
    }
    const uint64_t rd = s->r.load(std::memory_order_relaxed);
    const uint64_t avail = s->w.load(std::memory_order_acquire) - rd;
    const uint32_t n = uint32_t(std::min<uint64_t>(avail, frames));
    for (uint32_t i = 0; i < n; ++i) std::memcpy(out + size_t(i) * s->ch, &s->ring[size_t((rd + i) % s->cap) * s->ch], s->ch * sizeof(float));
    s->r.store(rd + n, std::memory_order_release);
    if (n < frames) {
        if (s->eof.load() && s->w.load(std::memory_order_acquire) == rd + n) return n;       // real end
        s->underruns.fetch_add(1);
        std::memset(out + size_t(n) * s->ch, 0, size_t(frames - n) * s->ch * sizeof(float));
        return frames;
    }
    return n;
}

NUE_API uint32_t NUE_CALL nue_adec_stream_pull_f32(void* ud, void* out, uint32_t frames) {
    nue_adec_stream s = static_cast<nue_adec_stream>(ud);
    return stream_pull(s, static_cast<float*>(out), frames);
}

NUE_API uint32_t NUE_CALL nue_adec_stream_pull_s16(void* ud, void* out, uint32_t frames) {
    nue_adec_stream s = static_cast<nue_adec_stream>(ud);
    float tmp[1024];
    const uint32_t per = 1024 / s->ch;
    uint32_t total = 0;
    int16_t* o = static_cast<int16_t*>(out);
    while (total < frames) {
        const uint32_t want = std::min(per, frames - total);
        const uint32_t got = stream_pull(s, tmp, want);
        for (size_t i = 0, n = size_t(got) * s->ch; i < n; ++i) {
            const float v = tmp[i] * 32768.0f;
            const float r = v >= 0 ? std::floor(v + 0.5f) : std::ceil(v - 0.5f);
            o[size_t(total) * s->ch + i] = clamp16(int32_t(r < -32768.0f ? -32768.0f : (r > 32767.0f ? 32767.0f : r)));
        }
        total += got;
        if (got < want) break;
    }
    return total;
}

}  // extern "C"
