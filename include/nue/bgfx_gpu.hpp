// The bgfx adapter: gpu::Gpu on top of bgfx (Direct3D 11 / 12, Vulkan, OpenGL (ES), Metal). The game's frame is rendered into an offscreen
// colour target of the game's own size; present() draws it to the window (scaling is free) and calls bgfx::frame().
#pragma once
#include "nue/gpu.hpp"
#include <memory>
#include <string>

namespace nue::gpu {

enum class Renderer { automatic, d3d11, d3d12, vulkan, opengl, metal, noop };

struct BgfxOptions {
    void* window = nullptr;                  // native window handle (HWND on Windows); null with Renderer::noop
    uint32_t window_width = 1280, window_height = 720;
    Renderer renderer = Renderer::automatic;
    bool vsync = false;
    bool profile_draws = false;              // time every draw (four clock reads each: about 4 % of a draw); off in the dll, on in the replay tool's benchmark
    bool single_threaded = true;             // call bgfx::renderFrame() before init: no render thread (deterministic, easy to debug)
    uint32_t transient_vertex_mb = 64, transient_index_mb = 16;
    void (*log)(const char* message) = nullptr;   // bgfx's own messages: fatal errors and backend warnings (Direct3D errors), one string each
    std::string shader_cache_dir;            // compiled shaders are kept here between runs (empty: no disk cache); avoids a ~20 ms stall per new program
};

struct BgfxStats {
    uint64_t programs = 0, shader_builds = 0, views = 0, draws = 0;
    double cross_ms = 0, compile_ms = 0;     // SPIR-V -> HLSL/GLSL/MSL and the shader compiler, accumulated
    uint64_t failed_creates = 0;             // bgfx handles that could not be made (limits, memory): the object is missing in the picture
    double frame_call_ms = 0;                // the last frame: time the game thread spent inside bgfx::frame() (sorting and handing over the frame, and waiting for the render thread if it is behind)
    double wait_render_ms = 0, wait_submit_ms = 0;   // the last frame: the game thread waited for the render thread / the render thread waited for the game thread
    double gpu_ms = 0, render_cpu_ms = 0;    // the last frame: GPU time and the CPU time of the backend (render thread), from bgfx's own counters
    const char* renderer_name = "";
};

class BgfxGpu : public Gpu {
public:
    virtual const BgfxStats& stats() const = 0;
    virtual void set_window_size(uint32_t width, uint32_t height) = 0;
    virtual void reset_profile() = 0;
    virtual std::string profile_report() const = 0;   // time inside the adapter's draw and present
    virtual std::string audit() const = 0;            // handles in use against bgfx's limits, memory, failed creations
};

std::unique_ptr<BgfxGpu> create_bgfx_gpu(const BgfxOptions& options, std::string& error);

}  // namespace nue::gpu
