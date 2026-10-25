// The bgfx half of nue_gfx (compiled with NUE_WITH_GFX): bgfx::init / reset / frame / shutdown for a context in MANAGED mode, the bgfx callback that routes
// bgfx's log, fatal errors (device lost), shader/pipeline cache and screenshots into the engine, and the capability mapping (bgfx caps -> nue_gfx_caps,
// nue_gpu_caps, nue_tex_caps). Everything here runs on the render thread (the thread that created the context) except the callback, which bgfx may call
// from its own render thread. Threading and lifecycle rules: include/nue/gfx.h and docs/GFX.md.
#include "gfx_internal.h"

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

using namespace nuei::gfxi;

bgfx::RendererType::Enum to_bgfx(nue_renderer r) {
    switch (r) {
        case NUE_RENDERER_NOOP: return bgfx::RendererType::Noop;
        case NUE_RENDERER_D3D11: return bgfx::RendererType::Direct3D11;
        case NUE_RENDERER_D3D12: return bgfx::RendererType::Direct3D12;
        case NUE_RENDERER_VULKAN: return bgfx::RendererType::Vulkan;
        case NUE_RENDERER_METAL: return bgfx::RendererType::Metal;
        case NUE_RENDERER_OPENGL: return bgfx::RendererType::OpenGL;
        case NUE_RENDERER_GLES: return bgfx::RendererType::OpenGLES;
        default: return bgfx::RendererType::Count;
    }
}

// ---- the callback -------------------------------------------------------------------------------------------------------------------------

struct Impl;
struct Callback final : bgfx::CallbackI {
    Impl* owner = nullptr;
    void fatal(const char* file, uint16_t line, bgfx::Fatal::Enum code, const char* str) override;
    void traceVargs(const char* file, uint16_t line, const char* format, va_list args) override;
    void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
    void profilerEnd() override {}
    uint32_t cacheReadSize(uint64_t id) override;
    bool cacheRead(uint64_t id, void* data, uint32_t size) override;
    void cacheWrite(uint64_t id, const void* data, uint32_t size) override;
    void screenShot(const char* path, uint32_t w, uint32_t h, uint32_t pitch, bgfx::TextureFormat::Enum fmt, const void* data, uint32_t size, bool yflip) override;
    void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
    void captureEnd() override {}
    void captureFrame(const void*, uint32_t) override {}
};

struct Impl {
    nue_gfx g = nullptr;
    Callback callback;
    bgfx::RendererType::Enum type = bgfx::RendererType::Noop;
    uint32_t swap_flags = 0;
    uint64_t native_nwh = 0;                    // the handle bgfx was initialised with: a different one after a surface re-creation means a new device
    std::mutex capture_m;
    nue_gfx_capture_fn capture_fn = nullptr;
    void* capture_ud = nullptr;
    std::vector<uint8_t> scratch;
};

Impl* impl_of(nue_gfx g) { return static_cast<Impl*>(g->impl); }

void Callback::fatal(const char* file, uint16_t line, bgfx::Fatal::Enum code, const char* str) {
    NUE_LOGE("bgfx", "fatal (%d) %s:%u: %s", int(code), file ? file : "?", unsigned(line), str ? str : "");
    if (code == bgfx::Fatal::DeviceLost && owner) on_device_lost(owner->g);
    // Other codes: the failing object is missing from the picture; bgfx's own default would abort the process, which is worse for a game than a missing texture.
}

void Callback::traceVargs(const char* file, uint16_t line, const char* format, va_list args) {
    char buffer[1024];
    std::vsnprintf(buffer, sizeof buffer, format, args);
    size_t n = std::strlen(buffer);
    while (n && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r')) buffer[--n] = 0;
    for (char* p = buffer; *p; ++p) if (*p == '\n' || *p == '\r') *p = ' ';
    if (!n) return;
    // bgfx labels its own trace with the source line; warnings and errors of the backends are worth a warning, the rest is debug noise
    const bool bad = std::strstr(buffer, "rror") || std::strstr(buffer, "FAILED") || std::strstr(buffer, "ailed");
    (void)file; (void)line;
    if (bad) NUE_LOGW("bgfx", "%s", buffer); else NUE_LOGD("bgfx", "%s", buffer);
}

// Cache entries are files <id as 16 hex digits>.bin in the shader cache directory (pipeline caches, driver program binaries); a short header guards them.
std::string cache_path(Impl* o, uint64_t id) {
    if (!o) return std::string();
    char name[48];
    std::snprintf(name, sizeof name, "bgfx_%016llx.bin", static_cast<unsigned long long>(id));
    return shader_cache_file(o->g, name);
}

uint32_t Callback::cacheReadSize(uint64_t id) {
    const std::string p = cache_path(owner, id);
    if (p.empty()) return 0;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return 0;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fclose(f);
    return size > 0 ? uint32_t(size) : 0;
}

bool Callback::cacheRead(uint64_t id, void* data, uint32_t size) {
    const std::string p = cache_path(owner, id);
    if (p.empty()) return false;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    const size_t got = std::fread(data, 1, size, f);
    std::fclose(f);
    return got == size;
}

void Callback::cacheWrite(uint64_t id, const void* data, uint32_t size) {
    const std::string p = cache_path(owner, id);
    if (p.empty()) return;
    const std::string tmp = p + ".tmp";                 // written beside and renamed: a crash never leaves a half file under the real name
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), p.c_str()) != 0) std::remove(tmp.c_str());
}

void Callback::screenShot(const char*, uint32_t w, uint32_t h, uint32_t pitch, bgfx::TextureFormat::Enum fmt, const void* data, uint32_t size, bool yflip) {
    nue_gfx_capture_fn fn;
    void* ud;
    {
        std::lock_guard<std::mutex> lock(owner->capture_m);
        fn = owner->capture_fn; ud = owner->capture_ud;
        owner->capture_fn = nullptr;                     // one shot
    }
    if (!fn || !data) return;
    // hand out BGRA8 with the top row first, whatever bgfx read back
    const bool rgba = fmt == bgfx::TextureFormat::RGBA8;
    if (!rgba && fmt != bgfx::TextureFormat::BGRA8) { NUE_LOGW("gfx", "capture: unsupported read-back format %d", int(fmt)); return; }
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(data) + size_t(yflip ? h - 1 - y : y) * pitch;
        uint8_t* dst = out.data() + size_t(y) * w * 4;
        std::memcpy(dst, src, size_t(w) * 4);
        if (rgba) for (uint32_t x = 0; x < w; ++x) std::swap(dst[x * 4], dst[x * 4 + 2]);
    }
    (void)size;
    fn(ud, int32_t(w), int32_t(h), int32_t(w * 4), out.data());
}

// ---- helpers ------------------------------------------------------------------------------------------------------------------------------

uint32_t global_reset_flags(nue_gfx g) {
    uint32_t f = BGFX_RESET_NONE;
    if (g->applied_vsync) f |= BGFX_RESET_VSYNC;
    if (g->flags & NUE_GFX_MAXANISOTROPY) f |= BGFX_RESET_MAXANISOTROPY;
    return f;
}

uint32_t swap_chain_flags(nue_gfx g) {
    uint32_t f = BGFX_SWAP_CHAIN_NONE;
    const uint32_t m = g->desc.msaa_samples;
    if (m >= 16) f |= BGFX_SWAP_CHAIN_MSAA_X16; else if (m >= 8) f |= BGFX_SWAP_CHAIN_MSAA_X8; else if (m >= 4) f |= BGFX_SWAP_CHAIN_MSAA_X4; else if (m >= 2) f |= BGFX_SWAP_CHAIN_MSAA_X2;
    if (g->flags & NUE_GFX_SRGB_BACKBUFFER) f |= BGFX_SWAP_CHAIN_SRGB_BACKBUFFER;
    if (g->flags & NUE_GFX_HDR10) f |= BGFX_SWAP_CHAIN_HDR10;
    return f;
}

void choose_adapter_ids(uint16_t* vendor, uint16_t* device) {
    *vendor = BGFX_PCI_ID_NONE; *device = 0;
    nue_power_request req;
    NUE_STRUCT_INIT(&req);
    if (NUE_FAILED(nue_power_get_request(&req)) || req.gpu == NUE_GPU_DEFAULT) return;
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    if (NUE_FAILED(nue_system_get_info(&si)) || si.gpu_count < 2) return;             // a single adapter: nothing to choose
    const bool want_integrated = req.gpu == NUE_GPU_LOW_POWER;
    for (uint32_t i = 0; i < si.gpu_count && i < NUE_MAX_GPUS; ++i) {
        const nue_gpu_info& gi = si.gpus[i];
        if (gi.vendor_id && (gi.is_integrated != 0) == want_integrated) { *vendor = uint16_t(gi.vendor_id); *device = uint16_t(gi.device_id); return; }
    }
}

// ---- the backend --------------------------------------------------------------------------------------------------------------------------

bool be_renderer_compiled(nue_renderer r) {
    if (r == NUE_RENDERER_NOOP) return true;
    bgfx::RendererType::Enum list[bgfx::RendererType::Count];
    const uint8_t n = bgfx::getSupportedRenderers(uint8_t(bgfx::RendererType::Count), list);
    for (uint8_t i = 0; i < n; ++i) if (list[i] == to_bgfx(r)) return true;
    return false;
}

nue_result be_init(nue_gfx g, nue_renderer r, std::string& error) {
    nue_native_window nw;
    NUE_STRUCT_INIT(&nw);
    const bool have_window = window_native(g, &nw);
    uint64_t ndt = 0, nwh = 0;
    if (r != NUE_RENDERER_NOOP) {
        if (!have_window) { error = "the window has no native surface (headless or dummy video driver)"; return NUE_ERR_UNSUPPORTED; }
        native_handles(nw, &ndt, &nwh);
        if (!nwh) { error = "the native window handle is empty"; return NUE_ERR_UNSUPPORTED; }
    }
    Impl* impl = new (std::nothrow) Impl();
    if (!impl) { error = "out of memory"; return NUE_ERR_OUT_OF_MEMORY; }
    impl->g = g;
    impl->callback.owner = impl;
    impl->type = to_bgfx(r);
    impl->swap_flags = swap_chain_flags(g);
    impl->native_nwh = nwh;

    if (g->flags & NUE_GFX_SINGLE_THREADED) bgfx::renderFrame();      // before init: no render thread, the backend runs inside bgfx::frame (the calling thread must stay the same)

    bgfx::Init init;
    init.type = impl->type;
    choose_adapter_ids(&init.vendorId, &init.deviceId);
    init.debug = (g->flags & NUE_GFX_DEBUG) != 0;
    init.profile = false;
    init.fallback = false;                                              // the engine owns the fallback order (several candidates, config override)
    init.swapChain.nwh = reinterpret_cast<void*>(static_cast<uintptr_t>(nwh));
    init.swapChain.ndt = reinterpret_cast<void*>(static_cast<uintptr_t>(ndt));
    init.swapChain.width = uint32_t(std::max(g->applied_width, 1));
    init.swapChain.height = uint32_t(std::max(g->applied_height, 1));
    init.swapChain.flags = impl->swap_flags;
    init.platformData.type = nw.kind == NUE_NATIVE_WAYLAND_SURFACE ? bgfx::NativeWindowHandleType::Wayland : bgfx::NativeWindowHandleType::Default;
    init.reset = global_reset_flags(g);
    if (g->desc.transient_vertex_mb) init.limits.maxTransientVbSize = g->desc.transient_vertex_mb << 20;
    if (g->desc.transient_index_mb) init.limits.maxTransientIbSize = g->desc.transient_index_mb << 20;
    if (g->desc.max_draw_calls) init.limits.numDrawCalls = g->desc.max_draw_calls;
    init.callback = &impl->callback;
    g->impl = impl;                                                     // the callback may fire during init
    if (!bgfx::init(init)) {
        g->impl = nullptr;
        delete impl;
        error = "bgfx::init failed";
        return NUE_ERR_BACKEND;
    }
    const bgfx::Caps* caps = bgfx::getCaps();
    NUE_LOGI("gfx", "bgfx initialised: %s, vendor 0x%04x device 0x%04x, max texture %u, %s", bgfx::getRendererName(caps->rendererType), unsigned(caps->vendorId),
             unsigned(caps->deviceId), unsigned(caps->limits.maxTextureSize), (caps->supported & BGFX_CAPS_RENDERER_MULTITHREADED) ? "render thread" : "single threaded");
    if (g->desc.max_texture_handles > caps->limits.maxTextures)
        NUE_LOGW("gfx", "%u texture handles were requested but bgfx was compiled for %u (NUE_GFX_MAX_TEXTURES)", g->desc.max_texture_handles, caps->limits.maxTextures);
    if (g->desc.max_buffer_handles > std::min(caps->limits.maxVertexBuffers, caps->limits.maxIndexBuffers))
        NUE_LOGW("gfx", "%u buffer handles were requested but bgfx was compiled for %u vertex / %u index buffers", g->desc.max_buffer_handles, caps->limits.maxVertexBuffers, caps->limits.maxIndexBuffers);
    if (g->desc.max_framebuffer_handles > caps->limits.maxFrameBuffers)
        NUE_LOGW("gfx", "%u framebuffer handles were requested but bgfx was compiled for %u", g->desc.max_framebuffer_handles, caps->limits.maxFrameBuffers);
    return NUE_OK;
}

void be_shutdown(nue_gfx g) {
    Impl* impl = impl_of(g);
    if (!impl) return;
    bgfx::shutdown();
    g->impl = nullptr;
    delete impl;
    std::lock_guard<std::mutex> lock(g->m);
    g->bgfx_initialized = false;
}

nue_result be_reset(nue_gfx g, bool surface_changed) {
    Impl* impl = impl_of(g);
    if (surface_changed) {
        // bgfx keeps the window handle it was initialised with. When the native window really changed (Android/iOS surface re-created) the swap chain cannot
        // be re-pointed, so the device is rebuilt: everything created on it is gone (generation + NOTIFY_DEVICE_RESET tell the client).
        nue_native_window nw;
        uint64_t ndt = 0, nwh = 0;
        if (window_native(g, &nw)) native_handles(nw, &ndt, &nwh);
        if (nwh && nwh != impl->native_nwh) {
            NUE_LOGI("gfx", "the native window changed: recreating the device");
            const nue_renderer r = g->renderer;
            be_shutdown(g);
            std::string err;
            const nue_result ir = be_init(g, r, err);
            if (NUE_FAILED(ir)) return nuei::fail(ir, "recreating the device after a surface change failed: %s", err.c_str());
            { std::lock_guard<std::mutex> lock(g->m); g->bgfx_initialized = true; g->generation++; }
            return NUE_OK;
        }
    }
    bgfx::SwapChain sc;
    sc.width = uint32_t(std::max(g->applied_width, 1));
    sc.height = uint32_t(std::max(g->applied_height, 1));
    sc.flags = impl->swap_flags;
    bgfx::reset(global_reset_flags(g), &sc);
    return NUE_OK;
}

uint32_t be_frame(nue_gfx) { return bgfx::frame(); }

const char* adapter_name_for(uint16_t vendor, uint16_t device, char* buffer, size_t capacity) {
    buffer[0] = 0;
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    if (NUE_FAILED(nue_system_get_info(&si))) return buffer;
    const nue_gpu_info* best = nullptr;
    for (uint32_t i = 0; i < si.gpu_count && i < NUE_MAX_GPUS; ++i) {
        const nue_gpu_info& gi = si.gpus[i];
        if (gi.vendor_id == vendor && (gi.device_id == device || !best)) best = &gi;
        if (gi.vendor_id == vendor && gi.device_id == device) break;
    }
    if (!best) for (uint32_t i = 0; i < si.gpu_count && i < NUE_MAX_GPUS; ++i) if (si.gpus[i].is_default) best = &si.gpus[i];
    if (best) std::snprintf(buffer, capacity, "%s", best->name);
    return buffer;
}

nue_result be_get_caps(nue_gfx g, nue_gfx_caps* out) {
    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps) return NUE_ERR_BAD_STATE;
    std::snprintf(out->renderer_name, sizeof out->renderer_name, "%s", bgfx::getRendererName(caps->rendererType));
    adapter_name_for(caps->vendorId, caps->deviceId, out->adapter_name, sizeof out->adapter_name);
    out->max_texture_size = caps->limits.maxTextureSize;
    out->max_framebuffer_attachments = caps->limits.maxFBAttachments;
    nue_gpu_caps probe;
    NUE_STRUCT_INIT(&probe);
    nue_gpu_probe(&probe);
    out->max_anisotropy = probe.max_anisotropy ? probe.max_anisotropy : 16;
    out->compute = (caps->supported & BGFX_CAPS_COMPUTE) != 0;
    out->instancing = true;                                             // bgfx emulates instancing where the API lacks it; it has no cap bit any more
    out->texture_compare = true;
    out->hdr10 = (caps->supported & BGFX_CAPS_HDR10) != 0;
    out->occlusion_query = caps->limits.maxOcclusionQueries > 0;
    out->vendor_id = caps->vendorId;
    out->device_id = caps->deviceId;
    out->max_draw_calls = caps->limits.maxDrawCalls;
    out->max_texture_handles = caps->limits.maxTextures;
    out->max_buffer_handles = std::min(caps->limits.maxVertexBuffers, caps->limits.maxIndexBuffers);
    (void)g;
    return NUE_OK;
}

// A format family is usable when every bgfx format that belongs to it is. Families bgfx cannot express (D32F+S8, ASTC HDR) keep the probe's answer.
struct FamilyMap { nue_format_family family; std::vector<bgfx::TextureFormat::Enum> formats; };

const std::vector<FamilyMap>& family_map() {
    using F = bgfx::TextureFormat;
    static const std::vector<FamilyMap> m = [] {
        std::vector<FamilyMap> v;
        auto add = [&](nue_format_family f, std::initializer_list<bgfx::TextureFormat::Enum> l) { v.push_back({f, std::vector<bgfx::TextureFormat::Enum>(l)}); };
        add(NUE_FMT_RGBA8, {F::RGBA8}); add(NUE_FMT_BGRA8, {F::BGRA8}); add(NUE_FMT_RGB565, {F::R5G6B5}); add(NUE_FMT_RGBA4444, {F::RGBA4}); add(NUE_FMT_RGB5A1, {F::RGB5A1});
        add(NUE_FMT_R8, {F::R8}); add(NUE_FMT_RG8, {F::RG8}); add(NUE_FMT_R16F, {F::R16F}); add(NUE_FMT_RGBA16F, {F::RGBA16F}); add(NUE_FMT_RGBA32F, {F::RGBA32F});
        add(NUE_FMT_RGB10A2, {F::RGB10A2}); add(NUE_FMT_DEPTH16, {F::D16}); add(NUE_FMT_DEPTH24_STENCIL8, {F::D24S8}); add(NUE_FMT_DEPTH32F, {F::D32F});
        add(NUE_FMT_BC1, {F::BC1}); add(NUE_FMT_BC2, {F::BC2}); add(NUE_FMT_BC3, {F::BC3}); add(NUE_FMT_BC4, {F::BC4, F::BC4S}); add(NUE_FMT_BC5, {F::BC5, F::BC5S});
        add(NUE_FMT_BC6H, {F::BC6H, F::BC6HU}); add(NUE_FMT_BC7, {F::BC7});
        add(NUE_FMT_ETC1, {F::ETC1}); add(NUE_FMT_ETC2_RGB, {F::ETC2, F::ETC2A1}); add(NUE_FMT_ETC2_RGBA, {F::ETC2A});
        add(NUE_FMT_EAC_R11, {F::EACR11, F::EACR11S}); add(NUE_FMT_EAC_RG11, {F::EACRG11, F::EACRG11S});
        add(NUE_FMT_ASTC_LDR, {F::ASTC4x4, F::ASTC5x4, F::ASTC5x5, F::ASTC6x5, F::ASTC6x6, F::ASTC8x5, F::ASTC8x6, F::ASTC8x8, F::ASTC10x5, F::ASTC10x6, F::ASTC10x8,
                               F::ASTC10x10, F::ASTC12x10, F::ASTC12x12});
        add(NUE_FMT_PVRTC1, {F::PTC12, F::PTC14, F::PTC12A, F::PTC14A}); add(NUE_FMT_PVRTC2, {F::PTC22, F::PTC24}); add(NUE_FMT_ATC, {F::ATC, F::ATCE, F::ATCI});
        return v;
    }();
    return m;
}

bool is_depth_family(nue_format_family f) { return f == NUE_FMT_DEPTH16 || f == NUE_FMT_DEPTH24_STENCIL8 || f == NUE_FMT_DEPTH32F || f == NUE_FMT_DEPTH32F_STENCIL8; }

nue_result be_merge_gpu_caps(nue_gfx, nue_gpu_caps* c) {
    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps) return NUE_ERR_BAD_STATE;
    for (const FamilyMap& fm : family_map()) {
        uint32_t all = ~0u, any = 0;
        for (bgfx::TextureFormat::Enum f : fm.formats) { all &= caps->formats[f]; any |= caps->formats[f]; }
        nue_format_support s = 0;
        if (all & BGFX_CAPS_FORMAT_TEXTURE_2D) {
            s |= NUE_FMTSUP_SAMPLE | NUE_FMTSUP_MIPMAPS;
            const bool depth = is_depth_family(fm.family);
            if (!depth && fm.family != NUE_FMT_RGBA32F) s |= NUE_FMTSUP_FILTER;
            else if (c->format[fm.family] & NUE_FMTSUP_FILTER) s |= NUE_FMTSUP_FILTER;     // 32-bit float filtering: keep the platform probe's (conservative) answer
            if (all & BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB) s |= NUE_FMTSUP_SRGB;
            if (any & BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED) s |= NUE_FMTSUP_EMULATED;
            if (all & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER) { s |= NUE_FMTSUP_RENDER_TARGET; if (!depth) s |= NUE_FMTSUP_BLEND; }
            if (all & BGFX_CAPS_FORMAT_TEXTURE_IMAGE_WRITE) s |= NUE_FMTSUP_STORAGE;
            if (all & (BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER_MSAA | BGFX_CAPS_FORMAT_TEXTURE_MSAA)) s |= NUE_FMTSUP_MSAA;
        }
        c->format[fm.family] = s;
    }
    // D32F+S8 and ASTC HDR have no bgfx format: the probe's values stand (D32F_STENCIL8 is only usable when bgfx lists a depth format for it, which it does not)
    c->format[NUE_FMT_DEPTH32F_STENCIL8] = 0;
    c->probed = NUE_TRUE;
    c->renderer = [&] {
        switch (caps->rendererType) {
            case bgfx::RendererType::Direct3D11: return NUE_RENDERER_D3D11; case bgfx::RendererType::Direct3D12: return NUE_RENDERER_D3D12;
            case bgfx::RendererType::Vulkan: return NUE_RENDERER_VULKAN; case bgfx::RendererType::Metal: return NUE_RENDERER_METAL;
            case bgfx::RendererType::OpenGL: return NUE_RENDERER_OPENGL; case bgfx::RendererType::OpenGLES: return NUE_RENDERER_GLES;
            default: return NUE_RENDERER_NOOP;
        }
    }();
    c->max_texture_2d = c->max_texture_cube = caps->limits.maxTextureSize;
    c->max_texture_3d = c->max_texture_3d ? std::min(c->max_texture_3d, caps->limits.maxTextureSize) : std::min<uint32_t>(caps->limits.maxTextureSize, 2048);
    c->max_array_layers = caps->limits.maxTextureLayers;
    c->max_render_targets = caps->limits.maxFBAttachments;
    if (!c->max_anisotropy) c->max_anisotropy = 16;
    const bool msaa = (caps->formats[bgfx::TextureFormat::BGRA8] | caps->formats[bgfx::TextureFormat::RGBA8]) & BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER_MSAA;
    c->max_msaa_samples = msaa ? std::max<uint32_t>(c->max_msaa_samples, 4) : 1;     // bgfx falls back to the best the device supports
    c->compute = (caps->supported & BGFX_CAPS_COMPUTE) != 0;
    c->instancing = NUE_TRUE;
    c->texture_compare = NUE_TRUE;
    c->npot_mipmaps = NUE_TRUE;
    return NUE_OK;
}

nue_result be_get_tex_caps(nue_gfx g, nue_tex_caps* out) {
    const bgfx::Caps* caps = bgfx::getCaps();
    if (!caps) return NUE_ERR_BAD_STATE;
    uint16_t narrow[bgfx::TextureFormat::Count];                         // the texture module reads the low 16 flag bits (everything it uses)
    for (int i = 0; i < int(bgfx::TextureFormat::Count); ++i) narrow[i] = uint16_t(caps->formats[i] & 0xFFFFu);
    nue_tex_caps tc;
    NUE_STRUCT_INIT(&tc);
    nue_result r = nue_tex_caps_from_bgfx(&tc, narrow, uint32_t(bgfx::TextureFormat::Count), caps->limits.maxTextureSize);
    if (NUE_FAILED(r)) return r;
    tc.max_array_layers = caps->limits.maxTextureLayers;
    nue_gpu_caps probe;
    NUE_STRUCT_INIT(&probe);
    nue_gpu_probe(&probe);
    if (probe.max_anisotropy) tc.max_anisotropy = probe.max_anisotropy;
    tc.available_memory_bytes = probe.dedicated_memory_bytes;
    const uint32_t keep = out->struct_size ? out->struct_size : uint32_t(sizeof tc);
    std::memcpy(out, &tc, keep < sizeof tc ? keep : sizeof tc);
    out->struct_size = keep;
    (void)g;
    return r;
}

nue_result be_request_capture(nue_gfx g, nue_gfx_capture_fn fn, void* ud) {
    Impl* impl = impl_of(g);
    {
        std::lock_guard<std::mutex> lock(impl->capture_m);
        impl->capture_fn = fn; impl->capture_ud = ud;
    }
    bgfx::requestScreenShot(BGFX_INVALID_HANDLE, "nue");
    return NUE_OK;
}

const GfxBackend g_backend = {be_renderer_compiled, be_init, be_shutdown, be_reset, be_frame, be_get_caps, be_merge_gpu_caps, be_get_tex_caps, be_request_capture};

}  // namespace

namespace nuei {
namespace gfxi {
const GfxBackend* bgfx_backend() { return &g_backend; }
}  // namespace gfxi
}  // namespace nuei
