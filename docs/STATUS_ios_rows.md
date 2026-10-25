# STATUS.md rows for iOS (text for the core session to merge; this layer may not edit STATUS.md)

| Capability | Provided by | Implemented | Tested how | Notes |
|---|---|---|---|---|
| `NUE_CAP_SAFE_AREA` | `window_safe_area` | written, not compiled | mapping unit-tested on the host | UIWindow.safeAreaInsets |
| `NUE_CAP_HAPTICS` | `haptics_*` | written, not compiled | - | CoreHaptics, iPhone only |
| `NUE_CAP_DATA_IMPORT`, `NUE_CAP_DATA_IMPORT_PICKER` | `import` | written, not compiled | path / validation logic unit-tested | UIDocumentPicker + security-scoped URL; the core copies |
| `NUE_CAP_THERMAL_STATE`, `NUE_CAP_BATTERY`, `NUE_CAP_LOW_POWER_MODE` | `power_info`, `pump` | written, not compiled | mapping + change detection unit-tested | polling + notifications |
| `NUE_CAP_KEEP_AWAKE` | `power_apply` | written, not compiled | request planning unit-tested | idleTimerDisabled |
| `NUE_CAP_THREAD_PRIORITY`, `NUE_CAP_CORE_CLASS` | QoS | written, not compiled | mapping unit-tested | QoS applies to the calling thread only |
| `NUE_CAP_CRASH_HANDLER` | `crash_install` | written, not compiled | - | signals only; no minidump |
| `NUE_CAP_GAME_MODE` | - | no | - | declare-only on iOS (plist), no public query |
| `NUE_CAP_MINIDUMP`, `NUE_CAP_PERF_HINT`, `NUE_CAP_THREAD_AFFINITY`, `NUE_CAP_TIMER_RESOLUTION`, `NUE_CAP_GPU_PREFERENCE` | - | no | - | not offered by iOS |

Platform row: iOS arm64 | `src/platform/ios` | layer written, never compiled (no macOS/Xcode available); host logic tests pass.
