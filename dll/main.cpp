// d3d9.dll entry points, configuration, logging, and IDirect3D9 (the factory).
// Capability questions (adapter identity, display modes, format and multisample support, device caps) are answered by the system's own Direct3D 9
// runtime, which is loaded from the Windows folder and only ever asked those questions (it never creates a device): the game then takes the same
// decisions it takes with the real driver. The device, and everything the game draws, is ours.
#include "facade.hpp"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>
#include <mutex>
#include <thread>
#include <typeinfo>

namespace nue::facade {

HMODULE g_module = nullptr;
const char* volatile g_last_call = "(none yet)";
std::atomic<unsigned> g_counter_present{0}, g_counter_draw{0}, g_counter_create{0}, g_counter_calls{0};

namespace {

std::atomic<int> g_devices{0};
FILE* g_log = nullptr;
std::mutex g_log_mutex;
Config g_config;
bool g_config_loaded = false;

std::string module_directory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string p = path;
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string ini_value(const std::string& ini, const char* key) {
    char buffer[512] = {};
    GetPrivateProfileStringA("graphics", key, "", buffer, sizeof buffer, ini.c_str());
    return buffer;
}

bool truthy(const std::string& v, bool fallback) {
    if (v.empty()) return fallback;
    return v == "1" || v == "true" || v == "on" || v == "yes";
}

void load_config() {
    if (g_config_loaded) return;
    g_config_loaded = true;
    g_config.dll_directory = module_directory();
    const std::string ini = g_config.dll_directory + "\\nue_d3d9.ini";
    // nue_d3d9.ini:  [graphics]  renderer=d3d11|vulkan   vsync=1   render_thread=1   log=1   shader_cache=<folder>
    if (auto v = ini_value(ini, "renderer"); !v.empty()) g_config.renderer = v;
    g_config.vsync = truthy(ini_value(ini, "vsync"), g_config.vsync);
    g_config.render_thread = truthy(ini_value(ini, "render_thread"), g_config.render_thread);
    g_config.log = truthy(ini_value(ini, "log"), g_config.log);
    g_config.log_unsupported = truthy(ini_value(ini, "log_unsupported"), g_config.log_unsupported);
    g_config.shader_cache = ini_value(ini, "shader_cache");
    if (const char* e = std::getenv("NUE_RENDERER")) g_config.renderer = e;   // environment wins
    if (const char* e = std::getenv("NUE_VSYNC")) g_config.vsync = truthy(e, g_config.vsync);
    if (const char* e = std::getenv("NUE_RENDER_THREAD")) g_config.render_thread = truthy(e, g_config.render_thread);
    if (g_config.shader_cache.empty()) g_config.shader_cache = g_config.dll_directory + "\\nue_shader_cache";
}

void log_line(const char* message) { log("%s", message); }

}  // namespace

const Config& config() { load_config(); return g_config; }

void log(const char* format, ...) {
    load_config();
    if (!g_config.log) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log) {
        g_log = std::fopen((g_config.dll_directory + "\\nue_d3d9.log").c_str(), "a");
        if (!g_log) { g_config.log = false; return; }
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void device_destroyed() { --g_devices; }

void apply_d3d9_fpu_mode(DWORD behaviour_flags) {
#if defined(_M_IX86) || defined(__i386__)
    unsigned short control = 0x007F;   // single precision, round to nearest, everything masked
    if (behaviour_flags & D3DCREATE_FPU_PRESERVE) {   // the caller's precision is kept, but invalid-operation, zero-divide, overflow and underflow are masked anyway
        __asm__ volatile("fnstcw %0" : "=m"(control));
        control |= 0x001D;
    }
    __asm__ volatile("fldcw %0" : : "m"(control));
#else
    (void)behaviour_flags;   // x86-64: SSE math, the x87 control word is not used
#endif
}

// ---- diagnostics: what happened when the game dies or stops ----------------------------------------------------------------------------

HRESULT trace_result(const char* method, HRESULT hr) {
    if (!FAILED(hr)) return hr;
    static std::mutex mutex;
    static std::map<std::string, int> seen;
    std::lock_guard<std::mutex> lock(mutex);
    char key[160];
    std::snprintf(key, sizeof key, "%s %08lx", method, static_cast<unsigned long>(hr));
    if (seen[key]++ < 3) log("call failed: %s returned 0x%08lx%s", method, static_cast<unsigned long>(hr), seen[key] == 3 ? " (not reported again)" : "");
    return hr;
}

HRESULT guard_failed(const char* method) {
    std::string what = "unknown";
    try { throw; } catch (const std::exception& e) { what = std::string(typeid(e).name()) + ": " + e.what(); } catch (...) {}
    log("C++ exception inside %s: %s", method, what.c_str());
    return D3DERR_INVALIDCALL;
}

namespace {

LONG CALLBACK exception_logger(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const void* address = info->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(address), &module);
    const bool ours = module == g_module;
    const bool fatal = code == EXCEPTION_STACK_OVERFLOW || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
    static std::atomic<int> cxx_logged{0};
    if (code == 0xE06D7363 && cxx_logged++ < 20) log("C++ exception thrown (first chance) while in %s", g_last_call);
    if (ours || fatal) {
        char name[MAX_PATH] = "?";
        if (module) GetModuleFileNameA(module, name, MAX_PATH);
        log("EXCEPTION %08lx at %p (%s+0x%llx) while in %s", static_cast<unsigned long>(code), address, name, (unsigned long long)(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)), g_last_call);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void on_terminate() {
    std::string what = "unknown";
    try { if (auto e = std::current_exception()) std::rethrow_exception(e); } catch (const std::exception& e) { what = std::string(typeid(e).name()) + ": " + e.what(); } catch (...) {}
    log("FATAL: std::terminate called (exception: %s) while in %s", what.c_str(), g_last_call);
    std::abort();
}

void on_abort(int) { log("FATAL: abort() called while in %s", g_last_call); }

void on_invalid_parameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned line, uintptr_t) {
    log("FATAL: invalid parameter passed to the C runtime (%ls in %ls, %ls:%u) while in %s", expression ? expression : L"?", function ? function : L"?", file ? file : L"?", line, g_last_call);
}

void heartbeat() {
    unsigned last = 0;
    int quiet = 0;
    for (;;) {
        Sleep(3000);
        const unsigned now = g_counter_calls.load();
        if (now != last) {
            log("alive: %u calls, %u presents, %u draws, %u resources created, in %s", now, g_counter_present.load(), g_counter_draw.load(), g_counter_create.load(), g_last_call);
            quiet = 0;
        } else if (++quiet == 1 || quiet % 10 == 0) log("no Direct3D 9 call for %d s (last: %s)", quiet * 3, g_last_call);
        last = now;
    }
}

void install_diagnostics() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::set_terminate(on_terminate);
        std::signal(SIGABRT, on_abort);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_invalid_parameter_handler(on_invalid_parameter);
        AddVectoredExceptionHandler(1, exception_logger);
        std::thread(heartbeat).detach();
    });
}

}  // namespace


// ---- IDirect3D9 -------------------------------------------------------------------------------------------------------------------------

Direct3D9::Direct3D9() {
    install_diagnostics();
    char system[MAX_PATH] = {};
    GetSystemDirectoryA(system, MAX_PATH);   // in a 32-bit process on 64-bit Windows this is SysWOW64
    const std::string path = std::string(system) + "\\d3d9.dll";
    if (HMODULE real = LoadLibraryA(path.c_str())) {
        using Create = IDirect3D9*(WINAPI*)(UINT);
        if (auto create = reinterpret_cast<Create>(GetProcAddress(real, "Direct3DCreate9"))) real_ = create(D3D_SDK_VERSION);
    }
    log("Direct3DCreate9: system runtime %s", real_ ? "available for capability queries" : "NOT available (using built-in defaults)");
}

Direct3D9::~Direct3D9() { if (real_) real_->Release(); }

UINT Direct3D9::GetAdapterCount() { return real_ ? real_->GetAdapterCount() : 1; }

HRESULT Direct3D9::GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9* id) {
    if (real_) return real_->GetAdapterIdentifier(adapter, flags, id);
    if (!id || adapter != 0) return D3DERR_INVALIDCALL;
    std::memset(id, 0, sizeof *id);
    std::strcpy(id->Driver, "nue_d3d9"); std::strcpy(id->Description, "nue_d3d9 (Direct3D 9 on bgfx)"); id->VendorId = 0x10DE; id->DeviceId = 0x0001;
    return D3D_OK;
}

UINT Direct3D9::GetAdapterModeCount(UINT adapter, D3DFORMAT format) { return real_ ? real_->GetAdapterModeCount(adapter, format) : 0; }
HRESULT Direct3D9::EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE* out) { return real_ ? real_->EnumAdapterModes(adapter, format, mode, out) : D3DERR_INVALIDCALL; }

HRESULT Direct3D9::GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE* mode) {
    if (real_) return real_->GetAdapterDisplayMode(adapter, mode);
    if (!mode) return D3DERR_INVALIDCALL;
    mode->Width = UINT(GetSystemMetrics(SM_CXSCREEN)); mode->Height = UINT(GetSystemMetrics(SM_CYSCREEN)); mode->RefreshRate = 60; mode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT Direct3D9::CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT back, BOOL windowed) { return real_ ? real_->CheckDeviceType(adapter, type, display, back, windowed) : D3D_OK; }
HRESULT Direct3D9::CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, DWORD usage, D3DRESOURCETYPE resource, D3DFORMAT check) { return real_ ? real_->CheckDeviceFormat(adapter, type, adapter_format, usage, resource, check) : D3D_OK; }
HRESULT Direct3D9::CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed, D3DMULTISAMPLE_TYPE multisample, DWORD* quality) {
    if (real_) return real_->CheckDeviceMultiSampleType(adapter, type, format, windowed, multisample, quality);
    if (quality) *quality = 1;
    return multisample == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
}
HRESULT Direct3D9::CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapter_format, D3DFORMAT render_target, D3DFORMAT depth) { return real_ ? real_->CheckDepthStencilMatch(adapter, type, adapter_format, render_target, depth) : D3D_OK; }
HRESULT Direct3D9::CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT source, D3DFORMAT target) { return real_ ? real_->CheckDeviceFormatConversion(adapter, type, source, target) : D3D_OK; }

HRESULT Direct3D9::GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9* caps) {
    if (!caps) return D3DERR_INVALIDCALL;
    if (real_) return real_->GetDeviceCaps(adapter, type, caps);
    return D3DERR_NOTAVAILABLE;
}

HMONITOR Direct3D9::GetAdapterMonitor(UINT adapter) { return real_ ? real_->GetAdapterMonitor(adapter) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY); }

HRESULT Direct3D9::CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behaviour, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (!pp || !out) return D3DERR_INVALIDCALL;
    *out = nullptr;
    HWND window = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    if (!window || adapter != 0) return D3DERR_INVALIDCALL;
    if (g_devices.load() > 0) { log("CreateDevice refused: a device already exists (bgfx is a singleton)"); return D3DERR_NOTAVAILABLE; }

    RECT client{};
    GetClientRect(window, &client);
    const UINT window_w = UINT(std::max<LONG>(client.right - client.left, 1)), window_h = UINT(std::max<LONG>(client.bottom - client.top, 1));
    const Config& cfg = config();

    gpu::BgfxOptions options;
    options.window = window;
    options.window_width = window_w; options.window_height = window_h;
    options.renderer = cfg.renderer == "vulkan" ? gpu::Renderer::vulkan : gpu::Renderer::d3d11;
    options.vsync = cfg.vsync && pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE;
    options.single_threaded = !cfg.render_thread;
    options.shader_cache_dir = cfg.shader_cache;
    options.log = log_line;
    std::string error;
    auto gpu_owner = gpu::create_bgfx_gpu(options, error);
    if (!gpu_owner) { log("CreateDevice failed: bgfx could not start (%s)", error.c_str()); return D3DERR_NOTAVAILABLE; }

    d3d9::LayerOptions layer_options;
    layer_options.log = log_line;
    layer_options.log_unsupported = cfg.log_unsupported;
    auto core = std::make_shared<Core>();
    core->gpu = std::move(gpu_owner);
    core->layer = d3d9::create_layer(*core->gpu, layer_options);

    // fill in what a game leaves to the runtime and make the layer's back buffer
    D3DPRESENT_PARAMETERS filled = *pp;
    if (!filled.BackBufferWidth) filled.BackBufferWidth = window_w;
    if (!filled.BackBufferHeight) filled.BackBufferHeight = window_h;
    if (filled.BackBufferFormat == D3DFMT_UNKNOWN) filled.BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!filled.BackBufferCount) filled.BackBufferCount = 1;
    d3d9::DeviceParams params;
    params.back_width = filled.BackBufferWidth; params.back_height = filled.BackBufferHeight; params.back_format = uint32_t(filled.BackBufferFormat);
    params.depth_format = filled.EnableAutoDepthStencil ? uint32_t(filled.AutoDepthStencilFormat) : 0;
    params.multisample = uint32_t(filled.MultiSampleType); params.buffers = filled.BackBufferCount; params.windowed = filled.Windowed != FALSE;
    params.present_interval = filled.PresentationInterval;
    if (core->layer->reset(params) != Status::ok) { log("CreateDevice failed: the layer refused the back buffer parameters"); return D3DERR_NOTAVAILABLE; }

    D3DCAPS9 caps{};
    GetDeviceCaps(adapter, type, &caps);
    apply_d3d9_fpu_mode(behaviour);
    D3DDEVICE_CREATION_PARAMETERS creation{adapter, type, focus, behaviour};
    *pp = filled;
    auto* device = new Device9(this, core, creation, filled, caps, window);
    ++g_devices;
    *out = device;
    log("CreateDevice: behaviour flags 0x%lx, %ux%u, back buffer format %d, depth %d, %s, interval %u, window %ux%u, renderer %s%s", static_cast<unsigned long>(behaviour), filled.BackBufferWidth, filled.BackBufferHeight, int(filled.BackBufferFormat),
        filled.EnableAutoDepthStencil ? int(filled.AutoDepthStencilFormat) : 0, filled.Windowed ? "windowed" : "fullscreen", unsigned(filled.PresentationInterval), window_w, window_h,
        core->gpu->stats().renderer_name, options.single_threaded ? "" : " (render thread)");
    return D3D_OK;
}

}  // namespace nue::facade

// ---- exports ----------------------------------------------------------------------------------------------------------------------------

#ifdef _WIN64
#define NUE_EXPORT(name, decorated) __pragma(comment(linker, "/EXPORT:" #name "=" #name))
#else
#define NUE_EXPORT(name, decorated) __pragma(comment(linker, "/EXPORT:" #name "=" decorated))
#endif

NUE_EXPORT(Direct3DCreate9, "_Direct3DCreate9@4")
NUE_EXPORT(Direct3DCreate9Ex, "_Direct3DCreate9Ex@8")
NUE_EXPORT(D3DPERF_BeginEvent, "_D3DPERF_BeginEvent@8")
NUE_EXPORT(D3DPERF_EndEvent, "_D3DPERF_EndEvent@0")
NUE_EXPORT(D3DPERF_SetMarker, "_D3DPERF_SetMarker@8")
NUE_EXPORT(D3DPERF_SetRegion, "_D3DPERF_SetRegion@8")
NUE_EXPORT(D3DPERF_QueryRepeatFrame, "_D3DPERF_QueryRepeatFrame@0")
NUE_EXPORT(D3DPERF_SetOptions, "_D3DPERF_SetOptions@4")
NUE_EXPORT(D3DPERF_GetStatus, "_D3DPERF_GetStatus@0")

extern "C" {
IDirect3D9* WINAPI Direct3DCreate9(UINT) { return new nue::facade::Direct3D9(); }
HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex**) { return D3DERR_NOTAVAILABLE; }
int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR) { return 0; }
int WINAPI D3DPERF_EndEvent() { return 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR, LPCWSTR) {}
void WINAPI D3DPERF_SetRegion(D3DCOLOR, LPCWSTR) {}
BOOL WINAPI D3DPERF_QueryRepeatFrame() { return FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD) {}
DWORD WINAPI D3DPERF_GetStatus() { return 0; }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { nue::facade::g_module = module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
