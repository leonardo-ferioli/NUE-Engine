// Pure mapping between the iOS backend's vocabulary and the public NUE types (nue/*.h). No Apple headers: host-tested.
// The adapter (adapter/platform_ios.mm) is a thin shell over these functions.
#pragma once
#include <cstdint>
#include <string>

#include "nue/nue.h"
#include "caps.hpp"
#include "lifecycle.hpp"
#include "power.hpp"


namespace nue::ios {

nue_thermal_state map_thermal(ThermalLevel);
// UIDeviceBatteryState raw: unknown 0, unplugged 1, charging 2, full 3.
nue_power_state map_battery_state(int ui_device_battery_state);
bool on_ac_power(nue_power_state);
// UIDevice.batteryLevel is -1 when unknown, otherwise 0..1.
int32_t battery_percent(float level);

// Fills a NUE_EVENT_*_CHANGED payload.
nue_event_power make_power_payload(nue_thermal_state, nue_power_state, int32_t percent, bool low_power, nue_game_mode_state game_mode);
// True when a change between two states must emit which event (bit 0 power, bit 1 thermal, bit 2 low power). Battery percent changes of
// less than 1 point are not events.
struct PowerSnapshot { nue_thermal_state thermal; nue_power_state state; int32_t percent; bool low_power; };
unsigned power_changes(const PowerSnapshot& before, const PowerSnapshot& after);
enum : unsigned { change_power = 1, change_thermal = 2, change_low_power = 4 };

// ---- QoS (the only priority control iOS gives a third-party app) ----
enum class Qos : uint8_t { user_interactive, user_initiated, normal, utility, background };
Qos qos_for_priority(nue_thread_priority);
nue_thread_priority priority_for_qos(Qos);
Qos qos_for_core_class(nue_cpu_preference);   // efficiency -> background, performance -> user_initiated, default -> normal
// POSIX priority (0..63 range of SCHED_OTHER on Darwin) used for threads other than the caller, where QoS cannot be set.
int posix_priority_for(nue_thread_priority);

// ---- power requests ----
struct PowerPlan {
    bool keep_awake = false;          // UIApplication.idleTimerDisabled
    float fps = 0;                    // > 0: frame-rate range request derived from target_frame_time_us
    nue_power_applied applied = 0;
    bool partial = false;             // something was requested that iOS cannot honour
    bool release_all = false;         // the request is all zero
};
PowerPlan plan_power_request(const nue_power_request&);

// ---- windows ----
// Safe area as a rectangle inside the window, in logical units (SDL_GetWindowSafeArea convention), from insets in points.
nue_rect_i safe_area_rect(float width, float height, float left, float top, float right, float bottom);

// ---- storage ----
// Relative directory below the base directory of the kind ("NUE/nfsu2", "NUE/nfsu2/settings"); empty app/org fall back to defaults.
// kind: nue_root_kind. Returns an empty string for NUE_ROOT_CUSTOM or unknown kinds.
std::string storage_subpath(int kind, const char* org, const char* app_id);
// Sanitises a folder component: letters, digits, '-', '_', '.' kept; anything else becomes '_'; empty -> fallback; never "." or "..".
std::string sanitize_component(const char* s, const char* fallback);

// ---- lifecycle / memory (API 0.1.1) ----
// The iOS layer owns the lifecycle (platform_ops.owns_lifecycle): the state machine's state becomes the NUE app state.
nue_app_state map_app_state(LifecycleState);
nue_memory_pressure map_pressure(MemoryPressure);
// Fills the iOS part of nue_memory_info (the core already filled physical_*). The jetsam limit is estimated as footprint + room left
// (os_proc_available_memory reports the room LEFT); with no OS figure process_limit stays 0 ("none known").
void fill_memory_info(nue_memory_info* inout, uint64_t footprint, uint64_t os_available, uint64_t recommended_working_set, MemoryPressure pressure);

// ---- GPU capabilities without a device (API 0.1.1) ----
nue_format_support format_support(nue_format_family family, const MetalProbe& probe, const DeviceCaps& caps);
// `probed` is TRUE: the numbers come from MTLDevice queries (see nue_ios_probe_gpu).
void fill_gpu_caps(nue_gpu_caps* out, const MetalProbe& probe, const DeviceCaps& caps);
// Payload of NUE_EVENT_MEMORY_PRESSURE.
nue_event_memory make_memory_payload(MemoryPressure level, uint64_t available_bytes);

// ---- crash ----
const char* signal_reason(int sig);   // static strings, async-signal-safe

// ---- log ----
// os_log_type_t raw values: default 0, info 1, debug 2, error 0x10, fault 0x11.
uint8_t os_log_type_for(nue_log_level);

}  // namespace nue::ios
