// Live graphics probes through dlopen. The Vulkan structures are declared here with their
// ABI layout so that no Vulkan SDK or header is needed to build the engine.
#include <cstdint>
#include <cstring>
#include <vector>

#include "lx_dl.hpp"
#include "lx_gpu.hpp"

namespace nue::lx {

#if defined(__linux__)

namespace {

using VkResult = int32_t;
using VkInstance = void*;
using VkPhysicalDevice = void*;
using PFN_void = void (*)();

struct VkApplicationInfo {
    int32_t sType;  // 0
    const void* pNext;
    const char* pApplicationName;
    uint32_t applicationVersion;
    const char* pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct VkInstanceCreateInfo {
    int32_t sType;  // 1
    const void* pNext;
    uint32_t flags;
    const VkApplicationInfo* pApplicationInfo;
    uint32_t enabledLayerCount;
    const char* const* ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* ppEnabledExtensionNames;
};

struct VkExtensionProperties {
    char extensionName[256];
    uint32_t specVersion;
};

// Only the fixed-size prefix of VkPhysicalDeviceProperties is read; the buffer is oversized.
struct DevicePropsPrefix {
    uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
    char deviceName[256];
};
static_assert(sizeof(VkExtensionProperties) == 260, "VkExtensionProperties layout");
static_assert(sizeof(DevicePropsPrefix) == 276, "VkPhysicalDeviceProperties prefix layout");

using PFN_vkGetInstanceProcAddr = PFN_void (*)(VkInstance, const char*);
using PFN_vkCreateInstance = VkResult (*)(const VkInstanceCreateInfo*, const void*, VkInstance*);
using PFN_vkDestroyInstance = void (*)(VkInstance, const void*);
using PFN_vkEnumerateInstanceVersion = VkResult (*)(uint32_t*);
using PFN_vkEnumerateInstanceExtensionProperties = VkResult (*)(const char*, uint32_t*, VkExtensionProperties*);
using PFN_vkEnumeratePhysicalDevices = VkResult (*)(VkInstance, uint32_t*, VkPhysicalDevice*);
using PFN_vkGetPhysicalDeviceProperties = void (*)(VkPhysicalDevice, void*);

}  // namespace

VulkanProbe probe_vulkan() {
    VulkanProbe p;
    p.attempted = true;
    DynLib lib;
    if (!lib.open("libvulkan.so.1", "libvulkan.so")) {
        p.error = "libvulkan.so.1 not found: " + lib.error();
        return p;
    }
    p.loader_found = true;
    auto gipa = lib.fn<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!gipa) {
        p.error = "vkGetInstanceProcAddr missing";
        return p;
    }
    auto enum_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
    p.instance_version = 1u << 22;  // 1.0 loader without vkEnumerateInstanceVersion
    if (enum_version) {
        uint32_t v = 0;
        if (enum_version(&v) == 0) p.instance_version = v;
    }
    auto enum_ext = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (enum_ext) {
        uint32_t n = 0;
        if (enum_ext(nullptr, &n, nullptr) == 0 && n > 0 && n < 1024) {
            std::vector<VkExtensionProperties> ext(n);
            if (enum_ext(nullptr, &n, ext.data()) == 0) {
                for (uint32_t i = 0; i < n; ++i) {
                    const char* e = ext[i].extensionName;
                    p.ext_surface |= std::strcmp(e, "VK_KHR_surface") == 0;
                    p.ext_xcb |= std::strcmp(e, "VK_KHR_xcb_surface") == 0;
                    p.ext_xlib |= std::strcmp(e, "VK_KHR_xlib_surface") == 0;
                    p.ext_wayland |= std::strcmp(e, "VK_KHR_wayland_surface") == 0;
                }
            }
        }
    }
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        p.error = "vkCreateInstance missing";
        return p;
    }
    VkApplicationInfo app{};
    app.sType = 0;
    app.pApplicationName = "nue_probe";
    app.pEngineName = "nue";
    app.apiVersion = (1u << 22);
    VkInstanceCreateInfo ci{};
    ci.sType = 1;
    ci.pApplicationInfo = &app;
    VkInstance inst = nullptr;
    VkResult r = create(&ci, nullptr, &inst);
    if (r != 0 || !inst) {
        p.error = "vkCreateInstance failed (VkResult " + std::to_string(r) + ")";
        return p;
    }
    p.instance_ok = true;
    auto enum_dev = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto get_props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    if (enum_dev && get_props) {
        VkPhysicalDevice devs[16] = {};
        uint32_t n = 16;
        VkResult er = enum_dev(inst, &n, devs);
        if (er == 0 || er == 5 /* VK_INCOMPLETE */) {
            for (uint32_t i = 0; i < n; ++i) {
                alignas(8) unsigned char buf[1024] = {};
                get_props(devs[i], buf);
                DevicePropsPrefix d;
                std::memcpy(&d, buf, sizeof d);
                VulkanDeviceInfo info;
                d.deviceName[255] = 0;
                info.name = d.deviceName;
                info.vendor_id = d.vendorID;
                info.device_id = d.deviceID;
                info.api_version = d.apiVersion;
                info.driver_version = d.driverVersion;
                info.type = static_cast<int>(d.deviceType);
                p.devices.push_back(std::move(info));
            }
        } else {
            p.error = "vkEnumeratePhysicalDevices failed (" + std::to_string(er) + ")";
        }
    }
    if (destroy) destroy(inst, nullptr);
    if (p.devices.empty() && p.error.empty()) p.error = "Vulkan loader works but no physical device is exposed";
    return p;
}

GlLibraries probe_gl_libraries() {
    GlLibraries g;
    DynLib l;
    g.egl = l.open("libEGL.so.1", "libEGL.so");
    g.gles2 = l.open("libGLESv2.so.2", "libGLESv2.so");
    g.gl = l.open("libGL.so.1", "libGL.so");
    g.opengl = l.open("libOpenGL.so.0");
    return g;
}

#else

VulkanProbe probe_vulkan() {
    VulkanProbe p;
    p.error = "live Vulkan probing is only available in Linux builds";
    return p;
}
GlLibraries probe_gl_libraries() { return {}; }

#endif

}  // namespace nue::lx
