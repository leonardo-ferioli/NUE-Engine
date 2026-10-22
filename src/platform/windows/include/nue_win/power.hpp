// Power and scheduling: EcoQoS / power-throttling opt-out, priority class, MMCSS, timer resolution and precise sleeping,
// keeping the display awake, battery / power-mode notifications, CPU frequency (throttling) hints, CPU topology (hybrid P/E cores).
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "nue_win/common.hpp"

namespace nue::win {

// ---- Power throttling (EcoQoS) -------------------------------------------------------------------------------------------
// Windows 10 1709+ lets the scheduler put a process on efficiency cores at low clocks ("EcoQoS"); Windows 11 additionally stops
// honouring timeBeginPeriod for a process whose windows are hidden / occluded. A game opts out explicitly.
struct ThrottlingState {
    Support execution_speed = Support::Unknown;   // Yes: throttling is forced ON, No: forced OFF (our opt-out) or automatic
    bool execution_speed_forced = false;          // the control bit is set: Windows obeys us instead of deciding itself
    bool ignore_timer_resolution_forced = false;  // Windows 11: the timer resolution request is honoured in the background
};
// Opts the process out of EcoQoS (and, on Windows 11, of the timer-resolution throttling). Cheap and idempotent.
Status process_disable_power_throttling(bool also_timer_resolution = true);
// Returns the process to automatic management.
Status process_reset_power_throttling();
Status process_query_power_throttling(ThrottlingState& out);
// Same for one thread (the control bit survives until the thread ends). `thread` null = current thread.
Status thread_disable_power_throttling(HANDLE thread = nullptr);
Status thread_reset_power_throttling(HANDLE thread = nullptr);

// ---- Priorities ------------------------------------------------------------------------------------------------------------
// REALTIME is intentionally not offered. A hard policy of the project (see PLATFORM.md): translated threads are capped at ABOVE_NORMAL,
// the original's TIME_CRITICAL request led to an unresponsive desktop.
enum class PriorityClass : uint8_t { Idle, BelowNormal, Normal, AboveNormal, High };
Status process_set_priority(PriorityClass c);
PriorityClass process_get_priority();
// Clamps a raw Win32 thread priority (THREAD_PRIORITY_*) to `max_allowed`, as the platform policy requires.
int clamp_thread_priority(int win32_priority, int max_allowed = THREAD_PRIORITY_ABOVE_NORMAL);
Status thread_set_priority(HANDLE thread, int win32_priority, int max_allowed = THREAD_PRIORITY_ABOVE_NORMAL);

// ---- MMCSS ---------------------------------------------------------------------------------------------------------------
// Multimedia Class Scheduler Service: registers the calling thread under a task such as "Games" or "Pro Audio" so that the
// scheduler gives it periodic, low-latency CPU time. Scoped: ends the registration in the destructor.
class MmcssScope {
public:
    MmcssScope() = default;
    ~MmcssScope() { end(); }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;
    // `task` examples: L"Games", L"Pro Audio", L"Audio", L"Playback". Must be called on the thread to register.
    Status begin(const wchar_t* task = L"Games");
    void end();
    bool active() const { return handle_ != nullptr; }
    DWORD task_index() const { return task_index_; }
    // AVRT_PRIORITY_LOW(-1) .. CRITICAL(2). Only meaningful while active.
    Status set_priority(int avrt_priority);
private:
    HANDLE handle_ = nullptr;
    DWORD task_index_ = 0;
};

// ---- Timer resolution and precise waits -------------------------------------------------------------------------------------
struct TimerResolutionInfo {
    double minimum_ms = 0, maximum_ms = 0, current_ms = 0;   // coarsest, finest, in effect for the system (ntdll)
    bool valid = false;
};
bool query_timer_resolution(TimerResolutionInfo& out);

// timeBeginPeriod / timeEndPeriod with balanced accounting. Since Windows 10 2004 the request is per process; since Windows 11 it is
// ignored while every window of the process is hidden/occluded unless process_disable_power_throttling() was called.
class TimerResolutionScope {
public:
    TimerResolutionScope() = default;
    ~TimerResolutionScope() { end(); }
    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;
    Status begin(UINT milliseconds = 1);
    void end();
    bool active() const { return period_ != 0; }
private:
    UINT period_ = 0;
};

// Sleep with sub-millisecond accuracy: a high-resolution waitable timer (Windows 10 1803+) for the bulk, a short spin for the tail.
class PreciseSleeper {
public:
    PreciseSleeper();
    ~PreciseSleeper();
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;
    bool high_resolution() const { return high_res_; }
    // Returns when `nanoseconds` have elapsed (never earlier). Waits in the kernel except for the last `spin_ns`.
    void sleep_ns(int64_t nanoseconds, int64_t spin_ns = 200000);
    void sleep_until_qpc(int64_t target_qpc, int64_t spin_ns = 200000);
private:
    HANDLE timer_ = nullptr;
    bool high_res_ = false;
};
int64_t qpc_now();
int64_t qpc_frequency();
inline int64_t qpc_from_ns(int64_t ns) { return int64_t(double(ns) * 1e-9 * double(qpc_frequency())); }
inline double qpc_to_ms(int64_t ticks) { return double(ticks) * 1000.0 / double(qpc_frequency()); }

// ---- Keep the display / system awake -----------------------------------------------------------------------------------------
// PowerCreateRequest/PowerSetRequest (visible in `powercfg /requests`), with SetThreadExecutionState as fallback.
class WakeLock {
public:
    WakeLock() = default;
    ~WakeLock() { release(); }
    WakeLock(const WakeLock&) = delete;
    WakeLock& operator=(const WakeLock&) = delete;
    Status acquire(const wchar_t* reason = L"NUE game running", bool display = true, bool system = true);
    void release();
    bool active() const { return request_ != nullptr || legacy_; }
    bool using_power_request() const { return request_ != nullptr; }
private:
    HANDLE request_ = nullptr;
    bool legacy_ = false, display_ = false, system_ = false;
};

// ---- Battery, energy saver, power mode ------------------------------------------------------------------------------------------
struct PowerStatus {
    bool valid = false;
    Support on_ac = Support::Unknown;
    bool has_battery = false;
    int battery_percent = -1;          // 0..100, -1 unknown
    int battery_seconds_left = -1;     // -1 unknown
    bool energy_saver = false;         // Battery Saver / Energy Saver is on (SYSTEM_STATUS_FLAG_POWER_SAVING_ON)
};
PowerStatus query_power_status();

// Mirrors EFFECTIVE_POWER_MODE (Windows 10 1809+). GameMode is reported while Windows has put the system in Game Mode.
enum class EffectivePowerMode : int { Unknown = -1, BatterySaver = 0, BetterBattery = 1, Balanced = 2, HighPerformance = 3, MaxPerformance = 4, GameMode = 5, MixedReality = 6 };
const char* power_mode_name(EffectivePowerMode m);
// Current mode via powrprof (registers and unregisters a one-shot notification); Unknown when the API is missing.
EffectivePowerMode query_effective_power_mode();

// Hints about CPU throttling without admin rights: PROCESSOR_POWER_INFORMATION (CallNtPowerInformation).
struct CpuFrequencyInfo {
    bool valid = false;
    uint32_t logical_processors = 0;
    uint32_t max_mhz = 0, current_mhz = 0, limit_mhz = 0;   // of logical processor 0..n-1: the highest values
    bool limited() const { return valid && limit_mhz && max_mhz && limit_mhz < max_mhz; }   // thermal / policy limit active
};
CpuFrequencyInfo query_cpu_frequency();

// Notifications that need no window: suspend/resume, AC/DC, battery, energy saver, display on/off, effective power mode.
struct PowerEvent {
    enum Kind { Suspend, ResumeAutomatic, ResumeSuspend, PowerSource, BatteryPercent, EnergySaver, DisplayState, PowerMode } kind;
    int value = 0;     // PowerSource: 0 AC / 1 DC / 2 short-term; BatteryPercent 0..100; EnergySaver 0/1; DisplayState 0 off / 1 on / 2 dimmed; PowerMode: EffectivePowerMode
};
using PowerCallback = std::function<void(const PowerEvent&)>;
class PowerNotifier {
public:
    PowerNotifier();
    ~PowerNotifier();
    PowerNotifier(const PowerNotifier&) = delete;
    PowerNotifier& operator=(const PowerNotifier&) = delete;
    // Callbacks run on a system thread: keep them short and thread-safe.
    Status start(PowerCallback callback);
    void stop();
    bool active() const;
    // Which sources registered (diagnostics).
    uint32_t registered_mask() const;
    // Testing seam: feeds an event through the same dispatch path.
    void inject_for_test(const PowerEvent& e);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace nue::win
