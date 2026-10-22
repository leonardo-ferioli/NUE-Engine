// GPU identification and texture-format capability policy.  Pure logic: the native probe (Vulkan and EGL)
// only collects raw facts and calls these functions.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace nue::android {

enum class GpuVendor : int32_t { Unknown = 0, Adreno, Mali, PowerVR, Xclipse, Nvidia, Amd, Intel, Software };

// Vulkan VkPhysicalDeviceProperties::vendorID: 0x5143 Qualcomm, 0x13B5 ARM, 0x1010 Imagination, 0x144D Samsung,
// 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, 0x10005 / 0x1AE0 software (Mesa llvmpipe, SwiftShader).
GpuVendor vendor_from_pci_id(uint32_t vendor_id);
// GL_RENDERER / VkPhysicalDeviceProperties::deviceName ("Adreno (TM) 740", "Mali-G715", "PowerVR D-Series ...",
// "Samsung Xclipse 940", "Android Emulator OpenGL ES Translator (...)").
GpuVendor vendor_from_name(std::string_view name);
const char* to_string(GpuVendor v);

// Adreno 6xx -> 6, "Adreno (TM) 740" -> 7; 0 if unknown.
int adreno_generation(std::string_view name);

enum TexCap : uint32_t {
    kTexBC1_3 = 1u << 0,
    kTexBC4_5 = 1u << 1,
    kTexBC6H_7 = 1u << 2,
    kTexETC2 = 1u << 3,
    kTexASTC_LDR = 1u << 4,
    kTexD24S8 = 1u << 5,
    kTexD32S8 = 1u << 6,
    kTexBGRA8 = 1u << 7,
    kTexAniso = 1u << 8
};

// Vulkan: the textureCompressionBC/ETC2/ASTC_LDR device features are necessary but only the format
// properties tell whether sampling with linear filtering is really supported, so the probe passes the result of
// vkGetPhysicalDeviceFormatProperties (SAMPLED_IMAGE | SAMPLED_IMAGE_FILTER_LINEAR) as booleans.
struct VkFormatFacts {
    bool bc1 = false, bc2 = false, bc3 = false, bc4 = false, bc5 = false, bc6h = false, bc7 = false;
    bool etc2_rgb = false, etc2_rgba8 = false, etc2_rgb_a1 = false;
    bool astc_4x4 = false;
    bool d24s8 = false, d32s8 = false;
    bool bgra8 = false;
    bool sampler_anisotropy = false;
};
uint32_t tex_caps_from_vulkan(const VkFormatFacts& f);

// OpenGL ES: extension string + version.  ES 3.0 mandates ETC2/EAC.  S3TC needs GL_EXT_texture_compression_s3tc
// (or the _dxt1 / _s3tc_srgb variants, or WEBGL-style GL_EXT_texture_compression_dxt1 + GL_ANGLE_texture_compression_dxt3/5).
uint32_t tex_caps_from_gles(int major, int minor, std::string_view extensions);

// The extension list is separated by spaces; exact token match, not substring.
bool has_extension(std::string_view list, std::string_view name);

enum class DxtStrategy : int32_t { NativeBC = 0, TranscodeETC2, TranscodeASTC, DecodeRGBA8 };
const char* to_string(DxtStrategy s);

// What the engine should do with the game's DXT1/3/5 textures.  Preference by memory and speed:
//   native BC (same size, no work) > ETC2 transcode (same size, fast block conversion, small quality loss)
//   > ASTC 4x4 (same size, slow to encode) > RGBA8 decode (4x-8x memory, exact).
DxtStrategy plan_dxt(uint32_t caps);

// Bytes per texel for one strategy, to size the texture budget.
float bytes_per_texel(DxtStrategy s, bool dxt1);

enum class ApiChoice { Vulkan, OpenGLES };

struct ApiPick {
    ApiChoice api = ApiChoice::OpenGLES;
    std::string reason;
};

struct ApiFacts {
    bool vulkan_present = false;
    uint32_t vulkan_api_version = 0;   // VK_MAKE_API_VERSION(variant, major, minor, patch)
    GpuVendor vendor = GpuVendor::Unknown;
    int adreno_gen = 0;
    int sdk_int = 0;
    bool gles3 = false;
};

// Default recommendation (the player can override it in the settings): Vulkan 1.1+ on Android 10+ where the
// driver is known to be solid, OpenGL ES 3.x otherwise.  Old Adreno (<= 5xx) and software renderers fall back.
ApiPick pick_graphics_api(const ApiFacts& f);

constexpr uint32_t vk_version(uint32_t major, uint32_t minor, uint32_t patch = 0) {
    return (major << 22) | (minor << 12) | patch;
}
constexpr uint32_t vk_major(uint32_t v) { return (v >> 22) & 0x7F; }
constexpr uint32_t vk_minor(uint32_t v) { return (v >> 12) & 0x3FF; }

} // namespace nue::android
