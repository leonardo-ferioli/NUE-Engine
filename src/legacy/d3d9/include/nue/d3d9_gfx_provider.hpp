// The engine's graphics context as the Direct3D 9 layer sees it (nue_gfx MANAGED mode; docs/GFX_D3D9_SEAM.md, docs/REQUESTS_gfx.md D2-D5).
// Optional: with no provider the drop-in d3d9.dll keeps its own behaviour (the bgfx adapter initialises bgfx, the window size is polled at Present,
// TestCooperativeLevel always succeeds, the shader cache is next to the dll). The interface has no nue_gfx type in it, so the layer builds without
// the engine; the engine side implements it on top of nue_gfx_get_state / nue_gfx_reset_device / nue_gfx_get_shader_cache_dir and its listener.
#pragma once
#include "nue/types.hpp"
#include <atomic>
#include <cstdint>
#include <string>

namespace nue::d3d9 {

// nue_gfx_state as Direct3D 9 needs it: OK; NEEDS_RESET (D3DERR_DEVICENOTRESET); SUSPENDED / DEVICE_LOST / ORPHANED (D3DERR_DEVICELOST).
enum class GfxState { ok, needs_reset, lost };

class GfxProvider {
public:
    virtual ~GfxProvider() = default;
    virtual GfxState state() const = 0;                 // any thread
    virtual bool reset_device() = 0;                    // in Reset after needs_reset, on the thread that owns bgfx: false = the device is still lost
    virtual bool owns_bgfx() const = 0;                 // bgfx was initialised by the engine (BgfxOptions::external_init) on the thread that creates the device
    virtual std::string shader_cache_dir() const = 0;   // native folder (created); empty = no opinion
};

// A window size reported from any thread (the provider's resize listener), applied at the next Present on the thread that owns bgfx.
class WindowSizeMailbox {
public:
    void post(uint32_t width, uint32_t height) { value_.store((1ull << 63) | (uint64_t(width & 0x7FFFFFFFu) << 32) | height, std::memory_order_release); }
    bool take(uint32_t& width, uint32_t& height) {   // the newest size since the last take, if any
        const uint64_t v = value_.exchange(0, std::memory_order_acquire);
        if (!(v >> 63)) return false;
        width = uint32_t(v >> 32) & 0x7FFFFFFFu; height = uint32_t(v);
        return true;
    }
private:
    std::atomic<uint64_t> value_{0};
};

// TestCooperativeLevel from the provider's state.
inline Status cooperative_level(const GfxProvider* provider) {
    if (!provider) return Status::ok;
    switch (provider->state()) {
    case GfxState::ok: return Status::ok;
    case GfxState::needs_reset: return Status::device_not_reset;
    default: return Status::device_lost;
    }
}

// What Reset may do: ok = go on (the device is usable, after a reset of it if it needed one); device_lost = Reset fails (suspended / lost).
// After a successful reset_device the GPU objects of the old device are gone: the caller re-creates what it owns.
inline Status prepare_reset(GfxProvider* provider, bool& device_was_reset) {
    device_was_reset = false;
    if (!provider) return Status::ok;
    switch (provider->state()) {
    case GfxState::ok: return Status::ok;
    case GfxState::needs_reset:
        if (!provider->reset_device()) return Status::device_lost;
        device_was_reset = true;
        return Status::ok;
    default: return Status::device_lost;
    }
}

// The shader cache folder: the ini's choice first, then the provider's, then the default next to the dll.
inline std::string choose_shader_cache_dir(const std::string& from_ini, const GfxProvider* provider, const std::string& fallback) {
    if (!from_ini.empty()) return from_ini;
    if (provider) { std::string dir = provider->shader_cache_dir(); if (!dir.empty()) return dir; }
    return fallback;
}

}  // namespace nue::d3d9
