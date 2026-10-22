#include <initguid.h>

#include "nue_win/power.hpp"

#include <avrt.h>
#include <powrprof.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "powrprof.lib")

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

extern "C" {
// timeapi.h without dragging mmsystem.h
__declspec(dllimport) UINT WINAPI timeBeginPeriod(UINT);
__declspec(dllimport) UINT WINAPI timeEndPeriod(UINT);
}

namespace nue::win {

// ---- Power throttling -----------------------------------------------------------------------------------------------------

Status process_disable_power_throttling(bool also_timer_resolution) {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = 0;   // control bit set, state bit clear = throttling forced OFF
    if (also_timer_resolution && is_windows11()) s.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;   // state clear = the request is honoured always
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) {
        if (s.ControlMask & PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) {   // an older Windows 11 build without the third bit
            s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) return Status::success();
        }
        return Status::last_error("SetProcessInformation(ProcessPowerThrottling)");
    }
    return Status::success();
}

Status process_force_power_throttling() {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;   // control + state set = throttling forced ON
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) return Status::last_error("SetProcessInformation(ProcessPowerThrottling on)");
    return Status::success();
}

Status process_reset_power_throttling() {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = 0;   // no control bits: the system decides again
    s.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) return Status::last_error("SetProcessInformation(ProcessPowerThrottling reset)");
    return Status::success();
}

Status process_query_power_throttling(ThrottlingState& out) {
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (!GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s)) return Status::last_error("GetProcessInformation(ProcessPowerThrottling)");
    out.execution_speed_forced = (s.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
    out.execution_speed = !out.execution_speed_forced ? Support::Unknown : ((s.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) ? Support::Yes : Support::No);
    out.ignore_timer_resolution_forced = (s.ControlMask & PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION) != 0 && !(s.StateMask & PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
    return Status::success();
}

namespace {
Status thread_throttling(HANDLE thread, bool disable) {
    THREAD_POWER_THROTTLING_STATE s{};
    s.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = disable ? THREAD_POWER_THROTTLING_EXECUTION_SPEED : 0;
    s.StateMask = 0;
    if (!SetThreadInformation(thread ? thread : GetCurrentThread(), ThreadPowerThrottling, &s, sizeof s)) return Status::last_error("SetThreadInformation(ThreadPowerThrottling)");
    return Status::success();
}
}  // namespace
Status thread_disable_power_throttling(HANDLE thread) { return thread_throttling(thread, true); }
Status thread_reset_power_throttling(HANDLE thread) { return thread_throttling(thread, false); }

// ---- Priorities -------------------------------------------------------------------------------------------------------------

Status process_set_priority(PriorityClass c) {
    static const DWORD map[] = {IDLE_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS, HIGH_PRIORITY_CLASS};
    if (!SetPriorityClass(GetCurrentProcess(), map[unsigned(c)])) return Status::last_error("SetPriorityClass");
    return Status::success();
}

PriorityClass process_get_priority() {
    switch (GetPriorityClass(GetCurrentProcess())) {
        case IDLE_PRIORITY_CLASS: return PriorityClass::Idle;
        case BELOW_NORMAL_PRIORITY_CLASS: return PriorityClass::BelowNormal;
        case ABOVE_NORMAL_PRIORITY_CLASS: return PriorityClass::AboveNormal;
        case HIGH_PRIORITY_CLASS: case REALTIME_PRIORITY_CLASS: return PriorityClass::High;
        default: return PriorityClass::Normal;
    }
}

int clamp_thread_priority(int p, int max_allowed) {
    // THREAD_PRIORITY_TIME_CRITICAL (15) and IDLE (-15) are special values outside the -2..2 ladder.
    if (p == THREAD_PRIORITY_TIME_CRITICAL) return max_allowed;
    if (p == THREAD_PRIORITY_IDLE) return THREAD_PRIORITY_IDLE;
    return std::min(p, max_allowed);
}

Status thread_set_priority(HANDLE thread, int p, int max_allowed) {
    if (!SetThreadPriority(thread ? thread : GetCurrentThread(), clamp_thread_priority(p, max_allowed))) return Status::last_error("SetThreadPriority");
    return Status::success();
}

// ---- MMCSS ----------------------------------------------------------------------------------------------------------------------

Status MmcssScope::begin(const wchar_t* task) {
    if (handle_) return Status::success();
    DWORD index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &index);
    if (!h) return Status::last_error("AvSetMmThreadCharacteristics");
    handle_ = h;
    task_index_ = index;
    return Status::success();
}

void MmcssScope::end() {
    if (handle_) { AvRevertMmThreadCharacteristics(handle_); handle_ = nullptr; task_index_ = 0; }
}

Status MmcssScope::set_priority(int p) {
    if (!handle_) return Status::fail("MmcssScope not active", ERROR_INVALID_STATE);
    if (!AvSetMmThreadPriority(handle_, AVRT_PRIORITY(p))) return Status::last_error("AvSetMmThreadPriority");
    return Status::success();
}

// ---- Timers ---------------------------------------------------------------------------------------------------------------------

bool query_timer_resolution(TimerResolutionInfo& out) {
    using Fn = LONG(NTAPI*)(PULONG min_, PULONG max_, PULONG cur);
    auto fn = load_proc<Fn>(L"ntdll.dll", "NtQueryTimerResolution");
    if (!fn) return false;
    ULONG mn = 0, mx = 0, cur = 0;   // 100 ns units; "minimum" is the coarsest period
    if (fn(&mn, &mx, &cur) != 0) return false;
    out.minimum_ms = mn / 10000.0;
    out.maximum_ms = mx / 10000.0;
    out.current_ms = cur / 10000.0;
    out.valid = true;
    return true;
}

Status TimerResolutionScope::begin(UINT ms) {
    if (period_) return Status::success();
    if (timeBeginPeriod(ms) != 0) return Status::fail("timeBeginPeriod", ERROR_NOT_SUPPORTED);
    period_ = ms;
    return Status::success();
}

Status TimerResolutionScope::begin_us(uint32_t us, uint32_t* granted) {
    end();
    if (us >= 1000) {
        const UINT ms = (us + 999) / 1000;
        Status s = begin(ms);
        if (s.ok && granted) *granted = ms * 1000;
        return s;
    }
    using Fn = LONG(NTAPI*)(ULONG desired, BOOLEAN set, PULONG actual);
    auto fn = load_proc<Fn>(L"ntdll.dll", "NtSetTimerResolution");
    if (!fn) return begin(1);
    ULONG want = us * 10 ? us * 10 : 5000, actual = 0;
    if (fn(want, TRUE, &actual) != 0) return Status::fail("NtSetTimerResolution", ERROR_NOT_SUPPORTED);
    nt_period_ = want;
    if (granted) *granted = actual / 10;
    return Status::success();
}

void TimerResolutionScope::end() {
    if (period_) { timeEndPeriod(period_); period_ = 0; }
    if (nt_period_) {
        using Fn = LONG(NTAPI*)(ULONG desired, BOOLEAN set, PULONG actual);
        if (auto fn = load_proc<Fn>(L"ntdll.dll", "NtSetTimerResolution")) { ULONG actual = 0; fn(nt_period_, FALSE, &actual); }
        nt_period_ = 0;
    }
}

int64_t qpc_frequency() {
    static const int64_t f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return int64_t(v.QuadPart); }();
    return f;
}
int64_t qpc_now() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return int64_t(v.QuadPart); }

PreciseSleeper::PreciseSleeper() {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    high_res_ = timer_ != nullptr;
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
}

PreciseSleeper::~PreciseSleeper() { if (timer_) CloseHandle(timer_); }

void PreciseSleeper::sleep_until_qpc(int64_t target, int64_t spin_ns) {
    const int64_t spin = qpc_from_ns(spin_ns);
    int64_t now = qpc_now();
    int64_t remaining = target - now;
    if (remaining > spin && timer_) {
        // Relative due time in 100 ns units, negative. Leave `spin` ticks to the busy loop.
        const double wait_ticks = double(remaining - spin);
        LARGE_INTEGER due;
        due.QuadPart = -int64_t(wait_ticks / double(qpc_frequency()) * 1e7);
        if (due.QuadPart < 0 && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer_, INFINITE);
    } else if (remaining > spin) {
        Sleep(DWORD(qpc_to_ms(remaining - spin)));
    }
    while (qpc_now() < target) YieldProcessor();
}

void PreciseSleeper::sleep_ns(int64_t ns, int64_t spin_ns) {
    if (ns <= 0) return;
    sleep_until_qpc(qpc_now() + qpc_from_ns(ns), spin_ns);
}

// ---- Wake lock -----------------------------------------------------------------------------------------------------------------

Status WakeLock::acquire(const wchar_t* reason, bool display, bool system) {
    release();
    REASON_CONTEXT ctx{};
    ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
    ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    ctx.Reason.SimpleReasonString = const_cast<LPWSTR>(reason);
    HANDLE h = PowerCreateRequest(&ctx);
    if (h && h != INVALID_HANDLE_VALUE) {
        bool ok = true;
        if (display) ok = PowerSetRequest(h, PowerRequestDisplayRequired) != FALSE;
        if (ok && system) ok = PowerSetRequest(h, PowerRequestSystemRequired) != FALSE;
        if (ok) { request_ = h; display_ = display; system_ = system; return Status::success(); }
        CloseHandle(h);
    }
    EXECUTION_STATE flags = ES_CONTINUOUS | (display ? ES_DISPLAY_REQUIRED : 0) | (system ? ES_SYSTEM_REQUIRED : 0);
    if (!SetThreadExecutionState(flags)) return Status::last_error("SetThreadExecutionState");
    legacy_ = true;
    display_ = display;
    system_ = system;
    return Status::success();
}

void WakeLock::release() {
    if (request_) {
        if (display_) PowerClearRequest(request_, PowerRequestDisplayRequired);
        if (system_) PowerClearRequest(request_, PowerRequestSystemRequired);
        CloseHandle(request_);
        request_ = nullptr;
    }
    if (legacy_) { SetThreadExecutionState(ES_CONTINUOUS); legacy_ = false; }
}

// ---- Power status / mode ------------------------------------------------------------------------------------------------------

PowerStatus query_power_status() {
    PowerStatus r;
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return r;
    r.valid = true;
    r.on_ac = s.ACLineStatus == 255 ? Support::Unknown : (s.ACLineStatus == 1 ? Support::Yes : Support::No);
    r.has_battery = !(s.BatteryFlag & 128) && s.BatteryFlag != 255;
    r.battery_percent = s.BatteryLifePercent == 255 ? -1 : int(s.BatteryLifePercent);
    r.battery_seconds_left = s.BatteryLifeTime == DWORD(-1) ? -1 : int(s.BatteryLifeTime);
    r.charging = (s.BatteryFlag != 255) && (s.BatteryFlag & 8) != 0;
    r.energy_saver = (s.SystemStatusFlag & 1) != 0;
    return r;
}

const char* power_mode_name(EffectivePowerMode m) {
    switch (m) {
        case EffectivePowerMode::BatterySaver: return "battery saver";
        case EffectivePowerMode::BetterBattery: return "better battery";
        case EffectivePowerMode::Balanced: return "balanced";
        case EffectivePowerMode::HighPerformance: return "high performance";
        case EffectivePowerMode::MaxPerformance: return "max performance";
        case EffectivePowerMode::GameMode: return "game mode";
        case EffectivePowerMode::MixedReality: return "mixed reality";
        default: return "unknown";
    }
}

namespace {
using EffectiveCallback = void(CALLBACK*)(int mode, void* context);
using RegisterEffectiveFn = HRESULT(WINAPI*)(ULONG version, EffectiveCallback, void* context, void** handle);
using UnregisterEffectiveFn = HRESULT(WINAPI*)(void* handle);
constexpr ULONG kEffectiveVersion = 1;   // EFFECTIVE_POWER_MODE_V1 (modes 0..4, V2 adds GameMode/MixedReality as 5/6)
constexpr ULONG kEffectiveVersion2 = 2;

struct OneShotMode {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<int> mode{-1};
    ~OneShotMode() { CloseHandle(event); }
};
void CALLBACK one_shot_cb(int mode, void* ctx) {
    auto* s = static_cast<OneShotMode*>(ctx);
    s->mode = mode;
    SetEvent(s->event);
}
}  // namespace

EffectivePowerMode query_effective_power_mode() {
    auto reg = load_proc<RegisterEffectiveFn>(L"powrprof.dll", "PowerRegisterForEffectivePowerModeNotifications");
    auto unreg = load_proc<UnregisterEffectiveFn>(L"powrprof.dll", "PowerUnregisterFromEffectivePowerModeNotifications");
    if (!reg || !unreg) return EffectivePowerMode::Unknown;
    OneShotMode state;
    void* handle = nullptr;
    HRESULT hr = reg(kEffectiveVersion2, one_shot_cb, &state, &handle);
    if (FAILED(hr)) hr = reg(kEffectiveVersion, one_shot_cb, &state, &handle);
    if (FAILED(hr) || !handle) return EffectivePowerMode::Unknown;
    WaitForSingleObject(state.event, 1000);
    unreg(handle);
    return EffectivePowerMode(state.mode.load());
}

CpuFrequencyInfo query_cpu_frequency() {
    CpuFrequencyInfo r;
    const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!n) return r;
    struct ProcessorPowerInformation { ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState; };   // PROCESSOR_POWER_INFORMATION
    std::vector<ProcessorPowerInformation> info(n);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, info.data(), ULONG(n * sizeof(ProcessorPowerInformation))) != 0) return r;
    r.valid = true;
    r.logical_processors = n;
    for (const auto& p : info) {
        r.max_mhz = std::max<uint32_t>(r.max_mhz, p.MaxMhz);
        r.current_mhz = std::max<uint32_t>(r.current_mhz, p.CurrentMhz);
        r.limit_mhz = std::max<uint32_t>(r.limit_mhz, p.MhzLimit);
    }
    return r;
}

// ---- PowerNotifier -------------------------------------------------------------------------------------------------------------

struct PowerNotifier::Impl {
    std::mutex mutex;
    PowerCallback callback;
    HPOWERNOTIFY suspend_handle = nullptr;
    HPOWERNOTIFY settings[4] = {};
    void* mode_handle = nullptr;
    UnregisterEffectiveFn unregister_mode = nullptr;
    uint32_t mask = 0;
    bool active = false;

    void emit(const PowerEvent& e) {
        PowerCallback cb;
        { std::lock_guard<std::mutex> g(mutex); cb = callback; }
        if (cb) cb(e);
    }

    static ULONG CALLBACK suspend_cb(PVOID context, ULONG type, PVOID) {
        auto* self = static_cast<Impl*>(context);
        switch (type) {
            case PBT_APMSUSPEND: self->emit({PowerEvent::Suspend, 0}); break;
            case PBT_APMRESUMESUSPEND: self->emit({PowerEvent::ResumeSuspend, 0}); break;
            case PBT_APMRESUMEAUTOMATIC: self->emit({PowerEvent::ResumeAutomatic, 0}); break;
            default: break;
        }
        return ERROR_SUCCESS;
    }

    static ULONG CALLBACK setting_cb(PVOID context, ULONG type, PVOID setting) {
        auto* self = static_cast<Impl*>(context);
        if (type != PBT_POWERSETTINGCHANGE || !setting) return ERROR_SUCCESS;
        auto* s = static_cast<POWERBROADCAST_SETTING*>(setting);
        if (s->DataLength < sizeof(DWORD)) return ERROR_SUCCESS;
        const int value = int(*reinterpret_cast<DWORD*>(s->Data));
        if (IsEqualGUID(s->PowerSetting, GUID_ACDC_POWER_SOURCE)) self->emit({PowerEvent::PowerSource, value});
        else if (IsEqualGUID(s->PowerSetting, GUID_BATTERY_PERCENTAGE_REMAINING)) self->emit({PowerEvent::BatteryPercent, value});
        else if (IsEqualGUID(s->PowerSetting, GUID_POWER_SAVING_STATUS)) self->emit({PowerEvent::EnergySaver, value});
        else if (IsEqualGUID(s->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)) self->emit({PowerEvent::DisplayState, value});
        return ERROR_SUCCESS;
    }

    static void CALLBACK mode_cb(int mode, void* context) { static_cast<Impl*>(context)->emit({PowerEvent::PowerMode, mode}); }
};

PowerNotifier::PowerNotifier() : impl_(new Impl) {}
PowerNotifier::~PowerNotifier() { stop(); }
bool PowerNotifier::active() const { return impl_->active; }
uint32_t PowerNotifier::registered_mask() const { return impl_->mask; }

void PowerNotifier::inject_for_test(const PowerEvent& e) { impl_->emit(e); }

Status PowerNotifier::start(PowerCallback callback) {
    stop();
    { std::lock_guard<std::mutex> g(impl_->mutex); impl_->callback = std::move(callback); }

    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS suspend_params{Impl::suspend_cb, impl_.get()};
    HPOWERNOTIFY h = nullptr;
    if (PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &suspend_params, &h) == ERROR_SUCCESS) { impl_->suspend_handle = h; impl_->mask |= 1; }

    static const GUID* const guids[] = {&GUID_ACDC_POWER_SOURCE, &GUID_BATTERY_PERCENTAGE_REMAINING, &GUID_POWER_SAVING_STATUS, &GUID_CONSOLE_DISPLAY_STATE};
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS setting_params{Impl::setting_cb, impl_.get()};
    for (int i = 0; i < 4; ++i) {
        HPOWERNOTIFY sh = RegisterPowerSettingNotification(reinterpret_cast<HANDLE>(&setting_params), guids[i], DEVICE_NOTIFY_CALLBACK);
        if (sh) { impl_->settings[i] = sh; impl_->mask |= 2u << i; }
    }

    auto reg = load_proc<RegisterEffectiveFn>(L"powrprof.dll", "PowerRegisterForEffectivePowerModeNotifications");
    impl_->unregister_mode = load_proc<UnregisterEffectiveFn>(L"powrprof.dll", "PowerUnregisterFromEffectivePowerModeNotifications");
    if (reg && impl_->unregister_mode) {
        void* mh = nullptr;
        HRESULT hr = reg(kEffectiveVersion2, Impl::mode_cb, impl_.get(), &mh);
        if (FAILED(hr)) hr = reg(kEffectiveVersion, Impl::mode_cb, impl_.get(), &mh);
        if (SUCCEEDED(hr) && mh) { impl_->mode_handle = mh; impl_->mask |= 32; }
    }
    impl_->active = impl_->mask != 0;
    return impl_->active ? Status::success() : Status::fail("no power notification could be registered", ERROR_NOT_SUPPORTED);
}

void PowerNotifier::stop() {
    if (impl_->suspend_handle) { PowerUnregisterSuspendResumeNotification(impl_->suspend_handle); impl_->suspend_handle = nullptr; }
    for (auto& s : impl_->settings) if (s) { UnregisterPowerSettingNotification(s); s = nullptr; }
    if (impl_->mode_handle && impl_->unregister_mode) { impl_->unregister_mode(impl_->mode_handle); impl_->mode_handle = nullptr; }
    impl_->mask = 0;
    impl_->active = false;
    std::lock_guard<std::mutex> g(impl_->mutex);
    impl_->callback = nullptr;
}

}  // namespace nue::win
