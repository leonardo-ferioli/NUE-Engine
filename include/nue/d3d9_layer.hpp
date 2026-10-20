// nue_d3d9: the Direct3D 9 layer. It implements d3d9::Device (what a Direct3D 9 game says, handle-based, no COM and no guest pointers) on top of
// a gpu::Gpu (what a modern API does). All Direct3D 9 semantics live here: state shadow and state blocks, resource lifetime and locks, format
// conversion, vertex declarations, shader translation and the fixed-function pipeline, pass (render target) management, buffer policy.
#pragma once
#include "nue/d3d9_device.hpp"
#include "nue/gpu.hpp"
#include <memory>
#include <string>

namespace nue::d3d9 {

struct LayerOptions {
    // Dynamic buffer policy: a buffer that is rewritten after it was drawn from is served through transient copies of the ranges draws read.
    bool log_unsupported = true;
    // Keep a CPU copy of texture contents after upload (needed for lock-and-modify patterns on managed textures); costs memory.
    bool keep_texture_copies = false;
    // Dump the first N translated shader pairs' failures to the log callback.
    void (*log)(const char* message) = nullptr;
    // Debugging: only draws numbered first..last (counted over the whole replay) are executed.
    uint64_t debug_draw_first = 0, debug_draw_last = ~0ull, debug_draw_also = ~0ull;   // also execute this one draw
    bool debug_record_draws = false;
    uint32_t msaa_override = 0;        // 0 = as the game asks; 1 = no multisampling; 2, 4, 8, 16 = that many samples for every multisampled target
    bool profile = false;              // time the phases of every draw (Layer::profile_report)
    bool debug_log_passes = false;     // log every pass and copy as it is opened   // keep a description of the last draws (Layer::recent_draws)
};

// What the layer did, for tests, the replay tool and the performance work.
struct LayerStats {
    uint64_t draws = 0, draws_fixed_function = 0, draws_skipped = 0, draw_up = 0, passes = 0, clears = 0, partial_clears = 0, frames = 0;
    uint64_t static_buffer_uploads = 0, static_buffer_bytes = 0, transient_copies = 0, transient_bytes = 0, transient_dedupe_hits = 0, index_rewrites = 0, fan_conversions = 0;
    uint64_t texture_uploads = 0, texture_upload_bytes = 0, texture_converted_bytes = 0;
    uint64_t programs = 0, shader_translations = 0, shader_failures = 0, constants_bytes = 0;
    uint64_t stretch_rects = 0, unsupported_calls = 0, bad_handles = 0;
    uint64_t draw_by_primitive[8] = {};
};

class Layer : public Device {
public:
    virtual const LayerStats& stats() const = 0;
    virtual void flush() = 0;                 // closes the open pass so that everything drawn so far is submitted to the Gpu
    virtual std::string report() const = 0;
    virtual void reset_profile() = 0;
    virtual std::string profile_report() const = 0;   // where the time of the draws went (needs LayerOptions::profile)
    virtual std::string recent_draws() const = 0;   // the last draws described (needs LayerOptions::debug_record_draws)   // human-readable summary of unsupported features met so far
};

std::unique_ptr<Layer> create_layer(gpu::Gpu& gpu, const LayerOptions& options = {});

}  // namespace nue::d3d9
