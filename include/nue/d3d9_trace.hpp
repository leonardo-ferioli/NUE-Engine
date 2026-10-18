// Direct3D 9 call traces: the file format, a writer (used by the recorder inside the native bridge), a reader and a headless replay into any
// d3d9::Device (docs/PLATFORM_API.md, 3.3 and 6). A trace is the sequence of Level A calls the game made, with the data they carried:
// backend-independent, so the same trace replays on the null device, on the Direct3D 9 layer with bgfx, and (through the bridge's own
// recording) is the ground truth for image comparison. Traces contain GAME DATA (textures, vertex buffers, shader bytecode): they stay on the
// user's disk and never go into git.
//
// File layout (little endian):
//   trace.d3d9   "NFD9TRC1", u32 version (1), then records:  u8 op, u32 payload_size, payload
//   blobs.bin    raw data blocks; a record refers to one as (u64 offset, u32 size); equal blocks are stored once
//   Objects are identified by the guest address of the COM object that held them (a 32-bit number, 0 = none).
// Ops 0..count-1 are d3d9::Method; the extra ops follow. Payloads are lists of u32 (floats as their bit pattern), blob references are 3 words
// (offset low, offset high, size); each op's payload is described next to the encoder in d3d9_trace.cpp.
#pragma once
#include "nue/d3d9_device.hpp"
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace nue::d3d9 {

enum Op : uint8_t {
    op_device_created = 128,   // (fixed numbers: the ops below do not move when a Level A method is added)   // adapter, behaviour flags, then the presentation parameters (8 words as in Method::reset)
    op_get_back_buffer,                           // surface object, swap chain, index
    op_frame_marker,                              // frame number, instructions of the main thread (low), (high)
    op_reference_image,                           // frame number, width, height, format, (no data: the image is frame_NNNNNN.bmp next to the trace)
    op_unknown_call,                              // vtable slot, argument words (up to 8)
    op_truncated,                                 // the size limit was reached: later data blobs are missing
    op_window_begin,                              // window index, frame: state and draw calls are recorded from here (a state snapshot follows)
    op_window_end,                                // window index, frame, draws, reference images
    op_end,
    op_checkpoint                                 // frame, complete windows so far: written and flushed at every window close, so a killed game still leaves a valid trace
};

struct Blob { uint64_t offset = 0; uint32_t size = 0; };

class TraceWriter {
public:
    // Creates `directory` (if needed) with trace.d3d9 and blobs.bin. `max_blob_bytes` bounds blobs.bin (0 = unlimited).
    bool open(const std::filesystem::path& directory, uint64_t max_blob_bytes, std::string& error);
    void close();
    void flush();   // everything written so far reaches the disk (a killed process keeps it)
    bool is_open() const { return trace_ != nullptr; }
    void record(uint8_t op, const std::vector<uint32_t>& payload);
    // Stores a data block (once per distinct content) and returns its reference; size 0 or a full disk gives {0, 0}.
    Blob blob(const void* data, uint32_t size);
    uint64_t records() const { return records_; }
    uint64_t blob_bytes() const { return blob_end_; }
    bool truncated() const { return truncated_; }
    ~TraceWriter() { close(); }
private:
    std::FILE* trace_ = nullptr;
    std::FILE* blobs_ = nullptr;
    uint64_t blob_end_ = 0, max_blob_bytes_ = 0, records_ = 0;
    bool truncated_ = false;
    std::unordered_map<uint64_t, Blob> index_;   // content hash (size mixed in) -> where the block is
};

struct Record { uint8_t op = 0; std::vector<uint32_t> words; };

class TraceReader {
public:
    bool open(const std::filesystem::path& directory, std::string& error);
    bool next(Record& out);                                   // false at the end
    std::vector<uint8_t> read_blob(uint64_t offset, uint32_t size) const;
    ~TraceReader() { close(); }
    void close();
private:
    std::FILE* trace_ = nullptr;
    std::FILE* blobs_ = nullptr;
};

struct ReplayStats {
    uint64_t records = 0, frames = 0, unsupported = 0, bad_objects = 0;
    uint64_t by_op[256] = {};
};

// Feeds a trace to a Level A device. Objects are re-created through the device and mapped from their recorded ids; lock/unlock pairs
// are replayed with the recorded data written into the staging memory. Stops after `max_frames` presents (0 = whole trace).
bool replay(TraceReader& reader, Device& device, ReplayStats& stats, std::string& error, uint64_t max_frames = 0);

}  // namespace nue::d3d9
