// FLAC through dr_flac (public domain / MIT-0, src/audio/third_party/dr_flac). Streaming over the byte source, sample exact seek.
#include "internal.hpp"

#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#include "dr_flac.h"

#include <cctype>
#include <cstdlib>
#include <string>

namespace nuea {
namespace {

struct Tags { std::string title; int64_t loop_start = -1, loop_end = -1, loop_len = -1; };
struct Ctx { ByteSource* src; uint64_t pos; Tags* tags; };

size_t on_read(void* ud, void* dst, size_t n) {
    Ctx* c = static_cast<Ctx*>(ud);
    const size_t got = c->src->read(c->pos, dst, n);
    c->pos += got;
    return got;
}
drflac_bool32 on_tell(void* ud, drflac_int64* cursor) {
    Ctx* c = static_cast<Ctx*>(ud);
    *cursor = drflac_int64(c->pos);
    return DRFLAC_TRUE;
}
drflac_bool32 on_seek(void* ud, int offset, drflac_seek_origin origin) {
    Ctx* c = static_cast<Ctx*>(ud);
    int64_t np = origin == DRFLAC_SEEK_SET ? int64_t(offset) : origin == DRFLAC_SEEK_END ? int64_t(c->src->size()) + offset : int64_t(c->pos) + offset;
    if (np < 0 || uint64_t(np) > c->src->size()) return DRFLAC_FALSE;
    c->pos = uint64_t(np);
    return DRFLAC_TRUE;
}

void on_meta(void* ud, drflac_metadata* m) {
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    Tags* t = static_cast<Ctx*>(ud)->tags;
    drflac_vorbis_comment_iterator it;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
    drflac_uint32 len = 0;
    while (const char* c = drflac_next_vorbis_comment(&it, &len)) {
        std::string s(c, len);
        const size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        for (auto& ch : key) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        const std::string val = s.substr(eq + 1);
        if (key == "TITLE") t->title = val;
        else if (key == "LOOPSTART") t->loop_start = std::atoll(val.c_str());
        else if (key == "LOOPEND") t->loop_end = std::atoll(val.c_str());
        else if (key == "LOOPLENGTH") t->loop_len = std::atoll(val.c_str());
    }
}

class FlacBackend : public Backend {
public:
    ~FlacBackend() override { if (f_) drflac_close(f_); }
    bool init(ByteSource* src) {
        ctx_ = {src, 0, &tags_};
        f_ = drflac_open_with_metadata(on_read, on_seek, on_tell, on_meta, &ctx_, nullptr);
        if (!f_) return false;
        info.codec = NUE_ACODEC_FLAC;
        info.spec.channels = f_->channels;
        info.spec.sample_rate = f_->sampleRate;
        info.spec.format = f_->bitsPerSample <= 16 ? NUE_AUDIO_S16 : NUE_AUDIO_F32;
        info.bits_per_sample = f_->bitsPerSample;
        info.frame_count = f_->totalPCMFrameCount ? f_->totalPCMFrameCount : UINT64_MAX;
        if (tags_.loop_start >= 0) {
            int64_t end = tags_.loop_end >= 0 ? tags_.loop_end : (tags_.loop_len >= 0 ? tags_.loop_start + tags_.loop_len : int64_t(info.frame_count));
            if (info.frame_count != UINT64_MAX && end > int64_t(info.frame_count)) end = int64_t(info.frame_count);
            if (end > tags_.loop_start) { info.has_loop = NUE_TRUE; info.loop_start = uint64_t(tags_.loop_start); info.loop_end = uint64_t(end); }
        }
        std::snprintf(info.title, sizeof(info.title), "%s", tags_.title.c_str());
        return f_->channels >= 1 && f_->channels <= NUE_AUDIO_MAX_CHANNELS;
    }
    uint32_t read(float* out, uint32_t frames, bool* corrupt) override {
        const drflac_uint64 got = drflac_read_pcm_frames_f32(f_, frames, out);
        pos += got;
        return uint32_t(got);
    }
    bool seek(uint64_t frame) override {
        if (!drflac_seek_to_pcm_frame(f_, frame)) return false;
        pos = frame;
        return true;
    }
private:
    drflac* f_ = nullptr;
    Ctx ctx_{};
    Tags tags_;
};

}  // namespace

std::unique_ptr<Backend> open_flac(ByteSource* src) {
    auto b = std::make_unique<FlacBackend>();
    if (!b->init(src)) return nullptr;
    return b;
}

}  // namespace nuea
