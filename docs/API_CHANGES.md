# API changes

Rules: while the API is < 1.0 additive changes are announced here and by message to the platform sessions; breaking changes are
avoided and, when unavoidable, listed under "Breaking" with the migration. Platform sessions request changes in
`docs/REQUESTS_<platform>.md` (or by message); the core session decides and records the answer here.

## 0.1.0 (draft, first commit)

Initial public headers: base, core, app, window, input, audio, mixer, storage, power, system, gfx, diag, entry, null_backend.
Internal contracts for implementers: `src/core/internal/backend.h` (devices) and `src/core/internal/platform_ops.h` (per-OS layer).

## 0.1.0 follow-ups (core block 1, additive, internal interfaces)

* `src/core/internal/backend.h`: `nue_window_snapshot` gained `nue_bool relative_mouse` (set it when relative mouse mode is active).
* Backends should call `nue_internal_window_changed()` (or push through the null-backend pattern: update the snapshot before announcing the event) so that
  `nue_window_get_*` is never older than the events that announce a change.
* `include/nue/app.h`: the engine loop also runs inside the OS entry point on mobile (SDL_main); no special mobile call path. `nue_app_run` keeps the loop alive in the background.
* Public C ABI: no change.
