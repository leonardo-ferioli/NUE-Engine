#include "gpu_caps.hpp"

#include <algorithm>
#include <cctype>

namespace nue::android {

GpuVendor vendor_from_pci_id(uint32_t id) {
    switch (id) {
    case 0x5143: return GpuVendor::Adreno;
    case 0x13B5: return GpuVendor::Mali;
    case 0x1010: return GpuVendor::PowerVR;
    case 0x144D: return GpuVendor::Xclipse;
    case 0x10DE: return GpuVendor::Nvidia;
    case 0x1002: return GpuVendor::Amd;
    case 0x8086: return GpuVendor::Intel;
    case 0x10005:
    case 0x1AE0: return GpuVendor::Software;
    default: return GpuVendor::Unknown;
    }
}

namespace {
std::string lower(std::string_view s) {
    std::string o(s);
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return o;
}
bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }
} // namespace

GpuVendor vendor_from_name(std::string_view name) {
    std::string n = lower(name);
    if (contains(n, "swiftshader") || contains(n, "llvmpipe") || contains(n, "software") || contains(n, "emulator") ||
        contains(n, "angle (google"))
        return GpuVendor::Software;
    if (contains(n, "adreno")) return GpuVendor::Adreno;
    if (contains(n, "mali")) return GpuVendor::Mali;
    if (contains(n, "immortalis")) return GpuVendor::Mali; // Arm's ray tracing Mali family
    if (contains(n, "powervr")) return GpuVendor::PowerVR;
    if (contains(n, "xclipse")) return GpuVendor::Xclipse;
    if (contains(n, "nvidia") || contains(n, "tegra")) return GpuVendor::Nvidia;
    if (contains(n, "amd") || contains(n, "radeon")) return GpuVendor::Amd;
    if (contains(n, "intel")) return GpuVendor::Intel;
    return GpuVendor::Unknown;
}

const char* to_string(GpuVendor v) {
    switch (v) {
    case GpuVendor::Unknown: return "unknown";
    case GpuVendor::Adreno: return "Adreno";
    case GpuVendor::Mali: return "Mali";
    case GpuVendor::PowerVR: return "PowerVR";
    case GpuVendor::Xclipse: return "Xclipse";
    case GpuVendor::Nvidia: return "NVIDIA";
    case GpuVendor::Amd: return "AMD";
    case GpuVendor::Intel: return "Intel";
    case GpuVendor::Software: return "software";
    }
    return "?";
}

int adreno_generation(std::string_view name) {
    std::string n = lower(name);
    size_t p = n.find("adreno");
    if (p == std::string::npos) return 0;
    // first digit sequence after "adreno": "adreno (tm) 740" -> 740 -> 7; "adreno 506" -> 5
    for (size_t i = p + 6; i < n.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(n[i]))) return n[i] - '0';
    }
    return 0;
}

uint32_t tex_caps_from_vulkan(const VkFormatFacts& f) {
    uint32_t c = 0;
    if (f.bc1 && f.bc2 && f.bc3) c |= kTexBC1_3;
    if (f.bc4 && f.bc5) c |= kTexBC4_5;
    if (f.bc6h && f.bc7) c |= kTexBC6H_7;
    if (f.etc2_rgb && f.etc2_rgba8 && f.etc2_rgb_a1) c |= kTexETC2;
    if (f.astc_4x4) c |= kTexASTC_LDR;
    if (f.d24s8) c |= kTexD24S8;
    if (f.d32s8) c |= kTexD32S8;
    if (f.bgra8) c |= kTexBGRA8;
    if (f.sampler_anisotropy) c |= kTexAniso;
    return c;
}

bool has_extension(std::string_view list, std::string_view name) {
    size_t pos = 0;
    while (pos < list.size()) {
        while (pos < list.size() && list[pos] == ' ') ++pos;
        size_t end = list.find(' ', pos);
        if (end == std::string_view::npos) end = list.size();
        if (list.substr(pos, end - pos) == name) return true;
        pos = end;
    }
    return false;
}

uint32_t tex_caps_from_gles(int major, int minor, std::string_view ext) {
    (void)minor;
    uint32_t c = 0;
    if (has_extension(ext, "GL_EXT_texture_compression_s3tc") ||
        (has_extension(ext, "GL_EXT_texture_compression_dxt1") && has_extension(ext, "GL_ANGLE_texture_compression_dxt3") &&
         has_extension(ext, "GL_ANGLE_texture_compression_dxt5")))
        c |= kTexBC1_3;
    if (has_extension(ext, "GL_EXT_texture_compression_rgtc")) c |= kTexBC4_5;
    if (has_extension(ext, "GL_EXT_texture_compression_bptc")) c |= kTexBC6H_7;
    if (major >= 3) c |= kTexETC2 | kTexD24S8; // mandated by ES 3.0
    if (has_extension(ext, "GL_KHR_texture_compression_astc_ldr") || has_extension(ext, "GL_KHR_texture_compression_astc_hdr"))
        c |= kTexASTC_LDR;
    if (has_extension(ext, "GL_EXT_texture_format_BGRA8888") || has_extension(ext, "GL_APPLE_texture_format_BGRA8888"))
        c |= kTexBGRA8;
    if (has_extension(ext, "GL_EXT_texture_filter_anisotropic")) c |= kTexAniso;
    return c;
}

const char* to_string(DxtStrategy s) {
    switch (s) {
    case DxtStrategy::NativeBC: return "native BC (DXT blocks uploaded unchanged)";
    case DxtStrategy::TranscodeETC2: return "transcode to ETC2";
    case DxtStrategy::TranscodeASTC: return "transcode to ASTC 4x4";
    case DxtStrategy::DecodeRGBA8: return "decode to RGBA8";
    }
    return "?";
}

DxtStrategy plan_dxt(uint32_t caps) {
    if (caps & kTexBC1_3) return DxtStrategy::NativeBC;
    if (caps & kTexETC2) return DxtStrategy::TranscodeETC2;
    if (caps & kTexASTC_LDR) return DxtStrategy::TranscodeASTC;
    return DxtStrategy::DecodeRGBA8;
}

float bytes_per_texel(DxtStrategy s, bool dxt1) {
    switch (s) {
    case DxtStrategy::NativeBC:
    case DxtStrategy::TranscodeETC2:
        return dxt1 ? 0.5f : 1.0f;
    case DxtStrategy::TranscodeASTC: return 1.0f; // 4x4 at 128 bit per block, there is no 64 bit ASTC block
    case DxtStrategy::DecodeRGBA8: return 4.0f;
    }
    return 4.0f;
}

ApiPick pick_graphics_api(const ApiFacts& f) {
    ApiPick p;
    if (!f.vulkan_present) {
        p.api = ApiChoice::OpenGLES;
        p.reason = "no Vulkan device";
        return p;
    }
    if (f.vulkan_api_version < vk_version(1, 1)) {
        p.api = ApiChoice::OpenGLES;
        p.reason = "Vulkan older than 1.1";
        return p;
    }
    if (f.vendor == GpuVendor::Software) {
        p.api = ApiChoice::OpenGLES;
        p.reason = "software Vulkan implementation";
        return p;
    }
    if (f.vendor == GpuVendor::Adreno && f.adreno_gen > 0 && f.adreno_gen < 6) {
        p.api = ApiChoice::OpenGLES; // Adreno 5xx Vulkan drivers have many known bugs; their GLES driver is mature
        p.reason = "Adreno 5xx or older: Vulkan driver unreliable";
        return p;
    }
    if (f.sdk_int && f.sdk_int < 29) {
        p.api = ApiChoice::OpenGLES;
        p.reason = "Android older than 10";
        return p;
    }
    p.api = ApiChoice::Vulkan;
    p.reason = "Vulkan 1.1+ on a supported GPU";
    return p;
}

} // namespace nue::android
