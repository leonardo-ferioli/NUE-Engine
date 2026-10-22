// GPU probe: collects raw facts through Vulkan (loaded with dlopen, so devices without a driver are fine) and a
// throw-away EGL / OpenGL ES 3 context, then lets logic/gpu_caps.cpp interpret them.  Runs once on its own thread
// because creating a GL context must not touch the thread that later renders.
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "android_state.hpp"

using namespace nue::android;

namespace {

struct VkProbe {
    bool present = false;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0;
    char name[96] = {0};
    uint64_t device_local = 0;
    int max_msaa = 1;
    int max_tex = 0;
    VkFormatFacts facts;
};

template <typename T>
T load(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, const char* name) {
    return reinterpret_cast<T>(gipa(inst, name));
}

VkProbe probe_vulkan() {
    VkProbe r;
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return r;
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) return r;
    auto enum_version = load<PFN_vkEnumerateInstanceVersion>(gipa, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    auto create_instance = load<PFN_vkCreateInstance>(gipa, VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_instance) return r;
    uint32_t inst_version = VK_API_VERSION_1_0;
    if (enum_version) enum_version(&inst_version);

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "nue-probe";
    app.apiVersion = inst_version >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (create_instance(&ci, nullptr, &inst) != VK_SUCCESS || !inst) return r;

    auto destroy_instance = load<PFN_vkDestroyInstance>(gipa, inst, "vkDestroyInstance");
    auto enum_devices = load<PFN_vkEnumeratePhysicalDevices>(gipa, inst, "vkEnumeratePhysicalDevices");
    auto get_props = load<PFN_vkGetPhysicalDeviceProperties>(gipa, inst, "vkGetPhysicalDeviceProperties");
    auto get_features = load<PFN_vkGetPhysicalDeviceFeatures>(gipa, inst, "vkGetPhysicalDeviceFeatures");
    auto get_format = load<PFN_vkGetPhysicalDeviceFormatProperties>(gipa, inst, "vkGetPhysicalDeviceFormatProperties");
    auto get_mem = load<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa, inst, "vkGetPhysicalDeviceMemoryProperties");
    if (enum_devices && get_props && get_features && get_format && get_mem) {
        uint32_t count = 0;
        enum_devices(inst, &count, nullptr);
        std::vector<VkPhysicalDevice> devs(count);
        if (count && enum_devices(inst, &count, devs.data()) >= 0 && count) {
            // first non-CPU device, else the first one
            VkPhysicalDevice pick = devs[0];
            for (uint32_t i = 0; i < count; ++i) {
                VkPhysicalDeviceProperties p;
                get_props(devs[i], &p);
                if (p.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
                    pick = devs[i];
                    break;
                }
            }
            VkPhysicalDeviceProperties props;
            get_props(pick, &props);
            VkPhysicalDeviceFeatures feat;
            get_features(pick, &feat);
            r.present = true;
            r.api_version = props.apiVersion;
            r.driver_version = props.driverVersion;
            r.vendor_id = props.vendorID;
            std::strncpy(r.name, props.deviceName, sizeof r.name - 1);
            r.max_tex = static_cast<int>(props.limits.maxImageDimension2D);
            VkSampleCountFlags msaa = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
            for (int s = 64; s >= 1; s >>= 1)
                if (msaa & static_cast<VkSampleCountFlags>(s)) {
                    r.max_msaa = s;
                    break;
                }
            VkPhysicalDeviceMemoryProperties mem;
            get_mem(pick, &mem);
            for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
                if (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) r.device_local += mem.memoryHeaps[i].size;

            auto sampled = [&](VkFormat f) {
                VkFormatProperties fp;
                get_format(pick, f, &fp);
                const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                return (fp.optimalTilingFeatures & need) == need;
            };
            VkFormatFacts& f = r.facts;
            f.bc1 = feat.textureCompressionBC && sampled(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
            f.bc2 = feat.textureCompressionBC && sampled(VK_FORMAT_BC2_UNORM_BLOCK);
            f.bc3 = feat.textureCompressionBC && sampled(VK_FORMAT_BC3_UNORM_BLOCK);
            f.bc4 = feat.textureCompressionBC && sampled(VK_FORMAT_BC4_UNORM_BLOCK);
            f.bc5 = feat.textureCompressionBC && sampled(VK_FORMAT_BC5_UNORM_BLOCK);
            f.bc6h = feat.textureCompressionBC && sampled(VK_FORMAT_BC6H_UFLOAT_BLOCK);
            f.bc7 = feat.textureCompressionBC && sampled(VK_FORMAT_BC7_UNORM_BLOCK);
            f.etc2_rgb = feat.textureCompressionETC2 && sampled(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
            f.etc2_rgba8 = feat.textureCompressionETC2 && sampled(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
            f.etc2_rgb_a1 = feat.textureCompressionETC2 && sampled(VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK);
            f.astc_4x4 = feat.textureCompressionASTC_LDR && sampled(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
            // depth formats: attachment support is what matters
            auto depth_ok = [&](VkFormat fmt) {
                VkFormatProperties fp;
                get_format(pick, fmt, &fp);
                return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            };
            f.d24s8 = depth_ok(VK_FORMAT_D24_UNORM_S8_UINT);
            f.d32s8 = depth_ok(VK_FORMAT_D32_SFLOAT_S8_UINT);
            f.bgra8 = sampled(VK_FORMAT_B8G8R8A8_UNORM);
            f.sampler_anisotropy = feat.samplerAnisotropy != 0;
        }
    }
    if (destroy_instance) destroy_instance(inst, nullptr);
    return r;
}

struct GlProbe {
    bool ok = false;
    int major = 0, minor = 0;
    char renderer[96] = {0};
    char version[96] = {0};
    int max_tex = 0;
    std::string extensions;
};

GlProbe probe_gles() {
    GlProbe r;
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return r;
    const EGLint cfg_attr[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        eglTerminate(dpy);
        return r;
    }
    const EGLint pb_attr[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (surf != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT && eglMakeCurrent(dpy, surf, surf, ctx)) {
        const char* s = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        if (s) std::strncpy(r.renderer, s, sizeof r.renderer - 1);
        s = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if (s) std::strncpy(r.version, s, sizeof r.version - 1);
        s = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        if (s) r.extensions = s;
        GLint major = 0, minor = 0, maxtex = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxtex);
        r.major = major;
        r.minor = minor;
        r.max_tex = maxtex;
        r.ok = major >= 3;
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (ctx != EGL_NO_CONTEXT) eglDestroyContext(dpy, ctx);
    if (surf != EGL_NO_SURFACE) eglDestroySurface(dpy, surf);
    eglTerminate(dpy);
    return r;
}

} // namespace

namespace nue::android::native {

void run_gpu_probe() {
    State& st = state();
    VkProbe vk = probe_vulkan();
    GlProbe gl = probe_gles();

    NueAndroidGpuInfo g{};
    g.struct_size = sizeof g;
    g.vulkan_present = vk.present;
    g.vulkan_api_version = vk.api_version;
    g.vulkan_driver_version = vk.driver_version;
    g.vendor_id = vk.vendor_id;
    std::strncpy(g.device_name, vk.present ? vk.name : gl.renderer, sizeof g.device_name - 1);
    g.device_local_bytes = vk.device_local;
    g.gles_major = gl.major;
    g.gles_minor = gl.minor;
    std::strncpy(g.gles_renderer, gl.renderer, sizeof g.gles_renderer - 1);
    std::strncpy(g.gles_version, gl.version, sizeof g.gles_version - 1);
    g.vk_tex_caps = vk.present ? tex_caps_from_vulkan(vk.facts) : 0;
    g.gles_tex_caps = gl.ok ? tex_caps_from_gles(gl.major, gl.minor, gl.extensions) : 0;

    GpuVendor vendor = vk.present ? vendor_from_pci_id(vk.vendor_id) : GpuVendor::Unknown;
    if (vendor == GpuVendor::Unknown) vendor = vendor_from_name(g.device_name);
    if (vendor == GpuVendor::Unknown) vendor = vendor_from_name(gl.renderer);
    g.vendor = static_cast<int32_t>(vendor);

    int sdk;
    {
        std::lock_guard<std::mutex> lock(st.mu);
        sdk = st.device.sdk_int;
    }
    ApiFacts facts;
    facts.vulkan_present = vk.present;
    facts.vulkan_api_version = vk.api_version;
    facts.vendor = vendor;
    facts.adreno_gen = adreno_generation(g.device_name);
    if (!facts.adreno_gen) facts.adreno_gen = adreno_generation(gl.renderer);
    facts.sdk_int = sdk;
    facts.gles3 = gl.ok;
    ApiPick pick = pick_graphics_api(facts);
    g.prefer_vulkan = pick.api == ApiChoice::Vulkan;
    std::strncpy(g.note, pick.reason.c_str(), sizeof g.note - 1);
    g.max_texture_size = pick.api == ApiChoice::Vulkan ? vk.max_tex : gl.max_tex;
    g.max_msaa = vk.present ? vk.max_msaa : 1;

    {
        std::lock_guard<std::mutex> lock(st.mu);
        g.probed = 1;
        st.gpu = g;
    }
    NLOGI("gpu probe done: %s, Vulkan %u.%u, GLES %d.%d, recommend %s (%s)", g.device_name, vk_major(vk.api_version),
          vk_minor(vk.api_version), gl.major, gl.minor, g.prefer_vulkan ? "Vulkan" : "OpenGL ES", g.note);
}

} // namespace nue::android::native
