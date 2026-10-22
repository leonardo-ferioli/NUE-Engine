// GPU selection and health: DXGI adapter enumeration, the per-app GPU preference, device-loss classification and a recovery state machine,
// adapter-change detection (driver reinstall, GPU added/removed). The 3D device itself belongs to nue_d3d9 / bgfx; this module only reads
// and decides, so it works with Direct3D 9, 11 and 12 alike.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

enum class GpuVendor : uint32_t { Unknown = 0, Amd = 0x1002, Nvidia = 0x10DE, Intel = 0x8086, Microsoft = 0x1414, Qualcomm = 0x5143, Apple = 0x106B };
const char* vendor_name(uint32_t vendor_id);

struct GpuMemoryBudget {
    bool valid = false;
    uint64_t budget = 0, current_usage = 0, available_for_reservation = 0, current_reservation = 0;   // local (dedicated) segment, bytes
    uint64_t nonlocal_budget = 0, nonlocal_usage = 0;                                                 // system memory the GPU may use
};

struct GpuOutputInfo {
    std::wstring device_name;     // \\.\DISPLAY1
    RECT desktop_rect{};
    bool attached = false;
    bool hdr_enabled = false;     // IDXGIOutput6: the output is in the ST.2084 / BT.2020 colour space (Windows HDR switched on); see display.hpp for capability
    float max_luminance = 0, min_luminance = 0, max_full_frame_luminance = 0;
};

struct GpuAdapterInfo {
    uint32_t index = 0;           // in the enumeration order requested (high performance first when IDXGIFactory6 is present)
    std::string name;
    uint32_t vendor_id = 0, device_id = 0, subsystem_id = 0, revision = 0;
    LUID luid{};
    uint64_t dedicated_video_memory = 0, dedicated_system_memory = 0, shared_system_memory = 0;
    bool software = false;        // WARP / Basic Render Driver / remote
    bool integrated = false;      // shares system memory (heuristic, see gpu.cpp)
    bool discrete = false;
    uint32_t driver_version_parts[4] = {};   // UMD version as a.b.c.d (e.g. 32.0.15.6094)
    std::string driver_version;
    bool driver_version_valid = false;
    GpuMemoryBudget memory;
    std::vector<GpuOutputInfo> outputs;
    bool d3d11_level_11_0 = false, d3d12_level_11_0 = false;    // feature level 11_0 or better available (create-device probe, no device kept)
};

struct GpuEnumeration {
    std::vector<GpuAdapterInfo> adapters;
    bool factory6 = false;               // EnumAdapterByGpuPreference available (Windows 10 1803+)
    bool factory_is_current = true;      // false: the adapter set changed since the factory was created
    bool tearing_supported = false;      // DXGI_FEATURE_PRESENT_ALLOW_TEARING: variable refresh rate / uncapped present in windowed mode
    bool hybrid = false;                 // an integrated and a discrete (non-software) GPU are both present
    int high_performance_index = -1;     // adapter DXGI considers the high-performance choice
    int power_saving_index = -1;
};
// `with_probes`: also test D3D11/D3D12 feature level support per adapter (creates and releases devices; ~tens of ms).
Status enumerate_gpus(GpuEnumeration& out, bool with_memory_budget = true, bool with_probes = false);

// Heuristic used by enumerate_gpus (exposed for tests; no device is created, so a sleeping discrete GPU is not woken): by vendor, name
// and dedicated memory. With `with_probes` the D3D11 UnifiedMemoryArchitecture flag replaces it.
bool looks_integrated(uint32_t vendor_id, const std::string& name, uint64_t dedicated_video_memory);

// ---- Cheap GPU memory snapshot and a one-shot D3D11 capability probe -------------------------------------------------------------------------------
struct GpuMemorySnapshot {
    bool valid = false;
    uint64_t dedicated_total = 0;      // DedicatedVideoMemory of the chosen adapter (0 for integrated GPUs)
    uint64_t budget = 0, usage = 0;    // local segment as the OS grants it to this process (IDXGIAdapter3::QueryVideoMemoryInfo)
    uint64_t shared_total = 0;         // SharedSystemMemory
    bool unified = false;              // integrated / UMA by the same heuristic as enumerate_gpus
};
// The high-performance non-software adapter (the one a default device would use). Creates no device; cheap enough for once-per-second polling.
GpuMemorySnapshot query_gpu_memory();

// Format support and limits from a throw-away Direct3D 11 device on the high-performance adapter (a hardware device is created and released: this can wake
// a sleeping discrete GPU, so call it when the answer is needed, not at start-up). `dxgi_formats` are DXGI_FORMAT values.
struct D3D11Probe {
    bool ok = false;
    uint32_t feature_level = 0;                 // D3D_FEATURE_LEVEL value, e.g. 0xB000 = 11_0
    bool unified_memory = false;
    uint64_t dedicated_memory = 0;
    uint32_t max_msaa_samples = 1;              // for R8G8B8A8_UNORM
    std::vector<std::pair<uint32_t, uint32_t>> formats;    // (DXGI_FORMAT, D3D11_FORMAT_SUPPORT bits)
    std::vector<uint32_t> srgb_ok;                         // DXGI_FORMAT values of the sRGB variants that exist (those among the requested ones)
};
Status probe_d3d11(const std::vector<uint32_t>& dxgi_formats, D3D11Probe& out);

// ---- Per-application GPU preference (Windows Settings > Graphics) --------------------------------------------------------------
// HKCU\Software\Microsoft\DirectX\UserGpuPreferences, value name = full exe path, data "GpuPreference=N;". The setting is read when the
// process is created, so a change only takes effect for the next launch. Only HKCU is written (no admin).
enum class AppGpuPreference : int { Unspecified = 0, PowerSaving = 1, HighPerformance = 2 };
struct AppGpuPreferenceStore { const wchar_t* subkey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences"; };   // overridable for tests
Status get_app_gpu_preference(const std::wstring& exe_path, AppGpuPreference& out, const AppGpuPreferenceStore& store = {});
Status set_app_gpu_preference(const std::wstring& exe_path, AppGpuPreference value, const AppGpuPreferenceStore& store = {});
std::wstring current_exe_path();
// True when the running executable exports NvOptimusEnablement / AmdPowerXpressRequestHighPerformance (see gpu_exports.cpp).
struct DiscreteGpuExports { bool nvidia = false; bool amd = false; };
DiscreteGpuExports query_discrete_gpu_exports(HMODULE exe = nullptr);

// ---- Windows-level graphics settings that affect a game ---------------------------------------------------------------------------
struct GraphicsSystemSettings {
    Support hardware_scheduling = Support::Unknown;        // HAGS: HKLM\...\GraphicsDrivers\HwSchMode == 2
    Support game_mode_optimizations_windowed = Support::Unknown;   // "Optimizations for windowed games" (DirectXUserGlobalSettings SwapEffectUpgradeEnable)
    Support variable_refresh_rate_windowed = Support::Unknown;     // "Variable refresh rate" in Graphics settings (VRROptimizeEnable)
    uint32_t tdr_delay_seconds = 0;                        // TdrDelay if overridden (default 2 s), 0 = default
    bool tdr_disabled = false;                             // TdrLevel == 0
};
GraphicsSystemSettings query_graphics_system_settings();

// ---- Device loss ---------------------------------------------------------------------------------------------------------------------
// The user's driver reinstall showed what a game must survive: the device is lost or removed while the process runs.
enum class GpuLoss : uint8_t {
    None,
    DeviceLost,        // D3D9: D3DERR_DEVICELOST. Wait; the device returns to NOTRESET on its own when focus/mode allows it.
    DeviceNotReset,    // D3D9: D3DERR_DEVICENOTRESET. Release default-pool resources and call Reset().
    DeviceHung,        // GPU stopped responding (TDR fired or is about to)
    DeviceRemoved,     // driver reinstalled / GPU removed / TDR: the device is gone, create a new one (new adapter enumeration first)
    DeviceReset,       // DXGI_ERROR_DEVICE_RESET: the GPU was reset; recreate the device
    DriverInternalError,
    OutOfMemory,
    InvalidCall,       // a programming error, not a loss
    Unknown            // failure that is not a known loss code
};
GpuLoss classify_hresult(HRESULT hr);
const char* loss_name(GpuLoss l);
inline bool is_loss(GpuLoss l) { return l != GpuLoss::None && l != GpuLoss::InvalidCall && l != GpuLoss::Unknown && l != GpuLoss::OutOfMemory; }

// Decides what to do after each present/test of the device. Pure state machine (no API calls) so it is fully unit-testable; the engine
// executes the returned action and reports the outcome back.
class GpuRecovery {
public:
    enum class State : uint8_t { Running, Lost, Resetting, Recreating, Failed };
    enum class Action : uint8_t {
        Continue,        // nothing to do
        WaitAndRetry,    // sleep `retry_delay_ms()` then test the device again (D3D9: TestCooperativeLevel)
        ResetDevice,     // call Reset() on the existing device (after releasing default-pool resources)
        RecreateDevice,  // destroy and create the device (re-enumerate adapters first: the LUID may have changed)
        GiveUp           // too many consecutive failures: show a message and exit cleanly
    };
    struct Policy {
        int max_reset_attempts = 3;       // resets before escalating to a full device recreation
        int max_recreate_attempts = 3;    // recreations before giving up
        uint32_t initial_delay_ms = 100;
        uint32_t max_delay_ms = 2000;     // exponential backoff cap
    };
    GpuRecovery() = default;
    explicit GpuRecovery(const Policy& p) : policy_(p) {}
    // Feed the HRESULT of Present()/TestCooperativeLevel()/CheckDeviceState. Returns the action to take.
    Action on_result(HRESULT hr);
    // Report the outcome of ResetDevice/RecreateDevice.
    void on_reset_result(bool success);
    void on_recreate_result(bool success);
    State state() const { return state_; }
    GpuLoss last_loss() const { return last_loss_; }
    uint32_t retry_delay_ms() const { return delay_ms_; }
    int total_losses() const { return total_losses_; }
    int total_recoveries() const { return total_recoveries_; }
    const Policy& policy() const { return policy_; }
    // Hooks for the engine (called synchronously from on_result / on_*_result).
    std::function<void(GpuLoss)> on_lost;        // first failure of a loss episode: stop rendering, release resources
    std::function<void()> on_recovered;          // device usable again: reload resources
    std::function<void()> on_failed;             // GiveUp
private:
    void enter_lost(GpuLoss l);
    void recovered();
    Policy policy_;
    State state_ = State::Running;
    GpuLoss last_loss_ = GpuLoss::None;
    int reset_attempts_ = 0, recreate_attempts_ = 0, total_losses_ = 0, total_recoveries_ = 0;
    uint32_t delay_ms_ = 0;
};

// Detects "the set of GPUs or their drivers changed" (driver reinstall, eGPU, GPU disabled in Device Manager) without a device.
// Uses IDXGIFactory7::RegisterAdaptersChangedEvent when present, IsCurrent() otherwise.
class AdapterWatcher {
public:
    AdapterWatcher();
    ~AdapterWatcher();
    AdapterWatcher(const AdapterWatcher&) = delete;
    AdapterWatcher& operator=(const AdapterWatcher&) = delete;
    Status start();
    void stop();
    bool changed();            // true once after each change; cheap, call it once per frame or per second
    bool uses_event() const;   // false: polling IsCurrent()
    // Testing seam: pretend a change happened.
    void inject_change_for_test();
    // The LUID of every adapter seen at start(), to detect that the one in use disappeared.
    std::vector<LUID> adapters_at_start() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nue::win
