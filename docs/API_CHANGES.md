# API changes

Rules: while the API is < 1.0 additive changes are announced here and by message to the platform sessions; breaking changes are
avoided and, when unavoidable, listed under "Breaking" with the migration. Platform sessions request changes in
`docs/REQUESTS_<platform>.md` (or by message); the core session decides and records the answer here.

## 0.1.0 (draft, first commit)

Initial public headers: base, core, app, window, input, audio, mixer, storage, power, system, gfx, diag, entry, null_backend.
Internal contracts for implementers: `src/core/internal/backend.h` (devices) and `src/core/internal/platform_ops.h` (per-OS layer).
