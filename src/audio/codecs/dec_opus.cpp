// Ogg Opus (RFC 7845) through libopus (BSD-3, src/audio/third_party/opus) with our own Ogg page reader. Streams the pages through the byte source,
// builds a page index at open (header scan only) for seeking, applies pre-skip, output gain and end trimming. Output is always 48 kHz.
#include "internal.hpp"

#include "opus_multistream.h"

#include <cctype>
#include <cstdlib>
#include <string>

namespace nuea {
namespace {

struct PageInfo {
    uint64_t off;
    uint32_t hdr_len, body_len;
    int64_t granule;
    uint8_t flags, nsegs;
};

struct Cursor {
    size_t page = 0;
    uint32_t seg = 0;
    uint64_t body_pos = 0;
    std::vector<uint8_t> segs;
    bool loaded = false;
};

class OpusBackend : public Backend {
public:
    ~OpusBackend() override { if (dec_) opus_multistream_decoder_destroy(dec_); }

    bool init(ByteSource* src) {
        src_ = src;
        if (!scan()) return false;
        if (!restart_at(0, false)) return false;
        std::vector<uint8_t> pkt;
        if (!next_packet(pkt) || pkt.size() < 19 || std::memcmp(pkt.data(), "OpusHead", 8) != 0) return false;
        if ((pkt[8] >> 4) != 0) return false;                                  // major version
        channels_ = pkt[9];
        pre_skip_ = rd_le16(&pkt[10]);
        gain_q8_ = int16_t(rd_le16(&pkt[16]));
        const uint8_t family = pkt[18];
        if (channels_ < 1 || channels_ > NUE_AUDIO_MAX_CHANNELS) return false;
        unsigned char map[NUE_AUDIO_MAX_CHANNELS] = {0, 1};
        int streams = 1, coupled = channels_ == 2 ? 1 : 0;
        if (family == 0) {
            if (channels_ > 2) return false;
        } else if (family == 1) {
            if (pkt.size() < size_t(21 + channels_)) return false;
            streams = pkt[19]; coupled = pkt[20];
            for (uint32_t i = 0; i < channels_; ++i) map[i] = pkt[21 + i];
        } else {
            return false;                                                     // ambisonics / discrete layouts: not decoded here
        }
        int err = 0;
        dec_ = opus_multistream_decoder_create(48000, int(channels_), streams, coupled, map, &err);
        if (!dec_ || err != OPUS_OK) return false;
        if (gain_q8_) opus_multistream_decoder_ctl(dec_, OPUS_SET_GAIN(int(gain_q8_)));
        family1_ = family == 1;
        if (!next_packet(pkt) || pkt.size() < 8 || std::memcmp(pkt.data(), "OpusTags", 8) != 0) return false;
        parse_tags(pkt);
        audio_start_ = cursor_;
        int64_t last = -1;
        for (size_t i = pages_.size(); i-- > 0;) if (pages_[i].granule >= 0) { last = pages_[i].granule; break; }
        if (last < 0 || int64_t(pre_skip_) > last) return false;
        total_ = uint64_t(last) - pre_skip_;
        pcm_.resize(size_t(5760) * channels_);
        info.codec = NUE_ACODEC_OPUS;
        info.spec.format = NUE_AUDIO_F32;
        info.spec.channels = channels_;
        info.spec.sample_rate = 48000;
        info.frame_count = total_;
        info.pre_skip_frames = pre_skip_;
        if (ls_ >= 0) {
            int64_t end = le_ >= 0 ? le_ : (ll_ >= 0 ? ls_ + ll_ : int64_t(total_));
            if (end > int64_t(total_)) end = int64_t(total_);
            if (end > ls_) { info.has_loop = NUE_TRUE; info.loop_start = uint64_t(ls_); info.loop_end = uint64_t(end); }
        }
        std::snprintf(info.title, sizeof(info.title), "%s", title_.c_str());
        return reset_to_start();
    }

    uint32_t read(float* out, uint32_t frames, bool* corrupt) override {
        uint32_t done = 0;
        while (done < frames && pos < total_) {
            if (pcm_pos_ >= pcm_len_) {
                if (!decode_next(corrupt)) break;
                continue;
            }
            const uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(frames - done, pcm_len_ - pcm_pos_), total_ - pos));
            std::memcpy(out + size_t(done) * channels_, &pcm_[size_t(pcm_pos_) * channels_], size_t(n) * channels_ * sizeof(float));
            pcm_pos_ += n; done += n; pos += n;
        }
        return done;
    }

    bool seek(uint64_t frame) override {
        if (frame > total_) return false;
        const int64_t target_g = int64_t(frame) + pre_skip_;
        const int64_t start_g = std::max<int64_t>(0, target_g - 9600);              // 200 ms of pre-roll (RFC 7845 asks for 80 ms; pure tones converge slower)
        size_t best = SIZE_MAX;                                                    // last page with a valid granule <= start_g
        for (size_t i = 0; i < pages_.size(); ++i) {
            if (pages_[i].granule < 0) continue;
            if (pages_[i].granule <= start_g) best = i; else break;
        }
        opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE);
        pcm_len_ = pcm_pos_ = 0;
        if (best == SIZE_MAX) {
            cursor_ = audio_start_;
            decoded_g_ = 0;
        } else {
            if (!restart_at(best + 1, true)) return false;
            decoded_g_ = pages_[best].granule;
        }
        if (!discard_to(target_g)) return false;
        pos = frame;
        return true;
    }

private:
    bool discard_to(int64_t target_g) {
        while (decoded_g_ < target_g) {
            if (pcm_pos_ >= pcm_len_ && !decode_next(nullptr)) return false;
            const int64_t take = std::min<int64_t>(target_g - decoded_g_, int64_t(pcm_len_ - pcm_pos_));
            pcm_pos_ += uint32_t(take);
            decoded_g_ += take;
        }
        return true;
    }
    bool reset_to_start() {
        opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE);
        cursor_ = audio_start_;
        decoded_g_ = 0;
        pcm_len_ = pcm_pos_ = 0;
        pos = 0;
        return discard_to(int64_t(pre_skip_));
    }

    bool decode_next(bool* corrupt) {
        if (!next_packet(pkt_)) return false;
        const int n = opus_multistream_decode_float(dec_, pkt_.data(), int(pkt_.size()), pcm_.data(), 5760, 0);
        if (n < 0) { if (corrupt) *corrupt = true; return false; }
        if (family1_ && channels_ > 2) reorder(pcm_.data(), uint32_t(n));
        pcm_len_ = uint32_t(n);
        pcm_pos_ = 0;
        return true;
    }

    // Vorbis channel order (what Opus mapping family 1 uses) -> the engine's WAVE order.
    void reorder(float* p, uint32_t frames) const {
        static const uint8_t m3[3] = {0, 2, 1}, m5[5] = {0, 2, 1, 3, 4}, m6[6] = {0, 2, 1, 5, 3, 4}, m8[8] = {0, 2, 1, 7, 5, 6, 3, 4};
        const uint8_t* m = channels_ == 3 ? m3 : channels_ == 5 ? m5 : channels_ == 6 ? m6 : channels_ == 8 ? m8 : nullptr;
        if (!m) return;
        float tmp[NUE_AUDIO_MAX_CHANNELS];
        for (uint32_t f = 0; f < frames; ++f) {
            float* fr = p + size_t(f) * channels_;
            for (uint32_t c = 0; c < channels_; ++c) tmp[c] = fr[m[c]];
            for (uint32_t c = 0; c < channels_; ++c) fr[c] = tmp[c];
        }
    }

    // ---- Ogg ----
    bool scan() {
        uint64_t off = 0;
        uint32_t serial = 0;
        bool have_serial = false;
        while (off + 27 <= src_->size()) {
            const uint8_t* h = src_->fetch(off, 27);
            if (!h || std::memcmp(h, "OggS", 4) != 0) break;
            const uint8_t nsegs = h[26];
            const uint8_t* segs = src_->fetch(off, 27 + nsegs);
            if (!segs) break;
            const uint32_t ser = rd_le32(segs + 14);
            if (!have_serial) { serial = ser; have_serial = true; }
            uint32_t body = 0;
            for (uint32_t i = 0; i < nsegs; ++i) body += segs[27 + i];
            if (ser == serial) {
                uint64_t g = 0;
                for (int i = 7; i >= 0; --i) g = (g << 8) | segs[6 + i];
                pages_.push_back({off, 27u + nsegs, body, int64_t(g), segs[5], nsegs});
                if (segs[5] & 4) break;                                         // end of the logical stream (chained streams are not followed)
            }
            off += 27 + nsegs + body;
        }
        return !pages_.empty();
    }
    bool load_page(Cursor& c) {
        if (c.page >= pages_.size()) return false;
        const PageInfo& p = pages_[c.page];
        const uint8_t* s = src_->fetch(p.off, p.hdr_len);
        if (!s) return false;
        c.segs.assign(s + 27, s + 27 + p.nsegs);
        c.seg = 0;
        c.body_pos = p.off + p.hdr_len;
        c.loaded = true;
        return true;
    }
    bool restart_at(size_t page, bool skip_continued) {
        cursor_ = Cursor();
        cursor_.page = page;
        if (!load_page(cursor_)) return false;
        if (skip_continued && (pages_[page].flags & 1)) {                         // begins with the tail of a packet from the previous page: drop it
            while (cursor_.seg < cursor_.segs.size()) {
                const uint8_t len = cursor_.segs[cursor_.seg++];
                cursor_.body_pos += len;
                if (len < 255) break;
            }
        }
        return true;
    }
    bool next_packet(std::vector<uint8_t>& pkt) {
        pkt.clear();
        for (;;) {
            if (!cursor_.loaded && !load_page(cursor_)) return false;
            while (cursor_.seg < cursor_.segs.size()) {
                const uint8_t len = cursor_.segs[cursor_.seg++];
                if (len) {
                    const uint8_t* b = src_->fetch(cursor_.body_pos, len);
                    if (!b) return false;
                    pkt.insert(pkt.end(), b, b + len);
                }
                cursor_.body_pos += len;
                if (len < 255) return true;
            }
            ++cursor_.page;
            cursor_.loaded = false;
            if (cursor_.page >= pages_.size()) return false;
        }
    }
    void parse_tags(const std::vector<uint8_t>& p) {
        size_t q = 8;
        auto u32 = [&](uint32_t& v) { if (q + 4 > p.size()) return false; v = rd_le32(&p[q]); q += 4; return true; };
        uint32_t vlen = 0, count = 0;
        if (!u32(vlen)) return;
        q += vlen;
        if (!u32(count)) return;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t len = 0;
            if (!u32(len) || q + len > p.size()) return;
            std::string s(reinterpret_cast<const char*>(&p[q]), len);
            q += len;
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = s.substr(0, eq);
            for (auto& c : key) c = char(std::toupper(static_cast<unsigned char>(c)));
            const std::string val = s.substr(eq + 1);
            if (key == "TITLE") title_ = val;
            else if (key == "LOOPSTART") ls_ = std::atoll(val.c_str());
            else if (key == "LOOPEND") le_ = std::atoll(val.c_str());
            else if (key == "LOOPLENGTH") ll_ = std::atoll(val.c_str());
        }
        // loop points in tags are relative to the decoded stream (after pre-skip), like the rest of the API
    }

    ByteSource* src_ = nullptr;
    std::vector<PageInfo> pages_;
    Cursor cursor_, audio_start_;
    OpusMSDecoder* dec_ = nullptr;
    uint32_t channels_ = 0, pre_skip_ = 0;
    int16_t gain_q8_ = 0;
    bool family1_ = false;
    uint64_t total_ = 0;
    std::vector<float> pcm_;
    std::vector<uint8_t> pkt_;
    uint32_t pcm_len_ = 0, pcm_pos_ = 0;
    int64_t decoded_g_ = 0;
    std::string title_;
    int64_t ls_ = -1, le_ = -1, ll_ = -1;
};

}  // namespace

std::unique_ptr<Backend> open_opus(ByteSource* src) {
    auto b = std::make_unique<OpusBackend>();
    if (!b->init(src)) return nullptr;
    return b;
}

}  // namespace nuea
