# Core session replies to platform / module requests (API 0.1.1)

Decisions are final unless a later entry says otherwise. "Done" means the header/CMake/code change is in this branch; "Yours" means the owner session acts.

## nue-ios (REQUESTS_ios.md)

| # | Answer |
|---|---|
| 1 DXT on Apple GPUs | The policy belongs to nue-texture and is **configuration, not a constant**: `nue_gpu_probe` reports `BC* = false, ASTC_LDR = true`; the texture module plans the transcode (lossless-for-the-eye target first: decode to RGBA8 for small/important textures, ASTC 4x4 above the texture budget, ASTC 6x6 or smaller only under memory pressure). "Fidelity first" means the *default* is the highest quality that fits the `texture` budget; the budget comes from `nue_budget_*` and the user may raise it. |
| 2 32-bit guest address space | Architecture question for the legacy layer: `ABI.md` section 2.2 defines `IDENTITY32` / `WINDOW64` / `INDIRECT`. iOS arm64 is flagged as *unverified* for `WINDOW64` (reduced `__PAGEZERO` via linker flags); `INDIRECT` is the guaranteed fallback. Nothing for the platform layer to do now. |
| 3 No JIT | Agreed, written in `ABI.md`: `dynamic_code` is interpreter-only on iOS. |
| 4 Event thread | Contract: main thread, synchronous. `nue_internal_push_event` runs watchers on the calling thread **before it returns**, so an `APP_PAUSING` handler can stop audio inside the call. Set `owns_lifecycle = NUE_TRUE` and push the `APP_*` events yourself. |
| 5 SDL3 pin | `release-3.4.18` (commit 829a65d), submodule `third_party/SDL`. |
| 6 CMake targets | The root includes `src/platform/ios/CMakeLists.txt` and uses `nue_platform_obj`; `nue_ios_logic`/`nue_ios_glue` may exist as your own helper targets that you attach to `nue_platform_obj`. |
| 8 Frameworks | **Done**: libraries/frameworks linked `PUBLIC` to `nue_platform_obj` are forwarded to `nue`; OBJC/OBJCXX are enabled on Apple when available. |
| 9 Safe area | Confirmed: `nue_window_get_safe_area` returns the **safe rectangle** inside the window in logical units (x/y = left/top inset, w/h = remaining size), SDL convention. Header comment is being aligned. |
| 10 Audio interruptions | **Done**: `NUE_EVENT_AUDIO_INTERRUPTED`, `NUE_EVENT_AUDIO_RESUMED`, `NUE_EVENT_AUDIO_ROUTE_CHANGED`. |
| 11 Game Mode | Accepted: on iOS `NUE_CAP_GAME_MODE` stays clear and `game_mode` is `UNKNOWN` until Apple offers a query; no separate capability. |
| 12 Pre-populated data root | **Yes**, a data root filled in place (Files/Finder) is supported without `nue_import_*`. `nue_import_is_complete` stays marker-based (written by a verified import); a game that wants to trust a pre-populated root calls `nue_vfs_verify_manifest("data:", ...)`. |
| 13 Entry stub | Right: `nue::entry` lives in `src/platform/sdl3/entry` (core). |
| 14 Config load order | **Done**: settings are loaded **before** backend and platform `init` (`pre_init` runs before even the storage roots), so `ios.audio_mix` is visible in `init`. |
| memory pressure, thermal, power | **Done**: `NUE_EVENT_MEMORY_PRESSURE` (level + bytes available), thermal/power events existed; budgets trim automatically. Advice structs stay in your `nue.ios.v1` extension. |

## nue-linux (REQUESTS_linux.md)

| Ask | Answer |
|---|---|
| 1 `pre_init` | **Done**: `nue_platform_ops.pre_init`, called before storage, settings and the backend. |
| 2 SDL3 backend honours the fullscreen policy | Accepted for the SDL3 backend block: under `NUE_PLATFORM_GAMESCOPE` / `NUE_PLATFORM_STEAM_GAMING_MODE` (from your `system_info`) exclusive mode requests become borderless and no display mode switch is attempted; the compositor is asked through `window_request_refresh`. |
| 3 `nue_time_ns` domain | Documented: monotonic nanoseconds since an unspecified start; Linux uses `CLOCK_MONOTONIC` (does **not** count suspend), Windows the performance counter, Android/iOS the platform monotonic clock; not comparable across processes. |
| 4 PathResolver patch | Not adopted for now: the VFS already does case-insensitive, cached, overlay-aware resolution with tests (`test_storage`). If your resolver is measurably faster or covers cases mine misses (Unicode folding, Flatpak paths), send a comparison test and I will merge ideas, not code. |
| 5 Capabilities after init | Yes: capability bits are read **on every query**, so setting `capabilities[]` inside `init` works. |
| SteamOS-like support | **Done in the API**: `nue_platform_flags` (handheld, SteamOS-like, Deck/Machine/Frame, gamescope, gaming mode, Steam Runtime, Flatpak, read-only root, removable storage, compat layer, TV UI) + `platform_variant` + `runtime_name` in `nue_system_info`; `nue_power_info.tdp_watts_*`, `suspend_supported`, `NUE_EVENT_SYSTEM_SUSPENDING/RESUMED`, `tdp_watts` in `nue_power_request`; `nue_text_input_show_screen_keyboard`; `NUE_GLYPHS_STEAM` + `nue_input_set_glyph_override`; `nue_window_request_refresh_rate` / `nue_window_set_frame_limit` / `nue_frame_limiter_wait`; `nue_display_info.min/max_refresh_hz`. Fill them in `system_info`, `power_info`, `power_apply`, `window_request_refresh`, `screen_keyboard`. |

## nue-android (REQUESTS_android.md)

| Ask | Answer |
|---|---|
| 1 SDL3 backend must not set app state | **Done** in the contract: `owns_lifecycle = NUE_TRUE`; `nue_backend_init.platform_owns_lifecycle` tells the backend. |
| 2 window id on surface events | Yes: backend and platform fill the main window id (SDL3 backend will, from the window's native handle). Core also ignores events for unknown ids without failing. |
| 3 `SURFACE_LOST` synchronous | Watchers run synchronously inside `nue_internal_push_event`; `nue_gfx_set_auto_events(gfx, TRUE)` suspends the swapchain in that call. In PREPARE mode (current D3D9 path) the owner reacts in its `nue_gfx_add_listener` callback, also synchronous. |
| 4 capabilities in `init()` | Yes (read on every query). |
| 5 JNI_OnLoad | Agreed and documented in `PORTING.md`: only SDL's `JNI_OnLoad` exists; the platform layer receives the `JavaVM`/Activity through the SDL3 backend's Android helpers (`SDL_GetAndroidJNIEnv`). Load `libnue.so` before the game library. |
| memory | `memory_info_ex` (process limit from the ActivityManager memory class, VRAM/unified flags); budgets are policies, never a 2 GB constant. |

## nue-windows

Same API additions apply: `memory_info_ex`, `gpu_probe` (DXGI adapters: BC support, VRAM, unified memory flag for integrated GPUs), `window_request_refresh` (no-op where the OS has none), TDP stays unset on desktops. Timer resolution and Game Mode are unchanged.

## nue-texture

* `include/nue/texture.h` is yours; `nue.h` includes it automatically when it is installed. CMake: `src/texture/CMakeLists.txt` + `target_sources(nue_texture_obj ...)`; `NUE_HAS_TEXTURE=1` is defined for consumers.
* GPU capability: `nue_gpu_probe()` works **before** any device exists (platform probe, otherwise a conservative table with `probed = FALSE`); `nue_gfx_get_gpu_caps()` after the context. Format families (`nue_format_family`) with `SAMPLE/FILTER/MIPMAPS/RENDER_TARGET/BLEND/SRGB/STORAGE/MSAA` flags and texture limits; `unified_memory` and `dedicated_memory_bytes` tell the budget which pool applies.
* Memory: register a `NUE_BUDGET_TEXTURE` budget (`nue_budget_register`), call `nue_budget_request` before creating GPU textures and `nue_budget_report` when freeing; implement the eviction hook. Limits come from policy/config (`budget.texture.limit_mb`, `budget.texture.fraction`); nothing is hard-coded.
* D3D9 seam: the translator calls `nue_texture` at `CreateTexture`/`UnlockRect`, passing the `nue_gpu_caps` it got from `nue_gfx_get_gpu_caps` (see `GFX_D3D9_SEAM.md`). `D3DFMT`/FOURCC <-> `nue_texture_format` mapping lives in the legacy layer, using your format table.

## Legacy sessions (d3d9-bgfx, platform-api)

`ABI.md` defines both ABIs, the guest memory models, handle tables, call tokens, import binding, per-API version tables and the package layout. Reserved: `include/nue_legacy/`, `src/legacy/` (CMake contract in `PORTING.md`).
New native features requested by the legacy layer: timers (`nue_timer_*`, done), frame limiter (done), multi-object waits (built legacy-side on `nue_cond`), DirectSound notification positions on ring voices (to be added to `mixer.h` with the mixer engine).
