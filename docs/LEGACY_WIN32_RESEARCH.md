# Research: open-source work close to the Win32 subset and DirectInput layers

Scope: what already exists that does the same job as `legacy_win32` (kernel32 / user32 / gdi32 / advapi32 / winmm subset for a translated
32-bit guest) and `legacy_dinput` (DirectInput 8), so the NUE layers reuse tested logic instead of rediscovering it. DirectSound has its own
note (`NUE_RESEARCH_AUDIO.md`, other session). Searched on 2026-10-25 (GitHub API for licence, language, last push; repository trees; the
source of the three most promising projects was read locally in an isolated scratch folder, never executed).

Result in one line: **no public project provides a Win32 + DirectInput surface for a statically recompiled 32-bit PC game**; the nearest
prior art is a Linux/macOS loader (wibo), a DirectInput *virtual device* implementation (Xidi), Wine (the semantic reference and a large test
corpus) and the console-recompilation runtimes (kernel object tables, waits). Nothing public targets SPEED2.EXE's OS surface: the only NFSU2
projects found are a clean-room engine re-implementation (OpenUG2) and GameCube/PC-MW decompilations of *game logic*, none useful here.

## Shortlist, ranked by usefulness for SPEED2.EXE

Usefulness = covers what the game really calls (see `NUE_HANDOVER_WIN32_DINPUT.md`, inventory), readable and testable code, maintained, licence
that lets it into NUE.

| # | Project | URL | Covers | Lang | Licence | Last push | Plan |
|---|---|---|---|---|---|---|---|
| 1 | **wibo** | https://github.com/decompals/wibo | A tiny Windows-API host for 32-bit guests on Linux/macOS: `dll/kernel32` (files, handles, heap, sync, TLS/FLS, memory, time, winnls, sysinfo, processenv), advapi32 (registry), user32 stubs, `src/handles.*` (generation-checked handle table with a quarantine of freed slots, handle values kept below 0x8000 for old programs, reference-counted pinned objects, a named-object namespace, waitable objects with `WaitForMultipleObjects` waiter callbacks), `src/files.*` (`pathFromWindows`: case-insensitive resolution, trailing-dot rule, drive prefix), `src/strutil.*`; C test programs per API (`test/test_*.c`) | C++20 | MIT | 2026-10-17 | Adapt: waitable-object model, wait-multiple, path conversion rules, string/time helpers. Its code is bound to POSIX fds and to its own guest loader, so only the logic is reused (see `THIRD_PARTY_SOURCES.md`). Its test programs are used as test ideas |
| 2 | **Xidi** | https://github.com/samuelgr/Xidi | A DirectInput 8 *device implementation* over XInput: `DataFormat` (SetDataFormat object/offset/instance semantics), `StateChangeEventBuffer` (buffered `GetDeviceData`, overflow, ring), `VirtualDirectInputDevice` (property handling, acquire rules, EnumObjects), mocks and unit tests | C++ | BSD-3-Clause | 2026-09-25 | Adapt the data-format and event-buffer logic into `legacy_dinput`'s Level A; use its tests as oracles |
| 3 | **Wine** | https://github.com/wine-mirror/wine (mirror of the official tree) | The reference semantics of everything: `dlls/kernel32`, `ntdll`, `user32`, `dinput` / `dinput8` (device.c: action maps, `GetDeviceData`, properties; keyboard/mouse/joystick), `winmm` (time), with `*/tests/*.c` conformance tests that also run on real Windows | C | LGPL-2.1-or-later | 2026-10-24 | **Read, do not copy**: behaviours are re-derived in our own code; its tests are used as oracles (their ideas, not their files). LGPL: flagged below |
| 4 | xenia (canary) | https://github.com/xenia-canary/xenia-canary | Xbox 360 emulator: kernel object table, `XEvent` / `XMutant` / `XSemaphore`, wait semantics, thread model for a guest with its own TEB | C++ | BSD-3-Clause (plus third-party notices) | 2026-10-25 | Reference for the guest handle table and wait semantics (the design in NUE `ABI.md` 2.3); nothing copied |
| 5 | SDL3 | https://github.com/libsdl-org/SDL | Window, events, timers, joystick/gamepad mapping: the NUE platform base | C | Zlib | 2026-10-25 | The host backends of the platform interface (input device snapshots, window events, timers) are what NUE already builds on SDL |
| 6 | dinputto8 / dxwrapper | https://github.com/elishacloud/dinputto8, https://github.com/elishacloud/dxwrapper | DirectInput 1-7 -> 8 and wrappers for old DirectX DLLs | C++ | Zlib (dinputto8); dxwrapper: mixed, see its `License.txt` | 2026-08/10 | Not needed (the game uses DirectInput 8 only); read for wrapper conventions |
| 7 | OpenUG2 | https://github.com/whoismept/OpenUG2 | Clean-room NFSU2 engine (game logic, formats, HUD) | C | MIT | 2026-10-25 | No OS surface; useful for game formats only |
| 8 | mingw-w64 headers | https://github.com/mingw-w64/mingw-w64 | Struct layouts, constants, GUIDs of the Win32 / DirectInput headers | C | public domain / ZPL parts | 2026-10-24 | Check constants and structure layouts against ours; nothing copied |
| 9 | XenonRecomp / N64Recomp / xboxrecomp | https://github.com/hedge-dev/XenonRecomp, https://github.com/N64Recomp/N64Recomp, https://github.com/sp00nznet/xboxrecomp | Static recompilers and the runtime shape around them (how a recompiled program meets its OS) | C++ / C | MIT | 2025-08 / 2026-05 / 2026-10 | Design reading only (the runtime layers are console-specific) |
| 10 | rexglue-sdk | https://github.com/rexglue/rexglue-sdk | Xbox 360 recompilation runtime derived from xenia | C | BSD-style (xenia derived) | 2026-10-02 | Design reading only |

Do not copy (copyleft, listed so nobody does by accident):

| Project | Licence | Why it matters |
|---|---|---|
| UnleashedRecomp (https://github.com/hedge-dev/UnleashedRecomp) | **GPL-3.0** | Closest in spirit (recompiled PC port with its own OS surface) but GPL: reading the architecture is fine, **no code may enter NUE** |
| ssbu-recomp (https://github.com/OpenSmash-Ultimate/ssbu-recomp) | **GPL-3.0** | Same |
| ReactOS (https://github.com/reactos/reactos) | **GPL-2.0** (parts LGPL) | Re-implementation of kernel32/user32 but GPL; its `dll/directx/wine` copy carries Wine's LGPL code |
| Cxbx-Reloaded (https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) | **GPL-2.0** | Xbox kernel layer, GPL |
| SR (https://github.com/M-HT/SR) | **no licence file** | Static recompiler of DOS/Windows executables; without a licence nothing may be copied |
| uwin (https://github.com/DCNick3/uwin) | **no licence file**, Rust, last push 2023 | Portable Win32 layer idea; unlicensed and stale |

## Findings that change the design

1. **A handle table with waitable objects is the one thing every project rebuilds** (wibo `handles.*`, xenia object table, NUE `ABI.md` 2.3).
   `legacy_win32` therefore gets a small generation-checked `HandleTable` with typed objects, and `WaitForMultipleObjects` is built on
   per-object waiter callbacks (the wibo shape), not on polling.
2. **Path rules worth copying as rules**, not code: `\` to `/`, strip `\\?\`, drop a drive prefix, per-component case-insensitive lookup,
   trailing-dot components are the same file ("foo." == "foo"), `.` and `..` untouched. The game-folder-as-root design already decided for NUE
   storage is a superset of this.
3. **DirectInput buffered data is the part the game depends on** (`GetDeviceData`, action maps) and the one thing Xidi implements and tests
   thoroughly (ring buffer, overflow reporting, `DIDFT` instance matching).
4. **Wine's tests are the oracle**: `kernel32:sync`, `kernel32:file`, `kernel32:heap`, `kernel32:thread`, `user32:msg`, `dinput:device`, `dinput:keyboard`.
   They run on real Windows too, so a behaviour can be checked against the genuine implementation: that is how our differential tests work
   (same call sequence through the portable implementation and through the host OS, results compared).
5. Nobody documents SPEED2.EXE's use of the OS surface; the inventory in the handover is first-hand (import table, traces, profile windows).

## Licences found (for review)

MIT (wibo, OpenUG2, xboxrecomp, XenonRecomp, N64Recomp), BSD-3-Clause (Xidi, xenia), Zlib (SDL3, dinputto8, DXVK), LGPL-2.1+ (Wine,
DSOAL, OpenAL Soft), **GPL** (UnleashedRecomp, ssbu-recomp, ReactOS, Cxbx-Reloaded), **no licence** (SR, uwin), mixed (dxwrapper, rexglue).
NUE itself had no `LICENSE` file in the local clone when this was written: whoever decides the NUE licence must check the borrowed set against
it (MIT and BSD-3 are compatible with permissive licensing; LGPL code would require dynamic-linking or replacement terms; GPL is excluded).
The code actually borrowed is listed in `THIRD_PARTY_SOURCES.md`.
