/*
 * NUE engine - legacy ABI: the Direct3D 9 layer drawing into a nue_gfx context (library nue_d3d9_gfx, src/legacy/d3d9/gfx).
 *
 * How a host shows the picture of a Direct3D 9 game, the same on every platform:
 *
 *     nue_window_create(...)                   the window: SDL3 backend on desktop, the Android backend's ANativeWindow on Android
 *     nue_gfx_create(window, MANAGED)          bgfx on that window, renderer per platform (Direct3D 11 on Windows, Vulkan on Android)
 *     nue::d3d9::create_gfx_device(gfx, ...)   this header: the Direct3D 9 layer on that bgfx
 *     device().reset(params)                   the game's CreateDevice: back buffer size, format, depth
 *     ... the game's facade drives device() (Level A, nue/d3d9_device.hpp) ...
 *     device().present()                       the back buffer is fitted into the window; nue_gfx_present submits the frame
 *
 * The layer never creates, adopts or polls a window: the window's size reaches it through nue_gfx (NUE_GFX_NOTIFY_RESIZED) and only moves the
 * letterbox; the game's back buffer keeps the size the game asked for. The game's COM objects, guest pointers and calling convention stay in the
 * game's facade, outside the engine.
 *
 * Device loss, with Direct3D 9's meaning. While nue_gfx is SUSPENDED (Android: the surface is gone), DEVICE_LOST or ORPHANED, the device is lost:
 * test_cooperative_level() and present() return Status::device_lost and draws and clears do nothing. When nue_gfx re-creates its device (generation
 * changes: a new ANativeWindow on Android, nue_gfx_reset_device), the layer makes its GPU objects again at once (textures from their kept copies,
 * see LayerOptions::texture_copy_mb) and reports Status::device_not_reset until the game calls reset(), as Direct3D 9 does after a lost device.
 *
 * Thread: every call on the render thread, the thread that called nue_gfx_create. The nue_gfx context must outlive the device.
 * C++17; no operating system type, no guest pointer.
 */
#ifndef NUE_LEGACY_D3D9_GFX_HPP
#define NUE_LEGACY_D3D9_GFX_HPP

#include "nue/gfx.h"
#include <cstdint>
#include <memory>
#include <string>

namespace nue::d3d9 {

class Device;          // Level A: nue/d3d9_device.hpp (library nue_d3d9)
class Layer;           // the layer itself: nue/d3d9_layer.hpp
struct LayerOptions;   // nue/d3d9_layer.hpp

struct GfxDeviceStats {
    uint32_t generation = 0;       // nue_gfx_native.generation the layer's GPU objects belong to
    uint64_t device_resets = 0;    // times the layer re-made its GPU objects for a new nue_gfx device
    uint64_t lost_presents = 0;    // presents refused while the device was lost
    uint64_t lost_draws = 0;       // draws and clears dropped while the device was lost
};

class GfxDevice {
public:
    virtual ~GfxDevice() = default;
    // Level A for the game's facade. Its test_cooperative_level, present and reset follow nue_gfx's state (see above); everything else is the layer's.
    virtual Device& device() = 0;
    virtual GfxDeviceStats stats() const = 0;
    virtual Layer& layer() = 0;   // statistics, reports and debug aids of the layer (LayerStats, audit())
};

// Makes the layer on `gfx`, which must be a MANAGED context (bgfx initialised by nue_gfx_create; NUE_GFX_PREPARE_ONLY is refused) created on this
// thread. Compiled shaders are kept in nue_gfx's shader cache (nue_gfx_desc.shader_cache_dir), and LayerOptions::program_list defaults to
// nue_programs.txt there. Null and `error` set on failure.
std::unique_ptr<GfxDevice> create_gfx_device(nue_gfx gfx, const LayerOptions& options, std::string& error);

}  // namespace nue::d3d9

#endif  /* NUE_LEGACY_D3D9_GFX_HPP */
