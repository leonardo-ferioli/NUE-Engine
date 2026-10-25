// Backends for PCM, block ADPCM (IMA / MS / Xbox / EA XAS) and frame-stream ADPCM (PSX / DSP / EA-XA), plus the RIFF WAVE parser.
#include "internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace nuea {
namespace {

constexpr uint32_t kChunkFrames = 2048;

// ------------------------------------------------------------------------------------------------------------------------- PCM

class PcmBackend : public Backend {
public:
    PcmBackend(ByteSource* src, nue_acodec codec, uint32_t channels, uint64_t offset, uint64_t frames)
        : src_(src), codec_(codec), ch_(channels), off_(offset) {
        bps_ = codec == NUE_ACODEC_PCM_U8 || codec == NUE_ACODEC_PCM_S8 ? 1 : codec == NUE_ACODEC_PCM_S16LE || codec == NUE_ACODEC_PCM_S16BE ? 2
             : codec == NUE_ACODEC_PCM_S24LE ? 3 : 4;
        frames_ = frames;
        info.bits_per_sample = bps_ * 8;
    }
    uint32_t read(float* out, uint32_t frames, bool* corrupt) override {
        uint32_t done = 0;
        while (done < frames && pos < frames_) {
            uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(frames - done, frames_ - pos), kChunkFrames));
            const size_t bytes = size_t(n) * ch_ * bps_;
            const uint8_t* p = src_->fetch(off_ + pos * ch_ * bps_, bytes);
            if (!p) { // truncated file: deliver what is there
                size_t avail = src_->size() > off_ + pos * ch_ * bps_ ? size_t(src_->size() - (off_ + pos * ch_ * bps_)) : 0;
                n = uint32_t(avail / (ch_ * bps_));
                if (n == 0) { frames_ = pos; break; }
                p = src_->fetch(off_ + pos * ch_ * bps_, size_t(n) * ch_ * bps_);
                if (!p) break;
            }
            convert(p, out + size_t(done) * ch_, size_t(n) * ch_);
            done += n; pos += n;
        }
        return done;
    }
    bool seek(uint64_t frame) override { pos = std::min(frame, frames_); return true; }
private:
    void convert(const uint8_t* p, float* o, size_t n) const {
        switch (codec_) {
        case NUE_ACODEC_PCM_U8:   for (size_t i = 0; i < n; ++i) o[i] = (float(p[i]) - 128.0f) * (1.0f / 128.0f); break;
        case NUE_ACODEC_PCM_S8:   for (size_t i = 0; i < n; ++i) o[i] = float(int8_t(p[i])) * (1.0f / 128.0f); break;
        case NUE_ACODEC_PCM_S16LE: for (size_t i = 0; i < n; ++i) o[i] = float(int16_t(rd_le16(p + 2 * i))) * (1.0f / 32768.0f); break;
        case NUE_ACODEC_PCM_S16BE: for (size_t i = 0; i < n; ++i) o[i] = float(int16_t(rd_be16(p + 2 * i))) * (1.0f / 32768.0f); break;
        case NUE_ACODEC_PCM_S24LE:
            for (size_t i = 0; i < n; ++i) {
                int32_t v = int32_t((uint32_t(p[3 * i]) << 8) | (uint32_t(p[3 * i + 1]) << 16) | (uint32_t(p[3 * i + 2]) << 24)) >> 8;
                o[i] = float(v) * (1.0f / 8388608.0f);
            }
            break;
        case NUE_ACODEC_PCM_S32LE: for (size_t i = 0; i < n; ++i) o[i] = float(double(int32_t(rd_le32(p + 4 * i))) * (1.0 / 2147483648.0)); break;
        case NUE_ACODEC_PCM_F32LE:
            for (size_t i = 0; i < n; ++i) { uint32_t u = rd_le32(p + 4 * i); float f; std::memcpy(&f, &u, 4); o[i] = f; }
            break;
        default: std::memset(o, 0, n * sizeof(float));
        }
    }
    ByteSource* src_;
    nue_acodec codec_;
    uint32_t ch_, bps_ = 2;
    uint64_t off_, frames_;
};

// ------------------------------------------------------------------------------------------------------------------------- block ADPCM

class BlockBackend : public Backend {
public:
    BlockBackend(ByteSource* src, nue_acodec codec, uint32_t channels, uint64_t offset, uint64_t data_size, uint32_t block_bytes,
                 const nue_ms_adpcm_coef* coefs, uint32_t coef_count)
        : src_(src), codec_(codec), ch_(channels), off_(offset), block_(block_bytes) {
        if (coefs && coef_count) coefs_.assign(coefs, coefs + coef_count);
        switch (codec) {
        case NUE_ACODEC_IMA_WAV: spb_ = nue_adpcm_ima_wav_samples_per_block(block_, ch_); break;
        case NUE_ACODEC_MS_ADPCM: spb_ = nue_adpcm_ms_samples_per_block(block_, ch_); break;
        case NUE_ACODEC_IMA_XBOX: block_ = NUE_XBOX_ADPCM_BYTES_PER_CHANNEL * ch_; spb_ = NUE_XBOX_ADPCM_FRAMES_PER_BLOCK; break;
        case NUE_ACODEC_EA_XAS: block_ = NUE_EA_XAS_BYTES_PER_CHANNEL * ch_; spb_ = NUE_EA_XAS_FRAMES_PER_BLOCK; break;
        default: break;
        }
        ok_ = spb_ != 0 && block_ != 0;
        if (!ok_) return;
        full_blocks_ = data_size / block_;
        frames_ = full_blocks_ * spb_;
        tail_bytes_ = uint32_t(data_size % block_);
        if (tail_bytes_) {                                    // a short last block (IMA/MS only; the others are fixed size)
            if (codec == NUE_ACODEC_IMA_WAV) tail_frames_ = nue_adpcm_ima_wav_samples_per_block(tail_bytes_, ch_);
            else if (codec == NUE_ACODEC_MS_ADPCM) tail_frames_ = nue_adpcm_ms_samples_per_block(tail_bytes_, ch_);
            frames_ += tail_frames_;
        }
        pcm_.resize(size_t(spb_) * ch_);
        info.frame_count = frames_;
    }
    bool ok() const { return ok_; }
    uint64_t frames() const { return frames_; }
    void set_frames(uint64_t f) { frames_ = std::min(frames_, f); info.frame_count = frames_; }
    uint32_t read(float* out, uint32_t frames, bool* corrupt) override {
        uint32_t done = 0;
        while (done < frames && pos < frames_) {
            const uint64_t blk = pos / spb_;
            if (blk != cur_ && !load(blk, corrupt)) { frames_ = pos; break; }
            const uint32_t in = uint32_t(pos - blk * spb_);
            uint32_t avail = cur_frames_ > in ? cur_frames_ - in : 0;
            uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(frames - done, avail), frames_ - pos));
            if (n == 0) { frames_ = pos; break; }
            s16_to_f32(pcm_.data() + size_t(in) * ch_, out + size_t(done) * ch_, size_t(n) * ch_);
            done += n; pos += n;
        }
        return done;
    }
    bool seek(uint64_t frame) override { pos = std::min(frame, frames_); return true; }
private:
    bool load(uint64_t blk, bool* corrupt) {
        const bool tail = blk >= full_blocks_;
        const uint32_t bytes = tail ? tail_bytes_ : block_;
        const uint8_t* p = src_->fetch(off_ + blk * block_, bytes);
        if (!p) return false;
        uint32_t n = 0;
        switch (codec_) {
        case NUE_ACODEC_IMA_WAV: n = nue_adpcm_ima_wav_decode_block(p, bytes, ch_, pcm_.data()); break;
        case NUE_ACODEC_MS_ADPCM: n = nue_adpcm_ms_decode_block(p, bytes, ch_, coefs_.empty() ? nullptr : coefs_.data(), uint32_t(coefs_.size()), pcm_.data()); break;
        case NUE_ACODEC_IMA_XBOX: n = nue_adpcm_xbox_decode_block(p, ch_, pcm_.data()); break;
        case NUE_ACODEC_EA_XAS: n = nue_adpcm_ea_xas_decode_block(p, ch_, pcm_.data()); break;
        default: break;
        }
        if (n == 0) { if (corrupt) *corrupt = true; return false; }
        cur_ = blk; cur_frames_ = n;
        return true;
    }
    ByteSource* src_;
    nue_acodec codec_;
    uint32_t ch_;
    uint64_t off_;
    uint32_t block_, spb_ = 0;
    bool ok_ = false;
    uint64_t frames_ = 0, full_blocks_ = 0;
    uint32_t tail_bytes_ = 0, tail_frames_ = 0;
    std::vector<nue_ms_adpcm_coef> coefs_;
    std::vector<int16_t> pcm_;
    uint64_t cur_ = ~0ull;
    uint32_t cur_frames_ = 0;
};

// ------------------------------------------------------------------------------------------------------------------------- frame-stream ADPCM

struct Hist { int32_t h1 = 0, h2 = 0; };

class FrameBackend : public Backend {
public:
    FrameBackend(ByteSource* src, const nue_adec_raw_desc& d, uint64_t data_size)
        : src_(src), codec_(d.codec), ch_(d.channels), base_(d.start_offset), interleave_(d.interleave) {
        switch (codec_) {
        case NUE_ACODEC_PSX_ADPCM: fb_ = NUE_PSX_FRAME_BYTES; fs_ = NUE_PSX_FRAME_SAMPLES; break;
        case NUE_ACODEC_DSP_ADPCM: fb_ = NUE_DSP_FRAME_BYTES; fs_ = NUE_DSP_FRAME_SAMPLES; break;
        default: fb_ = NUE_EA_XA_FRAME_BYTES; fs_ = NUE_EA_XA_FRAME_SAMPLES; break;
        }
        if (interleave_ == 0 || ch_ == 1) {
            chan_bytes_ = data_size / ch_;
            frames_total_ = chan_bytes_ / fb_;
            interleave_ = 0;
        } else {
            fpc_ = std::max<uint32_t>(1, interleave_ / fb_);
            interleave_ = fpc_ * fb_;     // chunks hold whole frames (the tail of a chunk is padding)
            const uint64_t group = uint64_t(ch_) * d.interleave;
            const uint64_t groups = data_size / group;
            const uint64_t rem = data_size % group;
            frames_total_ = groups * fpc_ + std::min<uint64_t>(fpc_, (rem / ch_) / fb_);
            full_group_ = d.interleave;
        }
        frames_ = frames_total_ * fs_;
        if (d.frame_count) frames_ = std::min<uint64_t>(frames_, d.frame_count);
        info.frame_count = frames_;
        for (uint32_t c = 0; c < ch_; ++c) {
            if (d.dsp) { std::memcpy(coef_[c], d.dsp[c].coef, sizeof(coef_[c])); init_[c].h1 = d.dsp[c].hist1; init_[c].h2 = d.dsp[c].hist2; }
            st_[c] = init_[c];
        }
        ckpt_.reserve(size_t(frames_total_ / kStride) + 2);
        out_.assign(size_t(ch_) * fs_, 0);
        psx_ignore_flags_ = false;
    }
    void set_psx_ignore_flags(bool v) { psx_ignore_flags_ = v; }
    uint32_t read(float* out, uint32_t frames, bool* corrupt) override {
        uint32_t done = 0;
        while (done < frames && pos < frames_) {
            const uint64_t f = pos / fs_;
            if (f != cur_ && !decode_frame(f)) { frames_ = pos; break; }
            const uint32_t in = uint32_t(pos % fs_);
            uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(frames - done, fs_ - in), frames_ - pos));
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t c = 0; c < ch_; ++c)
                    out[size_t(done + i) * ch_ + c] = float(out_[size_t(c) * fs_ + in + i]) * (1.0f / 32768.0f);
            done += n; pos += n;
        }
        return done;
    }
    bool seek(uint64_t frame) override { pos = std::min(frame, frames_); return true; }
private:
    static constexpr uint64_t kStride = 256;
    uint64_t frame_offset(uint32_t c, uint64_t f) const {
        if (interleave_ == 0) return base_ + uint64_t(c) * chan_bytes_ + f * fb_;
        const uint64_t chunk = f / fpc_, within = f % fpc_;
        return base_ + (chunk * ch_ + c) * full_group_ + within * fb_;
    }
    void decode_one(uint32_t c, const uint8_t* p, int16_t* o) {
        switch (codec_) {
        case NUE_ACODEC_PSX_ADPCM: { nue_psx_state s{st_[c].h1, st_[c].h2, psx_ignore_flags_ ? 1 : 0}; nue_adpcm_psx_decode_frame(&s, p, o); st_[c].h1 = s.hist1; st_[c].h2 = s.hist2; break; }
        case NUE_ACODEC_DSP_ADPCM: { nue_dsp_state s{st_[c].h1, st_[c].h2}; nue_adpcm_dsp_decode_frame(&s, coef_[c], p, o); st_[c].h1 = s.hist1; st_[c].h2 = s.hist2; break; }
        default: { nue_ea_xa_state s{st_[c].h1, st_[c].h2}; nue_adpcm_ea_xa_decode_frame(&s, NUE_EA_XA_V1, p, o); st_[c].h1 = s.hist1; st_[c].h2 = s.hist2; break; }
        }
    }
    // Decodes codec frame f of every channel into out_ (planar). Resynchronises from a checkpoint if f is not the next frame.
    bool decode_frame(uint64_t f) {
        if (f != next_) {
            if (f < next_ || f > next_ + kStride) {                       // jump: restore the nearest checkpoint at or before f
                const size_t count = ckpt_.size() / ch_;
                if (count == 0) { for (uint32_t c = 0; c < ch_; ++c) st_[c] = init_[c]; next_ = 0; }
                else {
                    const uint64_t k = std::min<uint64_t>(f / kStride, count - 1);
                    for (uint32_t c = 0; c < ch_; ++c) st_[c] = ckpt_[size_t(k) * ch_ + c];
                    next_ = k * kStride;
                }
            }
            while (next_ < f) if (!step()) return false;
        }
        return step();
    }
    bool step() {
        const uint64_t f = next_;
        if (f >= frames_total_) return false;
        if (f % kStride == 0 && f / kStride == ckpt_.size() / ch_) {       // checkpoint before decoding frame f
            for (uint32_t c = 0; c < ch_; ++c) ckpt_.push_back(st_[c]);
        }
        for (uint32_t c = 0; c < ch_; ++c) {
            const uint8_t* p = src_->fetch(frame_offset(c, f), fb_);
            if (!p) return false;
            decode_one(c, p, out_.data() + size_t(c) * fs_);
        }
        cur_ = f; next_ = f + 1;
        return true;
    }
    ByteSource* src_;
    nue_acodec codec_;
    uint32_t ch_;
    uint64_t base_;
    uint32_t interleave_;
    uint32_t fb_ = 16, fs_ = 28, fpc_ = 1;
    uint32_t full_group_ = 0;
    uint64_t chan_bytes_ = 0, frames_total_ = 0, frames_ = 0;
    Hist st_[NUE_AUDIO_MAX_CHANNELS], init_[NUE_AUDIO_MAX_CHANNELS];
    int16_t coef_[NUE_AUDIO_MAX_CHANNELS][16] = {};
    std::vector<Hist> ckpt_;                      // ckpt_[k * ch + c] = state of channel c before codec frame k * kStride
    std::vector<int16_t> out_;                    // planar: channel c at c * fs_
    uint64_t cur_ = ~0ull, next_ = 0;
    bool psx_ignore_flags_ = false;
};

// ------------------------------------------------------------------------------------------------------------------------- WAV

struct WavFmt {
    uint32_t tag = 0, channels = 0, rate = 0, block_align = 0, bits = 0;
    uint32_t samples_per_block = 0;
    std::vector<nue_ms_adpcm_coef> coefs;
};

}  // namespace

std::unique_ptr<Backend> open_raw(ByteSource* src, const nue_adec_raw_desc& d) {
    if (d.channels == 0 || d.channels > NUE_AUDIO_MAX_CHANNELS || d.sample_rate == 0) return nullptr;
    if (d.start_offset > src->size()) return nullptr;
    uint64_t data = d.data_size ? d.data_size : src->size() - d.start_offset;
    if (d.start_offset + data > src->size()) data = src->size() - d.start_offset;
    std::unique_ptr<Backend> be;
    switch (d.codec) {
    case NUE_ACODEC_PCM_U8: case NUE_ACODEC_PCM_S8: case NUE_ACODEC_PCM_S16LE: case NUE_ACODEC_PCM_S16BE:
    case NUE_ACODEC_PCM_S24LE: case NUE_ACODEC_PCM_S32LE: case NUE_ACODEC_PCM_F32LE: {
        const uint32_t bps = d.codec == NUE_ACODEC_PCM_U8 || d.codec == NUE_ACODEC_PCM_S8 ? 1 : d.codec == NUE_ACODEC_PCM_S16LE || d.codec == NUE_ACODEC_PCM_S16BE ? 2
                           : d.codec == NUE_ACODEC_PCM_S24LE ? 3 : 4;
        uint64_t frames = data / (uint64_t(bps) * d.channels);
        if (d.frame_count) frames = std::min<uint64_t>(frames, d.frame_count);
        be = std::make_unique<PcmBackend>(src, d.codec, d.channels, d.start_offset, frames);
        be->info.frame_count = frames;
        break;
    }
    case NUE_ACODEC_IMA_WAV: case NUE_ACODEC_MS_ADPCM: case NUE_ACODEC_IMA_XBOX: case NUE_ACODEC_EA_XAS: {
        auto b = std::make_unique<BlockBackend>(src, d.codec, d.channels, d.start_offset, data, d.block_size, d.ms_coefs, d.ms_coef_count);
        if (!b->ok()) return nullptr;
        if (d.frame_count) b->set_frames(d.frame_count);
        be = std::move(b);
        break;
    }
    case NUE_ACODEC_PSX_ADPCM: case NUE_ACODEC_DSP_ADPCM: case NUE_ACODEC_EA_XA: {
        if (d.codec == NUE_ACODEC_DSP_ADPCM && !d.dsp) return nullptr;
        if (d.codec == NUE_ACODEC_EA_XA && d.channels != 1 && d.interleave == 0) return nullptr;
        be = std::make_unique<FrameBackend>(src, d, data);
        break;
    }
    default: return nullptr;
    }
    be->info.struct_size = sizeof(nue_adec_info);
    be->info.codec = d.codec;
    be->info.spec.channels = d.channels;
    be->info.spec.sample_rate = d.sample_rate;
    be->info.spec.format = NUE_AUDIO_S16;
    if (d.codec == NUE_ACODEC_PCM_F32LE || d.codec == NUE_ACODEC_PCM_S24LE || d.codec == NUE_ACODEC_PCM_S32LE) be->info.spec.format = NUE_AUDIO_F32;
    if (d.loop_end > d.loop_start) {
        be->info.has_loop = NUE_TRUE;
        be->info.loop_start = d.loop_start;
        be->info.loop_end = d.loop_end;
    }
    return be;
}

std::unique_ptr<Backend> open_wav(ByteSource* src, const nue_adec_raw_desc*) {
    uint8_t hdr[12];
    if (src->read(0, hdr, 12) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return nullptr;
    WavFmt fmt;
    bool have_fmt = false;
    uint64_t data_off = 0, data_size = 0;
    bool have_data = false;
    uint32_t loop_start = 0, loop_end = 0;
    bool has_loop = false;
    std::string title;
    uint64_t pos = 12;
    while (pos + 8 <= src->size()) {
        uint8_t ch[8];
        if (src->read(pos, ch, 8) != 8) break;
        uint64_t size = rd_le32(ch + 4);
        const uint64_t body = pos + 8;
        if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
            std::vector<uint8_t> f(size_t(std::min<uint64_t>(size, 4096)));
            if (src->read(body, f.data(), f.size()) != f.size()) return nullptr;
            fmt.tag = rd_le16(f.data()); fmt.channels = rd_le16(f.data() + 2); fmt.rate = rd_le32(f.data() + 4);
            fmt.block_align = rd_le16(f.data() + 12); fmt.bits = rd_le16(f.data() + 14);
            const uint32_t cb = f.size() >= 18 ? rd_le16(f.data() + 16) : 0;
            if (fmt.tag == 0xFFFE && f.size() >= 40 && cb >= 22) fmt.tag = rd_le16(f.data() + 24);   // WAVE_FORMAT_EXTENSIBLE: sub-format GUID
            if ((fmt.tag == 0x11) && f.size() >= 20 && cb >= 2) fmt.samples_per_block = rd_le16(f.data() + 18);
            if (fmt.tag == 0x02 && f.size() >= 22 && cb >= 32) {
                fmt.samples_per_block = rd_le16(f.data() + 18);
                const uint32_t nc = rd_le16(f.data() + 20);
                for (uint32_t i = 0; i < nc && 22 + 4 * (i + 1) <= f.size(); ++i)
                    fmt.coefs.push_back({int16_t(rd_le16(f.data() + 22 + 4 * i)), int16_t(rd_le16(f.data() + 24 + 4 * i))});
            }
            have_fmt = true;
        } else if (std::memcmp(ch, "data", 4) == 0) {
            data_off = body;
            if (size == 0xFFFFFFFFu || body + size > src->size()) size = src->size() - body;      // streamed / truncated
            data_size = size;
            have_data = true;
        } else if (std::memcmp(ch, "smpl", 4) == 0 && size >= 36) {
            uint8_t s[60];
            const size_t want = size_t(std::min<uint64_t>(size, sizeof(s)));
            if (src->read(body, s, want) == want && rd_le32(s + 28) >= 1 && want >= 60) {   // first loop: id, type, start, end, fraction, count
                loop_start = rd_le32(s + 44); loop_end = rd_le32(s + 48) + 1;                // end is inclusive in smpl
                has_loop = loop_end > loop_start;
            }
        } else if (std::memcmp(ch, "LIST", 4) == 0 && size >= 12) {
            std::vector<uint8_t> l(size_t(std::min<uint64_t>(size, 4096)));
            if (src->read(body, l.data(), l.size()) == l.size() && std::memcmp(l.data(), "INFO", 4) == 0) {
                size_t q = 4;
                while (q + 8 <= l.size()) {
                    uint32_t sz = rd_le32(l.data() + q + 4);
                    if (std::memcmp(l.data() + q, "INAM", 4) == 0 && q + 8 + sz <= l.size()) {
                        title.assign(reinterpret_cast<const char*>(l.data() + q + 8), sz);
                        title.resize(std::strlen(title.c_str()));
                    }
                    q += 8 + ((sz + 1) & ~1u);
                }
            }
        }
        pos = body + size + (size & 1);
        if (have_data && have_fmt && size >= 0xFFFFFFF0u) break;
    }
    if (!have_fmt || !have_data || fmt.channels == 0 || fmt.channels > NUE_AUDIO_MAX_CHANNELS || fmt.rate == 0) return nullptr;

    nue_adec_raw_desc d;
    std::memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.channels = fmt.channels; d.sample_rate = fmt.rate; d.start_offset = data_off; d.data_size = data_size;
    d.block_size = fmt.block_align;
    switch (fmt.tag) {
    case 1:
        d.codec = fmt.bits == 8 ? NUE_ACODEC_PCM_U8 : fmt.bits == 16 ? NUE_ACODEC_PCM_S16LE : fmt.bits == 24 ? NUE_ACODEC_PCM_S24LE
                : fmt.bits == 32 ? NUE_ACODEC_PCM_S32LE : NUE_ACODEC_UNKNOWN;
        break;
    case 3: d.codec = fmt.bits == 32 ? NUE_ACODEC_PCM_F32LE : NUE_ACODEC_UNKNOWN; break;
    case 0x11: d.codec = NUE_ACODEC_IMA_WAV; break;
    case 0x69: d.codec = NUE_ACODEC_IMA_XBOX; break;
    case 0x02: d.codec = NUE_ACODEC_MS_ADPCM; d.ms_coefs = fmt.coefs.empty() ? nullptr : fmt.coefs.data(); d.ms_coef_count = uint32_t(fmt.coefs.size()); break;
    default: return nullptr;
    }
    if (d.codec == NUE_ACODEC_UNKNOWN) return nullptr;
    // The coefficient vector must outlive open_raw only for the BlockBackend copy, which happens inside.
    auto be = open_raw(src, d);
    if (!be) return nullptr;
    if (has_loop && loop_end <= be->info.frame_count + 1) { be->info.has_loop = NUE_TRUE; be->info.loop_start = loop_start; be->info.loop_end = std::min<uint64_t>(loop_end, be->info.frame_count); }
    std::snprintf(be->info.title, sizeof(be->info.title), "%s", title.c_str());
    if (fmt.rate && be->info.frame_count && d.codec != NUE_ACODEC_UNKNOWN) {
        const double secs = double(be->info.frame_count) / fmt.rate;
        be->info.bitrate = secs > 0 ? uint32_t(double(data_size) * 8.0 / secs) : 0;
    }
    return be;
}

}  // namespace nuea
