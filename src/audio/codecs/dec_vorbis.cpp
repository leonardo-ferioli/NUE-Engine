// Ogg Vorbis through stb_vorbis (MIT / public domain, src/audio/third_party/stb_vorbis). stb_vorbis decodes from memory: a source that is
// not already in memory is read into a private buffer once (the compressed file; the PCM is never held).
#include "internal.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
extern "C" {
#include "stb_vorbis.c"
}

namespace nuea {
namespace {

class VorbisBackend : public Backend {
public:
    ~VorbisBackend() override { if (v_) stb_vorbis_close(v_); }
    bool init(ByteSource* src) {
        data_ = src->all(store_);
        if (!data_ || src->size() > 0x7fffffffull) return false;
        int err = 0;
        v_ = stb_vorbis_open_memory(data_, int(src->size()), &err, nullptr);
        if (!v_) return false;
        const stb_vorbis_info vi = stb_vorbis_get_info(v_);
        if (vi.channels < 1 || vi.channels > NUE_AUDIO_MAX_CHANNELS) return false;
        ch_ = uint32_t(vi.channels);
        info.codec = NUE_ACODEC_VORBIS;
        info.spec.channels = ch_;
        info.spec.sample_rate = vi.sample_rate;
        info.spec.format = NUE_AUDIO_F32;
        info.bitrate = vi.max_frame_size ? 0 : 0;
        const unsigned total = stb_vorbis_stream_length_in_samples(v_);
        info.frame_count = total ? total : UINT64_MAX;
        const stb_vorbis_comment c = stb_vorbis_get_comment(v_);
        int64_t ls = -1, le = -1, ll = -1;
        for (int i = 0; i < c.comment_list_length; ++i) {
            std::string s(c.comment_list[i]);
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = s.substr(0, eq);
            for (auto& ch : key) ch = char(std::toupper(static_cast<unsigned char>(ch)));
            const std::string val = s.substr(eq + 1);
            if (key == "TITLE") std::snprintf(info.title, sizeof(info.title), "%s", val.c_str());
            else if (key == "LOOPSTART") ls = std::atoll(val.c_str());
            else if (key == "LOOPEND") le = std::atoll(val.c_str());
            else if (key == "LOOPLENGTH") ll = std::atoll(val.c_str());
        }
        if (ls >= 0) {
            int64_t end = le >= 0 ? le : (ll >= 0 ? ls + ll : (total ? int64_t(total) : -1));
            if (total && end > int64_t(total)) end = total;
            if (end > ls) { info.has_loop = NUE_TRUE; info.loop_start = uint64_t(ls); info.loop_end = uint64_t(end); }
        }
        return true;
    }
    uint32_t read(float* out, uint32_t frames, bool* corrupt) override {
        const int got = stb_vorbis_get_samples_float_interleaved(v_, int(ch_), out, int(frames * ch_));
        if (got < 0) { if (corrupt) *corrupt = true; return 0; }
        reorder(out, uint32_t(got));
        pos += uint64_t(got);
        return uint32_t(got);
    }
    bool seek(uint64_t frame) override {
        if (frame == 0) { if (!stb_vorbis_seek_start(v_)) return false; pos = 0; return true; }
        if (!stb_vorbis_seek(v_, unsigned(frame))) return false;
        pos = frame;
        return true;
    }
private:
    // Vorbis channel order -> the engine's WAVE order (FL FR FC LFE BL BR SL SR); 1, 2, 4 channels and 7 (undefined here) are unchanged.
    void reorder(float* p, uint32_t frames) const {
        static const uint8_t m3[3] = {0, 2, 1}, m5[5] = {0, 2, 1, 3, 4}, m6[6] = {0, 2, 1, 5, 3, 4}, m8[8] = {0, 2, 1, 7, 5, 6, 3, 4};
        const uint8_t* m = ch_ == 3 ? m3 : ch_ == 5 ? m5 : ch_ == 6 ? m6 : ch_ == 8 ? m8 : nullptr;
        if (!m) return;
        float tmp[NUE_AUDIO_MAX_CHANNELS];
        for (uint32_t f = 0; f < frames; ++f) {
            float* fr = p + size_t(f) * ch_;
            for (uint32_t c = 0; c < ch_; ++c) tmp[c] = fr[m[c]];
            for (uint32_t c = 0; c < ch_; ++c) fr[c] = tmp[c];
        }
    }
    std::vector<uint8_t> store_;
    const uint8_t* data_ = nullptr;
    stb_vorbis* v_ = nullptr;
    uint32_t ch_ = 0;
};

}  // namespace

std::unique_ptr<Backend> open_vorbis(ByteSource* src) {
    auto b = std::make_unique<VorbisBackend>();
    if (!b->init(src)) return nullptr;
    return b;
}

}  // namespace nuea
