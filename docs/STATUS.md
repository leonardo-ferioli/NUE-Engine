# STATUS (kept honest; updated with every commit that changes it)

Last update: first commit (API draft). Legend: **done** = implemented and covered by a passing automated test in this tree; **impl** = code exists, not tested here;
**skel** = skeleton / stub; **-** = nothing yet; **n/a** = does not exist on that platform.

## Pinned third-party

| Component | Version | Status |
|---|---|---|
| SDL3 | not yet added | planned: shallow submodule at `third_party/SDL`, release tag recorded here |

## Core + backends

| Module | Header | null backend | SDL3 backend | Tests |
|---|---|---|---|---|
| version, result names, last error | base/core | done | done (shared) | test_base |
| init/shutdown, allocator | core | - | - | - |
| log | core | - | - | - |
| config | core | - | - | - |
| hashing, clocks, sleep, threads/sync | core | - | - | - |
| events + lifecycle | app | - | - | - |
| displays/windows/cursor | window | - | - | - |
| input | input | - | - | - |
| audio streams | audio | - | - | - |
| mixer | mixer | - | n/a (no backend needed in manual mode) | - |
| VFS/files/async/verify/import | storage | - | n/a | - |
| power/thermal/game mode | power | - | n/a (platform layer) | - |
| system info + capabilities | system | - | - | - |
| gfx | gfx | - | - | - |
| diag | diag | - | n/a (platform layer) | - |
| C++ wrapper | nuepp | - | n/a | - |

## Platforms

| Platform | Layer | Status |
|---|---|---|
| Windows x64 / x86 | `src/platform/windows` | stub README only |
| Linux x64 | `src/platform/linux` | stub README only |
| Android arm64 | `src/platform/android` | stub README only |
| iOS arm64 | `src/platform/ios` | stub README only |

Nothing has been run on a real device or window by the core session: all verification is console-only (null backend / SDL dummy drivers).
