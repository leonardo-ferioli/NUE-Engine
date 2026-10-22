# API changes

Rules: while the API is < 1.0 additive changes are announced here and by message to the platform sessions; breaking changes are
avoided and, when unavoidable, listed under "Breaking" with the migration. Platform sessions request changes in
`docs/REQUESTS_<platform>.md` (or by message); the core session decides and records the answer here.

## 0.1.0 (draft, first commit)

Initial public headers: base, core, app, window, input, audio, mixer, storage, power, system, gfx, diag, entry, null_backend.
Internal contracts for implementers: `src/core/internal/backend.h` (devices) and `src/core/internal/platform_ops.h` (per-OS layer).

## 0.1.2 (additive)

* `mixer.h`: `nue_voice_ring_set_notifications` + `nue_ring_notify_fn` (DirectSound-style notification positions on ring voices, fired outside the mixer lock); listener space is documented as left-handed (+x right, +y up, +z forward); the [audio] functions take a short mutex, they are not lock-free.
* New headers `include/nuepp/{core,window,audio,storage}.hpp` (C++ wrapper over the C API).
* Fixes visible to platform layers: the VFS directory cache no longer depends on `nue_time_ns()`; tests use `GENERIC_EQ` so they stay valid on real platform layers.

## 0.1.1 (additive; consumers compiled against 0.1.0 keep working)

Public C API
* `memory.h` (new): `nue_memory_get_info`, budgets with policies, eviction hooks, pressure helpers; events `NUE_EVENT_MEMORY_PRESSURE`, `NUE_EVENT_SYSTEM_SUSPENDING`, `NUE_EVENT_SYSTEM_RESUMED`.
* `app.h`: events `NUE_EVENT_AUDIO_INTERRUPTED`, `NUE_EVENT_AUDIO_RESUMED`, `NUE_EVENT_AUDIO_ROUTE_CHANGED`; `nue_event_memory` payload.
* `system.h`: `nue_system_info.platform_flags/platform_variant/runtime_name` (appended), `nue_platform_flags`, `nue_system_get_platform_flags`.
* `power.h`: `nue_power_info.tdp_watts_current/min/max`, `suspend_supported`; `nue_power_request.tdp_watts`; `NUE_POWER_APPLIED_TDP` (all appended).
* `input.h`: `NUE_GLYPHS_STEAM`, `nue_input_set_glyph_override`, `nue_text_input_show_screen_keyboard`.
* `window.h`: `nue_display_info.min/max_refresh_hz/variable_refresh` (appended); `nue_window_request_refresh_rate`, `nue_window_set_frame_limit`, `nue_window_get_frame_limit`.
* `core.h`: `nue_frame_limiter_wait/reset`, `nue_timer_*`.
* `gfx.h`: `nue_gpu_caps`, `nue_format_family`, `nue_format_support`, `nue_gpu_probe`, `nue_gfx_get_gpu_caps`, `nue_format_family_name`.
* `null_backend.h`: `nue_null_set_memory`, `nue_null_signal_memory_pressure`, `nue_null_system_suspend`, `nue_null_audio_interruption`.
* `nue.h` includes `nue/texture.h` when it is installed.

Internal contracts (platform sessions)
* `platform_ops.h`: appended `pre_init`, `owns_lifecycle`, `memory_info_ex`, `gpu_probe`, `window_request_refresh`, `screen_keyboard` (guarded by `struct_size`).
* `backend.h`: `nue_backend_init.platform_owns_lifecycle`.
* Init order changed: platform `pre_init` -> storage roots -> settings load -> backend -> platform `init` (so `config_get_*` works inside platform init).
* CMake: libraries linked PUBLIC to `nue_platform_obj` / `nue_texture_obj` are forwarded to `nue`; `nue_texture_obj` and `src/legacy` hooks; OBJC/OBJCXX enabled on Apple.

## 0.1.0 follow-ups (core block 1, additive, internal interfaces)

* `src/core/internal/backend.h`: `nue_window_snapshot` gained `nue_bool relative_mouse` (set it when relative mouse mode is active).
* Backends should call `nue_internal_window_changed()` (or push through the null-backend pattern: update the snapshot before announcing the event) so that
  `nue_window_get_*` is never older than the events that announce a change.
* `include/nue/app.h`: the engine loop also runs inside the OS entry point on mobile (SDL_main); no special mobile call path. `nue_app_run` keeps the loop alive in the background.
* Public C ABI: no change.
