// Graphics detection and renderer selection policy for the bgfx back end on Linux.
//  - DRM devices from sysfs (vendor/device ids, kernel driver, boot VGA, VRAM)
//  - Vulkan ICD manifests (loader search path rules incl. VK_DRIVER_FILES / VK_ICD_FILENAMES)
//  - optional live probe of libvulkan.so.1 (dlopen; Linux only) for real device names/types
//  - plan_renderer(): Vulkan first, GL / GLES as fallback, GPU choice on multi-GPU machines
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lx_fs.hpp"
#include "lx_session.hpp"
#include "lx_xdg.hpp"

namespace nue::lx {

constexpr uint16_t kVendorAmd = 0x1002, kVendorNvidia = 0x10de, kVendorIntel = 0x8086;

const char* vendor_name(uint32_t vendor_id);

struct DrmDevice {
    std::string card;         // "card0"
    std::string render_node;  // "renderD128" or ""
    std::string pci_slot;     // "0000:03:00.0" or "" for platform devices
    uint32_t vendor = 0, device = 0;
    std::string kernel_driver;  // amdgpu, i915, xe, nvidia, nouveau, vc4, v3d, panfrost, msm, ...
    bool boot_vga = false;
    bool platform = false;      // SoC GPU (no PCI ids)
    uint64_t vram_bytes = 0;    // amdgpu dedicated VRAM, 0 if unknown/shared
    bool discrete_guess = false;
};
std::vector<DrmDevice> read_drm_devices(const IFileSystem& fs);

struct VulkanIcd {
    std::string manifest;
    std::string library_path;
    uint32_t api_version = 0;  // VK_MAKE_VERSION packed (major<<22|minor<<12|patch)
    std::string driver;        // radv, anv, nvidia, lavapipe, turnip, v3dv, ...
    bool software = false;
};
// Applies VK_DRIVER_FILES / VK_ICD_FILENAMES overrides, otherwise the standard search path.
std::vector<VulkanIcd> read_vulkan_icds(const IEnv& env, const IFileSystem& fs, const XdgDirs& xdg);
VulkanIcd parse_icd_manifest(const std::string& manifest_path, const std::string& json);
// Extracts a top-level-ish string field ("key": "value") from small JSON files. No unescaping beyond \" and \\.
bool json_string_field(const std::string& json, const std::string& key, std::string& value);

struct VulkanDeviceInfo {
    std::string name;
    uint32_t vendor_id = 0, device_id = 0, api_version = 0, driver_version = 0;
    int type = 0;  // VkPhysicalDeviceType: 1 integrated, 2 discrete, 3 virtual, 4 cpu
    bool software() const { return type == 4; }
};

struct VulkanProbe {
    bool attempted = false;
    bool loader_found = false;
    uint32_t instance_version = 0;
    bool instance_ok = false;
    bool ext_surface = false, ext_xcb = false, ext_xlib = false, ext_wayland = false;
    std::vector<VulkanDeviceInfo> devices;
    std::string error;
};
// Loads libvulkan.so.1 with dlopen and enumerates devices (instance only, no surface). On
// non-Linux builds it returns a probe with attempted=false.
VulkanProbe probe_vulkan();

// Texture-related capabilities of one physical device, read without creating a logical device or a surface.
struct VulkanCaps {
    bool ok = false;
    std::string error, device_name;
    uint32_t vendor_id = 0, device_id = 0;
    int device_type = 0;                // VkPhysicalDeviceType
    bool texture_bc = false, texture_etc2 = false, texture_astc_ldr = false;
    bool sampler_anisotropy = false, geometry_shader = false, tessellation_shader = false;
    uint32_t max_texture_2d = 0, max_texture_3d = 0, max_texture_cube = 0, max_array_layers = 0;
    uint64_t device_local_bytes = 0;    // largest DEVICE_LOCAL heap (VRAM on discrete GPUs, shared on integrated ones)
    std::vector<std::pair<int32_t, uint32_t>> format_features;  // (VkFormat, optimal-tiling VkFormatFeatureFlags) for the requested formats
};
// Picks the physical device matching prefer_vendor/prefer_device (0 = best: discrete, then integrated, never CPU unless alone).
VulkanCaps probe_vulkan_caps(uint32_t prefer_vendor, uint32_t prefer_device, const std::vector<int32_t>& vk_formats);

struct GlLibraries {
    bool egl = false, gles2 = false, gl = false, opengl = false;  // libEGL, libGLESv2, libGL, libOpenGL
};
GlLibraries probe_gl_libraries();  // dlopen-based; all false on non-Linux builds

enum class GfxApi { Vulkan, OpenGL, OpenGLES, None };
const char* to_string(GfxApi a);
const char* bgfx_renderer_name(GfxApi a);  // "Vulkan", "OpenGL", "OpenGLES", ""

struct GraphicsProbe {
    std::vector<DrmDevice> drm;
    std::vector<VulkanIcd> icds;
    VulkanProbe vk;
    GlLibraries gl;
};

struct PlanRequest {
    bool prefer_low_power = false;      // battery saver: integrated GPU first
    int device_index = -1;              // index into the candidate list (user override), -1 = auto
    uint32_t vendor_id = 0, device_id = 0;  // explicit choice from settings, 0 = auto
    bool force_api = false;
    GfxApi api = GfxApi::Vulkan;        // used only with force_api
    bool allow_software = false;        // accept llvmpipe/lavapipe without a warning-level note
};

struct RendererPlan {
    GfxApi api = GfxApi::None;
    std::vector<GfxApi> fallbacks;
    uint16_t bgfx_vendor_id = 0, bgfx_device_id = 0;  // bgfx::Init::vendorId/deviceId; 0 = let bgfx choose
    std::string device_name;
    bool software = false;
    bool hybrid_gpu = false;
    // Environment to set BEFORE the Vulkan loader / GL library is initialised.
    std::vector<std::pair<std::string, std::string>> env;
    std::vector<std::string> notes;
    std::vector<std::string> warnings;
};
RendererPlan plan_renderer(const GraphicsProbe& probe, const PlanRequest& req, const SessionInfo& session);

}  // namespace nue::lx
