#include "nue_win/gpu.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwchar>
#include <mutex>

#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace nue::win {

const char* vendor_name(uint32_t id) {
    switch (id) {
        case 0x1002: case 0x1022: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x8086: case 0x8087: return "Intel";
        case 0x1414: return "Microsoft";
        case 0x5143: return "Qualcomm";
        case 0x106B: return "Apple";
        case 0x13B5: return "ARM";
        default: return "unknown";
    }
}

namespace {
std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }
bool contains(const std::string& hay, const char* needle) { return lower(hay).find(needle) != std::string::npos; }
constexpr uint64_t GiB = 1ull << 30;
}  // namespace

bool looks_integrated(uint32_t vendor, const std::string& name, uint64_t dedicated) {
    switch (vendor) {
        case 0x10DE: return false;                         // NVIDIA has no PC integrated GPU
        case 0x8086: return dedicated < GiB;               // Arc A/B-series and Data Center GPUs report GiBs; UHD/Iris/Xe/Arc iGPU report MiBs
        case 0x1002: case 0x1022: {
            if (contains(name, " rx ") || contains(name, "radeon pro") || contains(name, "radeon vii") || contains(name, "firepro") || contains(name, "instinct")) return false;
            if (contains(name, "radeon(tm) graphics") || contains(name, "radeon graphics") || contains(name, "vega") || contains(name, "radeon r")) return dedicated <= 4 * GiB;
            // "Radeon 780M" / "Radeon 890M": the trailing M with digits marks the APU parts
            const std::string l = lower(name);
            for (size_t i = 1; i < l.size(); ++i) if (l[i] == 'm' && std::isdigit((unsigned char)l[i - 1]) && (i + 1 == l.size() || l[i + 1] == ' ')) return true;
            return false;
        }
        default: return dedicated < 512ull * 1024 * 1024;
    }
}

namespace {

std::string version_string(const uint32_t p[4]) {
    char b[64];
    std::snprintf(b, sizeof b, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
    return b;
}

void fill_outputs(IDXGIAdapter1* adapter, GpuAdapterInfo& info) {
    for (UINT o = 0;; ++o) {
        ComPtr<IDXGIOutput> out;
        if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND) break;
        GpuOutputInfo oi;
        DXGI_OUTPUT_DESC d{};
        if (SUCCEEDED(out->GetDesc(&d))) { oi.device_name = d.DeviceName; oi.desktop_rect = d.DesktopCoordinates; oi.attached = d.AttachedToDesktop != FALSE; }
        ComPtr<IDXGIOutput6> out6;
        if (SUCCEEDED(out.As(&out6))) {
            DXGI_OUTPUT_DESC1 d1{};
            if (SUCCEEDED(out6->GetDesc1(&d1))) {
                oi.hdr_enabled = d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                oi.max_luminance = d1.MaxLuminance;
                oi.min_luminance = d1.MinLuminance;
                oi.max_full_frame_luminance = d1.MaxFullFrameLuminance;
            }
        }
        info.outputs.push_back(std::move(oi));
    }
}

void probe_adapter(IDXGIAdapter1* adapter, GpuAdapterInfo& info) {
    using D3D11Create = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    using D3D12Create = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    if (auto create = load_proc<D3D11Create>(L"d3d11.dll", "D3D11CreateDevice")) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> dev;
        D3D_FEATURE_LEVEL got{};
        if (SUCCEEDED(create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, nullptr))) {
            info.d3d11_level_11_0 = got >= D3D_FEATURE_LEVEL_11_0;
            D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2))) {
                info.integrated = o2.UnifiedMemoryArchitecture != FALSE;
                info.discrete = !info.integrated && !info.software;
            }
        }
    }
    if (auto create12 = load_proc<D3D12Create>(L"d3d12.dll", "D3D12CreateDevice"))
        info.d3d12_level_11_0 = SUCCEEDED(create12(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
}

}  // namespace

Status enumerate_gpus(GpuEnumeration& out, bool with_memory_budget, bool with_probes) {
    out = GpuEnumeration{};
    ComPtr<IDXGIFactory1> factory1;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory1));
    if (FAILED(hr)) return Status::hresult("CreateDXGIFactory1", hr);
    ComPtr<IDXGIFactory6> factory6;
    out.factory6 = SUCCEEDED(factory1.As(&factory6));
    out.factory_is_current = factory1->IsCurrent() != FALSE;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory1.As(&factory5))) {
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) out.tearing_supported = tearing != FALSE;
    }

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory6) { if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) break; }
        else if (factory1->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(adapter->GetDesc1(&d))) continue;
        GpuAdapterInfo info;
        info.index = i;
        info.name = to_utf8(d.Description);
        info.vendor_id = d.VendorId;
        info.device_id = d.DeviceId;
        info.subsystem_id = d.SubSysId;
        info.revision = d.Revision;
        info.luid = d.AdapterLuid;
        info.dedicated_video_memory = d.DedicatedVideoMemory;
        info.dedicated_system_memory = d.DedicatedSystemMemory;
        info.shared_system_memory = d.SharedSystemMemory;
        info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (d.VendorId == 0x1414 && d.DeviceId == 0x8C);
        if (!info.software) {
            info.integrated = looks_integrated(d.VendorId, info.name, d.DedicatedVideoMemory);
            info.discrete = !info.integrated;
        }
        LARGE_INTEGER umd{};
        if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            info.driver_version_parts[0] = HIWORD(umd.HighPart);
            info.driver_version_parts[1] = LOWORD(umd.HighPart);
            info.driver_version_parts[2] = HIWORD(umd.LowPart);
            info.driver_version_parts[3] = LOWORD(umd.LowPart);
            info.driver_version = version_string(info.driver_version_parts);
            info.driver_version_valid = true;
        }
        if (with_memory_budget) {
            ComPtr<IDXGIAdapter3> a3;
            if (SUCCEEDED(adapter.As(&a3))) {
                DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
                if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
                    info.memory.valid = true;
                    info.memory.budget = local.Budget;
                    info.memory.current_usage = local.CurrentUsage;
                    info.memory.available_for_reservation = local.AvailableForReservation;
                    info.memory.current_reservation = local.CurrentReservation;
                    if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) { info.memory.nonlocal_budget = nonlocal.Budget; info.memory.nonlocal_usage = nonlocal.CurrentUsage; }
                }
            }
        }
        fill_outputs(adapter.Get(), info);
        if (with_probes && !info.software) probe_adapter(adapter.Get(), info);
        out.adapters.push_back(std::move(info));
    }

    // Preference indexes: the first non-software adapter in each ordering.
    for (size_t i = 0; i < out.adapters.size(); ++i) if (!out.adapters[i].software) { out.high_performance_index = int(i); break; }
    if (factory6) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_MINIMUM_POWER, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            for (size_t k = 0; k < out.adapters.size(); ++k) if (out.adapters[k].luid.LowPart == d.AdapterLuid.LowPart && out.adapters[k].luid.HighPart == d.AdapterLuid.HighPart) { out.power_saving_index = int(k); break; }
            break;
        }
    } else {
        out.power_saving_index = out.high_performance_index;
    }
    bool has_int = false, has_dis = false;
    for (const auto& a : out.adapters) { has_int |= a.integrated && !a.software; has_dis |= a.discrete && !a.software; }
    out.hybrid = has_int && has_dis;
    return Status::success();
}

GpuMemorySnapshot query_gpu_memory() {
    GpuMemorySnapshot r;
    static LUID chosen{};
    static bool have_luid = false;
    static bool integrated = false;
    static uint64_t dedicated = 0, shared = 0;
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return r;
    ComPtr<IDXGIAdapter1> adapter;
    if (have_luid) {
        if (FAILED(factory->EnumAdapterByLuid(chosen, IID_PPV_ARGS(&adapter)))) have_luid = false;   // adapter vanished (driver reinstall): choose again
    }
    if (!have_luid) {
        ComPtr<IDXGIFactory6> f6;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            HRESULT hr = SUCCEEDED(factory.As(&f6)) ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) : factory->EnumAdapters1(i, &a);
            if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr)) break;
            DXGI_ADAPTER_DESC1 d{};
            if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            adapter = a;
            chosen = d.AdapterLuid;
            integrated = looks_integrated(d.VendorId, to_utf8(d.Description), d.DedicatedVideoMemory);
            dedicated = d.DedicatedVideoMemory;
            shared = d.SharedSystemMemory;
            have_luid = true;
            break;
        }
    }
    if (!adapter) return r;
    r.dedicated_total = integrated ? 0 : dedicated;
    r.shared_total = shared;
    r.unified = integrated;
    ComPtr<IDXGIAdapter3> a3;
    DXGI_QUERY_VIDEO_MEMORY_INFO local{};
    if (SUCCEEDED(adapter.As(&a3)) && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) { r.budget = local.Budget; r.usage = local.CurrentUsage; }
    r.valid = true;
    return r;
}

Status probe_d3d11(const std::vector<uint32_t>& formats, D3D11Probe& out) {
    out = D3D11Probe{};
    using D3D11Create = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    auto create = load_proc<D3D11Create>(L"d3d11.dll", "D3D11CreateDevice");
    if (!create) return Status::fail("d3d11.dll not available", ERROR_PROC_NOT_FOUND);
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return Status::hresult("CreateDXGIFactory1", hr);
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 desc{};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        hr = SUCCEEDED(factory.As(&f6)) ? f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) : factory->EnumAdapters1(i, &a);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter = a; desc = d; break; }
    }
    if (!adapter) return Status::fail("no hardware adapter", ERROR_NOT_FOUND);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                        D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1};
    ComPtr<ID3D11Device> dev;
    D3D_FEATURE_LEVEL got{};
    hr = create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &got, nullptr);
    if (FAILED(hr)) return Status::hresult("D3D11CreateDevice", hr);
    out.feature_level = uint32_t(got);
    out.dedicated_memory = desc.DedicatedVideoMemory;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof o2))) out.unified_memory = o2.UnifiedMemoryArchitecture != FALSE;
    else out.unified_memory = looks_integrated(desc.VendorId, to_utf8(desc.Description), desc.DedicatedVideoMemory);
    for (uint32_t f : formats) {
        UINT bits = 0;
        if (FAILED(dev->CheckFormatSupport(DXGI_FORMAT(f), &bits))) bits = 0;
        out.formats.push_back({f, bits});
        // sRGB variants (R8G8B8A8 29, B8G8R8A8 91, BC1 72, BC2 75, BC3 78, BC7 99) are asked for explicitly by the caller as extra entries
    }
    for (UINT samples : {2u, 4u, 8u, 16u}) {
        UINT quality = 0;
        if (SUCCEEDED(dev->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, samples, &quality)) && quality > 0) out.max_msaa_samples = samples;
    }
    out.ok = true;
    return Status::success();
}

// ---- Per-app GPU preference ---------------------------------------------------------------------------------------------------------

std::wstring current_exe_path() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
        if (n == 0) return {};
        if (n < p.size()) { p.resize(n); return p; }
        p.resize(p.size() * 2);
    }
}

Status get_app_gpu_preference(const std::wstring& exe, AppGpuPreference& out, const AppGpuPreferenceStore& store) {
    out = AppGpuPreference::Unspecified;
    std::wstring data;
    if (!reg_read_string(HKEY_CURRENT_USER, store.subkey, exe.c_str(), data)) return Status::success();   // no entry = Windows decides
    const size_t k = data.find(L"GpuPreference=");
    if (k == std::wstring::npos) return Status::success();
    const int v = _wtoi(data.c_str() + k + 14);
    out = v == 1 ? AppGpuPreference::PowerSaving : (v == 2 ? AppGpuPreference::HighPerformance : AppGpuPreference::Unspecified);
    return Status::success();
}

Status set_app_gpu_preference(const std::wstring& exe, AppGpuPreference value, const AppGpuPreferenceStore& store) {
    if (value == AppGpuPreference::Unspecified) {
        LSTATUS r = RegDeleteKeyValueW(HKEY_CURRENT_USER, store.subkey, exe.c_str());
        return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) ? Status::success() : Status::fail("RegDeleteKeyValue", uint32_t(r));
    }
    std::wstring data = L"GpuPreference=" + std::to_wstring(int(value)) + L";";
    LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, store.subkey, exe.c_str(), REG_SZ, data.c_str(), DWORD((data.size() + 1) * sizeof(wchar_t)));
    return r == ERROR_SUCCESS ? Status::success() : Status::fail("RegSetKeyValue(UserGpuPreferences)", uint32_t(r));
}

DiscreteGpuExports query_discrete_gpu_exports(HMODULE exe) {
    if (!exe) exe = GetModuleHandleW(nullptr);
    DiscreteGpuExports r;
    r.nvidia = GetProcAddress(exe, "NvOptimusEnablement") != nullptr;
    r.amd = GetProcAddress(exe, "AmdPowerXpressRequestHighPerformance") != nullptr;
    return r;
}

GraphicsSystemSettings query_graphics_system_settings() {
    GraphicsSystemSettings s;
    DWORD v = 0;
    if (reg_read_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", v)) s.hardware_scheduling = v == 2 ? Support::Yes : Support::No;
    if (reg_read_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"TdrDelay", v)) s.tdr_delay_seconds = v;
    if (reg_read_dword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"TdrLevel", v)) s.tdr_disabled = v == 0;
    std::wstring g;
    if (reg_read_string(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", L"DirectXUserGlobalSettings", g)) {
        auto flag = [&](const wchar_t* key) {
            size_t k = g.find(key);
            if (k == std::wstring::npos) return Support::Unknown;
            return g[k + wcslen(key)] == L'1' ? Support::Yes : Support::No;
        };
        s.game_mode_optimizations_windowed = flag(L"SwapEffectUpgradeEnable=");
        s.variable_refresh_rate_windowed = flag(L"VRROptimizeEnable=");
    }
    return s;
}

// ---- Device loss ----------------------------------------------------------------------------------------------------------------------

GpuLoss classify_hresult(HRESULT hr) {
    if (SUCCEEDED(hr)) return GpuLoss::None;   // includes S_PRESENT_OCCLUDED / S_PRESENT_MODE_CHANGED
    switch (uint32_t(hr)) {
        case 0x88760868: return GpuLoss::DeviceLost;                   // D3DERR_DEVICELOST
        case 0x88760869: return GpuLoss::DeviceNotReset;               // D3DERR_DEVICENOTRESET
        case 0x88760870: return GpuLoss::DeviceRemoved;                // D3DERR_DEVICEREMOVED
        case 0x88760874: return GpuLoss::DeviceHung;                   // D3DERR_DEVICEHUNG
        case 0x88760827: return GpuLoss::DriverInternalError;          // D3DERR_DRIVERINTERNALERROR
        case 0x8876017C: return GpuLoss::OutOfMemory;                  // D3DERR_OUTOFVIDEOMEMORY
        case 0x8876086C: return GpuLoss::InvalidCall;                  // D3DERR_INVALIDCALL
        case 0x887A0005: return GpuLoss::DeviceRemoved;                // DXGI_ERROR_DEVICE_REMOVED
        case 0x887A0006: return GpuLoss::DeviceHung;                   // DXGI_ERROR_DEVICE_HUNG
        case 0x887A0007: return GpuLoss::DeviceReset;                  // DXGI_ERROR_DEVICE_RESET
        case 0x887A0020: return GpuLoss::DriverInternalError;          // DXGI_ERROR_DRIVER_INTERNAL_ERROR
        case 0x887A0001: return GpuLoss::InvalidCall;                  // DXGI_ERROR_INVALID_CALL
        case 0x8007000E: return GpuLoss::OutOfMemory;                  // E_OUTOFMEMORY
        default: return GpuLoss::Unknown;
    }
}

const char* loss_name(GpuLoss l) {
    switch (l) {
        case GpuLoss::None: return "none";
        case GpuLoss::DeviceLost: return "device lost";
        case GpuLoss::DeviceNotReset: return "device not reset";
        case GpuLoss::DeviceHung: return "device hung";
        case GpuLoss::DeviceRemoved: return "device removed";
        case GpuLoss::DeviceReset: return "device reset";
        case GpuLoss::DriverInternalError: return "driver internal error";
        case GpuLoss::OutOfMemory: return "out of video memory";
        case GpuLoss::InvalidCall: return "invalid call";
        default: return "unknown";
    }
}

void GpuRecovery::enter_lost(GpuLoss l) {
    last_loss_ = l;
    if (state_ == State::Running) {
        state_ = State::Lost;
        ++total_losses_;
        delay_ms_ = policy_.initial_delay_ms;
        if (on_lost) on_lost(l);
    }
}

void GpuRecovery::recovered() {
    const bool was_down = state_ != State::Running;
    state_ = State::Running;
    reset_attempts_ = 0;
    recreate_attempts_ = 0;
    delay_ms_ = policy_.initial_delay_ms;
    if (was_down) { ++total_recoveries_; if (on_recovered) on_recovered(); }
}

GpuRecovery::Action GpuRecovery::on_result(HRESULT hr) {
    if (state_ == State::Failed) return Action::GiveUp;
    const GpuLoss loss = classify_hresult(hr);
    if (loss == GpuLoss::None) {
        if (state_ == State::Lost) recovered();   // e.g. TestCooperativeLevel went DEVICELOST -> D3D_OK on its own
        return Action::Continue;
    }
    if (loss == GpuLoss::InvalidCall || loss == GpuLoss::Unknown || loss == GpuLoss::OutOfMemory) return Action::Continue;   // not a device loss

    auto escalate_recreate = [&]() {
        if (recreate_attempts_ >= policy_.max_recreate_attempts) {
            state_ = State::Failed;
            if (on_failed) on_failed();
            return Action::GiveUp;
        }
        ++recreate_attempts_;
        state_ = State::Recreating;
        return Action::RecreateDevice;
    };

    switch (loss) {
        case GpuLoss::DeviceLost:
            enter_lost(loss);
            return Action::WaitAndRetry;    // exclusive-mode game that lost focus: wait as long as it takes (no attempt counted)
        case GpuLoss::DeviceNotReset:
            enter_lost(loss);
            if (reset_attempts_ < policy_.max_reset_attempts) { ++reset_attempts_; state_ = State::Resetting; return Action::ResetDevice; }
            return escalate_recreate();
        default:   // removed / hung / reset / driver error: the device object cannot be revived
            enter_lost(loss);
            return escalate_recreate();
    }
}

void GpuRecovery::on_reset_result(bool success) {
    if (success) { recovered(); return; }
    state_ = State::Lost;
    delay_ms_ = std::min(policy_.max_delay_ms, delay_ms_ * 2);
}

void GpuRecovery::on_recreate_result(bool success) {
    if (success) { recovered(); return; }
    if (recreate_attempts_ >= policy_.max_recreate_attempts) { state_ = State::Failed; if (on_failed) on_failed(); return; }
    state_ = State::Lost;
    delay_ms_ = std::min(policy_.max_delay_ms, delay_ms_ * 2);
}

// ---- AdapterWatcher ------------------------------------------------------------------------------------------------------------------

struct AdapterWatcher::Impl {
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIFactory7> factory7;
    HANDLE event = nullptr;
    DWORD cookie = 0;
    std::vector<LUID> luids;
    std::atomic<bool> injected{false};

    void snapshot() {
        luids.clear();
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> a;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(a->GetDesc1(&d))) luids.push_back(d.AdapterLuid);
        }
    }
};

AdapterWatcher::AdapterWatcher() : impl_(new Impl) {}
AdapterWatcher::~AdapterWatcher() { stop(); }
bool AdapterWatcher::uses_event() const { return impl_->event != nullptr; }
std::vector<LUID> AdapterWatcher::adapters_at_start() const { return impl_->luids; }
void AdapterWatcher::inject_change_for_test() { impl_->injected = true; }

Status AdapterWatcher::start() {
    stop();
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&impl_->factory));
    if (FAILED(hr)) return Status::hresult("CreateDXGIFactory1", hr);
    impl_->snapshot();
    if (SUCCEEDED(impl_->factory.As(&impl_->factory7))) {
        impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (impl_->event) {
            hr = impl_->factory7->RegisterAdaptersChangedEvent(impl_->event, &impl_->cookie);
            if (FAILED(hr)) { CloseHandle(impl_->event); impl_->event = nullptr; }
        }
    }
    return Status::success();
}

void AdapterWatcher::stop() {
    if (impl_->factory7 && impl_->event) impl_->factory7->UnregisterAdaptersChangedEvent(impl_->cookie);
    if (impl_->event) { CloseHandle(impl_->event); impl_->event = nullptr; }
    impl_->factory7.Reset();
    impl_->factory.Reset();
}

bool AdapterWatcher::changed() {
    bool c = impl_->injected.exchange(false);
    if (impl_->event) c |= WaitForSingleObject(impl_->event, 0) == WAIT_OBJECT_0;
    if (impl_->factory && !impl_->factory->IsCurrent()) {
        c = true;
        // Windows only reports a stale factory once: build a fresh one so the next change is seen again.
        impl_->factory7.Reset();
        impl_->factory.Reset();
        ComPtr<IDXGIFactory1> f;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
            impl_->factory = f;
            impl_->snapshot();
            if (impl_->event) { CloseHandle(impl_->event); impl_->event = nullptr; }
            if (SUCCEEDED(impl_->factory.As(&impl_->factory7))) {
                impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (impl_->event && FAILED(impl_->factory7->RegisterAdaptersChangedEvent(impl_->event, &impl_->cookie))) { CloseHandle(impl_->event); impl_->event = nullptr; }
            }
        }
    }
    return c;
}

}  // namespace nue::win
