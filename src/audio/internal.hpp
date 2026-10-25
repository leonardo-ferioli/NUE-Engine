// Internal helpers of the audio module: byte sources and the backend interface of the streaming decoder.
#pragma once
#include "nue/audio_codec.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace nuea {

// Random access bytes. A memory source hands out pointers into the caller's buffer (no copy, no allocation). A callback source reads
// through a window (default 64 KiB) that is refilled on demand. fetch() returns a pointer valid until the next fetch/read call.
class ByteSource {
public:
    explicit ByteSource(const void* data, size_t size) : mem_(static_cast<const uint8_t*>(data)), size_(size) {}
    explicit ByteSource(const nue_adec_source& src) : src_(src), size_(src.size), window_(kWindow) {}
    ~ByteSource() { if (src_.close) src_.close(src_.userdata); }
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;

    uint64_t size() const { return size_; }
    bool is_memory() const { return mem_ != nullptr; }
    // Pointer to `n` bytes at `offset`, or nullptr if the range is not fully inside the source (or the read failed).
    const uint8_t* fetch(uint64_t offset, size_t n) {
        if (offset > size_ || n > size_ - offset) return nullptr;
        if (mem_) return mem_ + offset;
        if (n == 0) return window_.data();
        if (offset >= win_off_ && offset + n <= win_off_ + win_len_) return window_.data() + (offset - win_off_);
        if (n > window_.size()) window_.resize(n);
        size_t want = std::min<uint64_t>(window_.size(), size_ - offset);
        size_t got = 0;
        if (NUE_FAILED(src_.read(src_.userdata, offset, window_.data(), want, &got)) || got < n) { win_len_ = 0; return nullptr; }
        win_off_ = offset; win_len_ = got;
        return window_.data();
    }
    // Copies up to n bytes; returns the number copied (stops at the end of the source).
    size_t read(uint64_t offset, void* dst, size_t n) {
        if (offset >= size_) return 0;
        n = size_t(std::min<uint64_t>(n, size_ - offset));
        if (mem_) { std::memcpy(dst, mem_ + offset, n); return n; }
        size_t got = 0;
        if (NUE_FAILED(src_.read(src_.userdata, offset, dst, n, &got))) return 0;
        return got;
    }
    // Whole source as one contiguous block (memory: no copy; callback source: read into `store`).
    const uint8_t* all(std::vector<uint8_t>& store) {
        if (mem_) return mem_;
        store.resize(size_t(size_));
        if (read(0, store.data(), store.size()) != store.size()) return nullptr;
        return store.data();
    }
private:
    static constexpr size_t kWindow = 64 * 1024;
    const uint8_t* mem_ = nullptr;
    nue_adec_source src_{};
    uint64_t size_ = 0;
    std::vector<uint8_t> window_;
    uint64_t win_off_ = 0;
    size_t win_len_ = 0;
};

inline uint16_t rd_le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t rd_le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
inline uint16_t rd_be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t rd_be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }

inline int16_t clamp16(int32_t v) { return int16_t(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

// A decoding backend produces frames from position() onward and can seek. Frames are interleaved float32.
class Backend {
public:
    virtual ~Backend() {}
    nue_adec_info info{};
    // Decodes up to `frames` frames at the current position. Returns the number produced (0 at the end). `corrupt` is set on bad data.
    virtual uint32_t read(float* out, uint32_t frames, bool* corrupt) = 0;
    virtual bool seek(uint64_t frame) = 0;
    uint64_t pos = 0;
};

std::unique_ptr<Backend> open_wav(ByteSource* src, const nue_adec_raw_desc* raw);                  // RIFF WAVE or raw PCM/ADPCM
std::unique_ptr<Backend> open_raw(ByteSource* src, const nue_adec_raw_desc& raw);
std::unique_ptr<Backend> open_flac(ByteSource* src);
std::unique_ptr<Backend> open_vorbis(ByteSource* src);
std::unique_ptr<Backend> open_opus(ByteSource* src);

inline void s16_to_f32(const int16_t* in, float* out, size_t n) {
    for (size_t i = 0; i < n; ++i) out[i] = float(in[i]) * (1.0f / 32768.0f);
}

}  // namespace nuea
