// Level A trace: a versioned record of every call a DirectSound client made on a Device, with the PCM it wrote, and a replayer that runs it again on a fresh
// Device without the client. This is the reverse-engineering workflow of docs/REVERSE_ENGINEERING.md for DirectSound:
//
//   record   wrap a Device with make_recording_device() (EngineConfig::trace, or NUE_DSOUND_TRACE=<file> for dsound.dll) and run the client once;
//   replay   replay_trace() feeds the same calls, in the same order, to a Device and checks every answer (HRESULTs, regions, cursors, status) and the hash of
//            every mixed block against the record: a mismatch names the record where the two runs diverged;
//   compare  two implementations of the client's audio (the translated original, a native reimplementation) are equal when the PCM they write into the
//            DirectSound buffers is equal: `pcm_hash` is the FNV-1a of every byte passed to Unlock, in order, independent of how the mixing is done below.
//
// Format (version 1, little endian): "NDST", u32 version, then records `u8 opcode, u32 payload bytes, payload`; the last record (opcode 255) carries the totals.
// Everything the client can observe (answers of every call that changes or reads state) is recorded; pure getters of constants are not.
// Thread: the recorder serialises the calls it records (one lock around the inner call and the append), so the order in the file is the order the Device
// executed them; `set_format` is the exception (it may reopen the output, which joins the mix thread), it is recorded right after it returns.
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nue_legacy/dsound_device.hpp"

namespace nue::dsound {

constexpr uint32_t trace_version = 1;

class TraceRecorder {
public:
    TraceRecorder();
    // The whole trace so far, closed with its totals record (the recorder can keep recording afterwards: call finish() again later).
    std::vector<uint8_t> finish() const;
    bool save(const std::string& path) const;   // finish() to a file
    uint64_t records() const;
    uint64_t pcm_hash() const;                  // FNV-1a over every byte written through Unlock so far
    // internal: used by the recording device
    std::mutex& mutex() { return mutex_; }
    void append(uint8_t opcode, const std::vector<uint8_t>& payload);
    void note_pcm(const uint8_t* data, size_t bytes);
    void note_render(uint32_t frames, const float* samples, uint32_t channels);

private:
    mutable std::mutex mutex_;
    std::vector<uint8_t> body_;
    uint64_t records_ = 0, pcm_hash_ = 0xcbf29ce484222325ull, mixed_hash_ = 0xcbf29ce484222325ull;
    uint64_t renders_ = 0, frames_ = 0, unlocks_ = 0, pcm_bytes_ = 0;
};

// A Device that forwards every call to `inner` and records it. The recorder must outlive the returned device.
std::unique_ptr<Device> make_recording_device(std::unique_ptr<Device> inner, TraceRecorder& recorder);

struct TraceInfo {
    bool valid = false;
    uint32_t version = 0;
    uint64_t records = 0, renders = 0, rendered_frames = 0, unlocks = 0, pcm_bytes = 0, pcm_hash = 0, mixed_hash = 0;
    std::string error;
};
TraceInfo trace_info(const uint8_t* data, size_t bytes);

struct ReplayResult {
    bool ok = false;
    uint64_t records = 0;           // replayed
    uint64_t pcm_hash = 0;          // of the bytes this replay passed to Unlock (equals the record's when the replay is faithful)
    uint64_t mixed_hash = 0;        // FNV-1a over the quantised mixed output of every render, in order
    std::string error;              // the first divergence: record number, operation, recorded and replayed answer
};
// Replays on `device`, which must be a fresh Device configured like the recorded one (same Config; the output layout and latency are replayed from the trace).
ReplayResult replay_trace(const uint8_t* data, size_t bytes, Device& device);

}  // namespace nue::dsound
