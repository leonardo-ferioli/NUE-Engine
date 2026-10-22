// System queries used by the NUE platform adapter: device/OS info, locale, memory, safe area, device haptics.
// Never compiled: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mutex>
#include <os/proc.h>
#include <sys/sysctl.h>

#include "nue_ios.h"

namespace {
uint32_t sysctl_u32(const char* name) {
    uint32_t v = 0;
    size_t size = sizeof v;
    if (sysctlbyname(name, &v, &size, nullptr, 0) != 0) return 0;
    return v;
}


std::mutex g_haptic_mutex;
CHHapticEngine* g_haptic_engine = nil;

CHHapticEngine* haptic_engine() {   // caller holds g_haptic_mutex
    if (g_haptic_engine) return g_haptic_engine;
    if (![CHHapticEngine capabilitiesForHardware].supportsHaptics) return nil;
    NSError* err = nil;
    CHHapticEngine* e = [[CHHapticEngine alloc] initAndReturnError:&err];
    if (!e) return nil;
    e.autoShutdownEnabled = YES;   // the engine stops itself when idle; startAndReturnError is called before every use
    __weak CHHapticEngine* weak = e;
    e.resetHandler = ^{ NSError* e2 = nil; [weak startAndReturnError:&e2]; };
    g_haptic_engine = e;
    return e;
}
}  // namespace

extern "C" {

int nue_ios_system_info(NueIosSystemInfo* out) {
    if (!out) return -1;
    std::memset(out, 0, sizeof *out);
    NSOperatingSystemVersion v = [NSProcessInfo processInfo].operatingSystemVersion;
    std::snprintf(out->os_version, sizeof out->os_version, "%ld.%ld%s", (long)v.majorVersion, (long)v.minorVersion,
                  v.patchVersion ? [[NSString stringWithFormat:@".%ld", (long)v.patchVersion] UTF8String] : "");
    char machine[96] = {0};
    size_t size = sizeof machine;
    sysctlbyname("hw.machine", machine, &size, nullptr, 0);
#if TARGET_OS_SIMULATOR
    out->is_simulator = 1;
    const char* sim = getenv("SIMULATOR_MODEL_IDENTIFIER");   // the real model being simulated
    std::snprintf(out->device_model, sizeof out->device_model, "%s", sim ? sim : machine);
#else
    std::snprintf(out->device_model, sizeof out->device_model, "%s", machine);
#endif
    std::snprintf(out->cpu_name, sizeof out->cpu_name, "Apple arm64 (%s)", out->device_model);   // no public chip-name API
    out->idiom = static_cast<int>([UIDevice currentDevice].userInterfaceIdiom);
    out->logical_cores = sysctl_u32("hw.logicalcpu");
    out->physical_cores = sysctl_u32("hw.physicalcpu");
    out->performance_cores = sysctl_u32("hw.perflevel0.physicalcpu");   // 0 when the key does not exist
    out->efficiency_cores = sysctl_u32("hw.perflevel1.physicalcpu");
    out->physical_memory = [NSProcessInfo processInfo].physicalMemory;
    out->dark_mode = [UITraitCollection currentTraitCollection].userInterfaceStyle == UIUserInterfaceStyleDark ? 1 : 0;
    return 0;
}

int nue_ios_locale(char* buf, size_t size) {
    NSString* lang = [NSLocale preferredLanguages].firstObject ?: @"en-US";
    if (!buf || size == 0) return -1;
    std::snprintf(buf, size, "%s", [lang UTF8String]);
    return static_cast<int>(std::strlen(buf));
}

int64_t nue_ios_memory_available(void) {
    uint64_t avail = os_proc_available_memory();   // 0 on the Simulator
    if (avail) return static_cast<int64_t>(avail);
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) return -1;
    vm_size_t page = 0;
    host_page_size(mach_host_self(), &page);
    return static_cast<int64_t>((static_cast<uint64_t>(vm.free_count) + vm.inactive_count + vm.speculative_count) * page);
}

int64_t nue_ios_memory_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return -1;
    return static_cast<int64_t>(info.phys_footprint);
}

int nue_ios_safe_area(void* ui_window, float out_ltrb[4], float* width, float* height) {
    UIWindow* w = (__bridge UIWindow*)ui_window;
    if (![w isKindOfClass:[UIWindow class]] || !out_ltrb) return -1;
    UIEdgeInsets i = w.safeAreaInsets;
    out_ltrb[0] = static_cast<float>(i.left); out_ltrb[1] = static_cast<float>(i.top);
    out_ltrb[2] = static_cast<float>(i.right); out_ltrb[3] = static_cast<float>(i.bottom);
    if (width) *width = static_cast<float>(w.bounds.size.width);
    if (height) *height = static_cast<float>(w.bounds.size.height);
    return 0;
}

int nue_ios_device_haptics_available(void) {
    return [CHHapticEngine capabilitiesForHardware].supportsHaptics ? 1 : 0;
}

int nue_ios_device_haptic(uint32_t duration_ms, float intensity) {
    std::lock_guard<std::mutex> lk(g_haptic_mutex);
    CHHapticEngine* e = haptic_engine();
    if (!e) return -1;
    NSError* err = nil;
    if (![e startAndReturnError:&err]) return -1;
    if (intensity < 0) intensity = 0;
    if (intensity > 1) intensity = 1;
    CHHapticEventParameter* amount = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
    CHHapticEventParameter* sharp = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
    // Continuous events are limited to 30 s by CoreHaptics; short pulses use a transient event.
    CHHapticEvent* ev = duration_ms <= 30
        ? [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticTransient parameters:@[amount, sharp] relativeTime:0]
        : [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[amount, sharp] relativeTime:0
                                          duration:MIN(duration_ms, 30000u) / 1000.0];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ev] parameters:@[] error:&err];
    if (!pattern) return -1;
    id<CHHapticPatternPlayer> player = [e createPlayerWithPattern:pattern error:&err];
    return (player && [player startAtTime:0 error:&err]) ? 0 : -1;
}

}  // extern "C"
