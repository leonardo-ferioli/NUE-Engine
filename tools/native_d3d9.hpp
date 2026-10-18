// The real Direct3D 9 runtime behind d3d9::Device, for the replay tool (ground truth). Windows only.
#pragma once
#include "nue/d3d9_device.hpp"
#include <memory>
#include <string>
#include <vector>

namespace nue::d3d9 {

class NativeCapture {
public:
    virtual ~NativeCapture() = default;
    virtual Device& device() = 0;
    virtual void set_draw_range(uint64_t first, uint64_t last) = 0;                    // only draws first..last are executed
    virtual bool capture(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height) = 0;   // the back buffer, BGRA, alpha 255
};

std::unique_ptr<NativeCapture> create_native_d3d9(void* hwnd, std::string& error);

}  // namespace nue::d3d9
