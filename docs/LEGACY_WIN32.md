# legacy_win32 and legacy_dinput: the Win32 subset and DirectInput of a translated 32-bit game

Owner: legacy (win32/dinput). Enable with `-DNUE_WITH_LEGACY_WIN32=ON` (needs only `nue::nue`; builds with `NUE_WITH_LEGACY_D3D9=OFF`).

## Layout in this repository

```
include/nue_legacy/status.hpp               Id, Status, Result, spans; status_from(nue_result) / result_from(Status)
include/nue_legacy/guest_memory.hpp         nue::legacy::guest::Memory (shared with legacy_dsound): the one door between a 32-bit guest address and the host
include/nue_legacy/platform/*.hpp           the interfaces the layer is written against (Clock, Threads, Storage, Config, System, Input, Window, Display),
                                            std_platform.hpp (std implementations), null_services.hpp / null_window.hpp (deterministic fixtures), nue_platform.hpp
include/nue_legacy/win32/*.hpp              Level A modules: time, text, registry, sync, files, memory (+ arena_memory), process, window, seh, guest_threads
include/nue_legacy/win32/host/*.hpp         the real Windows behind a Level A interface (registry, address space, polling input); Windows only
include/nue_legacy/dinput.hpp               DirectInput 8 device model
src/legacy/win32/{src,host,platform,nue_backend,tools}   implementations (nue_backend: the interfaces over nue_*; tools/gen_text_data.cpp regenerates src/text_data.inc on Windows)
src/legacy/dinput/src                       DirectInput implementation
tests/legacy/win32                          headless tests (ctest nue_test_legacy_win32_<module>); *_windows compare with the real OS; nue/ = platform conformance suite
```

Targets: `nue::legacy_platform`, `nue::legacy_win32`, `nue::legacy_dinput`, `nue::legacy_win32_nue` (the interfaces over the native API), `nue_legacy_win32_host` (Windows only).
Namespaces: `nue::legacy::win32`, `nue::legacy::dinput`, `nue::legacy::platform`, `nue::legacy::guest`. C++20 per target (`cxx_std_20` PUBLIC on `nue_legacy_platform`); the native stack stays C++17.

**What is NOT in this repository: the facade.** The code that knows a particular guest's calling convention (stdcall words on the guest stack), guest pointers and COM objects
belongs to the host that runs the guest (today the NFS Underground 2 port, `win32_facade*.cpp` there). The facade is the reference for which arguments and COM layouts a game
uses; NUE's own facade is written against `docs/ABI.md`.

Differences between the text below and this tree (the text was written in the port repository): `legacy_win32/` is `include/nue_legacy/win32` + `src/legacy/win32`;
`platform/` is `include/nue_legacy/platform` + `src/legacy/win32/platform`; `nfsu2::` is `nue::legacy::`; `guest::Memory` is `nue::legacy::guest::Memory`; ctest names
carry the prefix `nue_test_`; `docs/legacy/*.tbl` and the inventory tools are in the port repository.


Audience: the NUE agents that will absorb this as `legacy_win32` and `legacy_dinput`. No game data and no personal paths are in this file.
Companion files: `docs/LEGACY_WIN32_RESEARCH.md` (prior art, licences), `docs/THIRD_PARTY_SOURCES.md` (borrowed code: **none**),
`docs/legacy/win32.tbl`, `docs/legacy/dinput8.tbl` (the NUE version tables), `docs/legacy/win32_inventory.md` (the full inventory table).

## 1. What the game needs

`SPEED2.EXE` imports 252 functions; 249 are in scope here (Direct3D, DirectDraw and DirectSound entry points are other layers).
By DLL: kernel32 152, user32 38, ws2_32 22, gdi32 12, tapi32 9, advapi32 6, winmm 6, the rest a handful. The inventory
(`tools/win32_inventory.py`) joins the import table with the game's own call traces (menu, loading, garage, race): **130 of the 249 are reached**
in the traced sessions. 119 are not called there; that is what the traces show, not a proof that the game never calls them.

| Area | Imports | Reached | What the traces show (call counts are measured; the purpose is read from the call sites) |
|---|---:|---:|---|
| time (+ timers) | 15 | 14 | QueryPerformanceCounter is the hottest call of the game (2,500 to 63,000 calls per frame depending on the scene), GetTickCount / timeGetTime, `timeSetEvent` (multimedia timer callbacks into guest code), `Sleep` |
| sync | 29 | 20 | critical sections (about 175 enters per frame), TLS (about 57 `TlsGetValue` per frame), events, mutexes, `CreateThread`, waits |
| memory | 16 | 7 | the C runtime's heaps (`HeapAlloc` about 160 per frame), `VirtualAlloc` for large blocks and for the interpreter's generated code |
| process | 32 | 21 | `GetLastError`, module handles, `GetProcAddress` (it looks up optional entry points), version, system info, command line, environment, `OutputDebugStringA` |
| files | 37 | 21 | `CreateFileA` / `ReadFile` / `SetFilePointer` on relative paths under the game folder, `FindFirstFileA`, `GetFullPathNameA`; the bridge also treats overlapped opens specially |
| text | 20 | 8 | code-page conversion, `CompareStringA`, `LCMapStringA`, locale queries made by the C runtime start-up |
| registry | 6 | 6 | the game's own settings keys (the mods already redirect them to a settings file) |
| window | 36 | 30 | window class + window, message loop, cursor, focus, `GetSystemMetrics`, `AdjustWindowRect` |
| gdi | 12 | 1 | `DeleteObject` only |
| net (ws2_32, tapi32, comm) | 41 | 0 | never called |
| seh | 4 | 1 | `RtlUnwind`, unhandled-exception filter |
| DirectInput | 1 import + 2 COM interfaces | yes | `DirectInput8Create`, then `CreateDevice`, `EnumDevices`, `EnumDevicesBySemantics`, `Acquire`/`Unacquire`, `GetDeviceState`, `GetDeviceData`, `SetDataFormat`, `SetCooperativeLevel`, `Poll`, `BuildActionMap`, `SetActionMap` |

Two facts that shape the design: `OutputDebugStringA` costs tens of milliseconds per frame in a traced menu when a debugger or a listener is
attached, so a layer must answer it with a counting sink; and the QPC/GetTickCount calls are so frequent that they must be a clock read and
nothing else.

## 2. The layers and where each file is

```
legacy_win32/                 the library (target legacy_win32, tests legacy_win32_tests, ctest names legacy_win32_*)
  include/legacy_win32/       Level A interfaces: no Windows header, no guest pointer, no COM object, no host call, no game knowledge
  src/                        the portable implementations
  host/                       Windows backends (the real OS behind a Level A interface); Windows only: registry, address space, input (XInput + polling)
  tests/                      headless tests; the *_windows ones compare with the real OS
  tools/gen_text_data.cpp     regenerates src/text_data.inc from the Windows NLS answers (Windows only)
platform/                     the platform interfaces the library is written against, shaped like NUE's native headers:
  clock.hpp (core.h time_*), threads.hpp (core.h thread/mutex/cond/semaphore/tls/timer), storage.hpp (storage.h), config.hpp (core.h config_*),
  system.hpp (system.h), input.hpp (input.h), window.hpp / display.hpp (window.h)
  std/                        implementations over the C++ standard library (the default on every host)
  null/null_services.*        deterministic implementations: virtual clock, in-memory file system, scripted input, null system
win32_facade*.{hpp,cpp}       THE FACADE: the only code that knows the guest calling convention (stdcall words on the guest stack), guest pointers and COM
platform/win32_facade_tests.cpp   the facade driven the way translated code calls imports (console test, no game code run)
tools/win32_inventory.py, tools/win32_api_status.py   inventory and the function-by-function status table (data in the .py, output in docs/legacy)
```

Chain: guest (translated code, 32-bit identity memory) -> **facade** (`win32_facade*.cpp`, registered as import overrides of the bridge) ->
**Level A** (`legacy_win32`: plain handle-based C++, no pointers) -> **platform interfaces** (Clock, Threads, Storage, Config, System, Input,
Window, Display) -> **backends** (std, null, Windows host). Level A never includes a Windows header; the only Windows code is under `host/`.

What NUE takes: `legacy_win32/include`, `legacy_win32/src`, `legacy_win32/tests`, the null services (they are the test fixtures), and **rewrites the
facade** against its own guest ABI (`docs/ABI.md`): the facade here is the reference of which arguments and COM layouts the game uses. The
platform interfaces are deliberately close to the NUE native headers so that the std/null backends are replaced by NUE's own (`core.h`, `storage.h`,
`input.h`, `window.h`).

### Original versus borrowed

All of `legacy_win32`, `platform/`, the facade and the tests are **original code written for this project**. Nothing was copied from another
project (`THIRD_PARTY_SOURCES.md`: "none yet"). Behaviour rules were *read* in wibo (MIT) and checked against the real Windows (not against Wine
source); the DirectInput buffer model is checked against the real `dinput8.dll`. Generated data: `src/text_data.inc` is produced from Windows' own NLS
answers by `tools/gen_text_data.cpp` (data about the system, not code from a project). **No GPL or LGPL code is used.** Licence flags for what NUE might
want to borrow later are in the research note (Wine is LGPL, ReactOS and the Linux recompilation runtimes UnleashedRecomp / ssbu-recomp are GPL: do not copy).

## 3. The Level A modules

| Module | Header | What it models | Verified against Windows |
|---|---|---|---|
| handles | `handles.hpp` | generation-checked handle table with typed objects; guest handles are in a biased range (0x00C00000 and up) so real OS handles can still be passed to real functions in a mixed system | unit (55 checks) |
| time | `time.hpp` | ticks, QPC, FILETIME <-> SYSTEMTIME, local time, time zone, `timeBeginPeriod` | 80,000 random conversions against kernel32 |
| text | `text.hpp` | code pages, best-fit, character classes, case mapping, collation (`CompareString`, `LCMapString`), locale data, `wsprintf` | tables generated from Windows NLS; every function compared (classes, collation 6,000 string pairs per flag set, 1,255 locale checks, wsprintf cases) |
| registry | `registry.hpp` | keys, typed values, enumeration, deletion; `ConfigRegistry` over `platform::Config`, and a Windows host backend | scripted run against the real registry |
| sync | `sync.hpp` | events, mutexes, critical sections, TLS, threads (exit codes, suspended start), multimedia timers, `WaitFor*` exact on several objects (no polling) | scenarios against kernel32 (`sync_windows`) |
| files | `files.hpp` | the CreateFile dispositions and errors, share and access modes, find, paths (drive letters, `\\?\`, trailing dots), mappings, standard handles | 87 observations against kernel32 |
| memory | `memory.hpp`, `arena_memory.hpp` | `AddressSpace` with the Win32 region rules and page protections in a software table (4 KiB guest pages, never the host MMU), size-class heaps, GlobalAlloc, `IsBad*Ptr`; `make_arena_address_space(guest::Memory&, window)` (portable) and a Windows host address space; `VirtualApi` (announces memory that becomes executable outside the image, register only); `ArenaMemory` (a portable `guest::Memory` for tests and tools) | 33 observations against VirtualAlloc / HeapAlloc |
| process | `process.hpp` | module table + exports, `GetProcAddress`, environment, version, system info, processor features, toolhelp (own process), priorities, error mode, `OutputDebugString` sink, exit | 16 checks against kernel32 (host-derived facts compared) |
| window | `window.hpp` | classes, windows, queues, `GetMessage` / `PeekMessage` / `PostMessage` / `DefWindowProc`, focus, cursor, platform input and window events as `WM_*` | message sequences against user32 |
| seh | `seh.hpp` | `RaiseException`, `RtlUnwind`, `UnhandledExceptionFilter`, `SetUnhandledExceptionFilter` as a state machine over guest registers and memory (`GuestCpuState`), tokens for handler returns; executes no guest code | scripted CPU + guest memory (56 checks); the i386 rules are ported from the bridge's state machine |
| guest_threads | `guest_threads.hpp` | `CreateThread` / `ExitThread` and `timeSetEvent` / `timeKillEvent` over `Sync` and the engine interfaces `GuestExecutor` / `GuestContext` (event callbacks, shared timer context, a timer killing itself, faults stop the process) | fake engine (43 checks) |
| dinput | `dinput.hpp` | device enumeration, data formats, state, buffered data, properties, cooperative levels, acquire / poll, capabilities, action maps for keyboard and mouse | call rules and results against the real `dinput8.dll` (7 differential checks, 0 differences) |

## 4. The facade and how it is switched on

`Win32Facade` (`win32_facade.hpp`) registers import overrides with the bridge. Groups: `time,text,process,sync,memory,files,dinput`, and, for hosts without a real Windows, `registry` (the Reg* imports over `ConfigRegistry`, seeded with the game's keys, persistable as text) and `traps` (the 52 network, serial, TAPI and GDI imports the game never calls answer with a logged failure; `DeleteObject` succeeds on nothing). `all` includes `registry` and `traps` only where there is no real OS, because on 32-bit Windows the mods' registry policy and the real functions are in place. The window group is intentionally not a facade group, see section 7. It is **off by default**: `--win32-layer[=groups]`, ini key `win32_layer`, or
environment `NFSU2_WIN32_LAYER`. Behaviour with the layer off is bit-for-bit the previous behaviour. With it on, the facade logs which groups are
installed (`Win32 layer (legacy_win32): ...`).

Rules the facade follows (each one is a trap if ported wrongly):

- A handle above 0x00C00000 belongs to the library; anything else goes to the real function. `WaitForMultipleObjects` over both kinds is polled (not atomic).
- With a real OS (`NFSU2_FACADE_REAL_OS`, 32-bit Windows only) overlapped or special `CreateFile` stays real. Without one the facade is pure: no `windows.h`, a handle the library does not own is ERROR_INVALID_HANDLE, an overlapped file is an ordinary file whose `ReadFile`/`WriteFile` with an OVERLAPPED complete before they return, `GetOverlappedResult` reads the result from the structure. That pure path is compile-checked for a 64-bit target and has no run test (the facade test needs the 32-bit bridge).
- `CreateThread`, `timeSetEvent`/`timeKillEvent`, SEH, `VirtualAlloc`/`VirtualProtect` (dynamic-code announcement), module table and `GetProcAddress` are in the bridge today but exist as Level A with explicit engine interfaces (`guest_threads.hpp`, `seh.hpp`, `VirtualApi`, `ProcessApi::set_proc_resolver`); the port connects its execution engine by implementing `GuestExecutor`/`GuestContext` and `GuestCpuState`. They are tested with a fake engine, not yet connected to the bridge.
- Infinite waits and `Sleep` are done in 100 ms slices so that a stopping host interrupts them.
- `SetThreadPriority(TIME_CRITICAL)` is capped to ABOVE_NORMAL (2026-10-16 incident: a thread at TIME_CRITICAL starved the audio thread and the system).
- DirectInput COM objects are `{vtable, id}` in guest memory; method tokens are native hooks; callbacks into the guest run through the bridge.

## 5. Every Windows behaviour found (read before porting)

Found by running the same scripted scenario on the real OS and on Level A; every item below was a difference at some point and is now a test.

Synchronisation and time
- Object names (events, mutexes) are **case-sensitive** in the Win32 namespace; file names are not.
- `TlsGetValue` of a freed index returns 0 with **no error**; `TlsFree` of it fails with ERROR_INVALID_PARAMETER.
- `WaitForMultipleObjects` with `bWaitAll` and a duplicated handle fails with ERROR_INVALID_PARAMETER; a wait on an invalid handle fails with ERROR_INVALID_HANDLE.
- `timeKillEvent` of an unknown id returns 97 (`TIMERR_NOCANDO`), not a generic error.
- `SetEnvironmentVariable` with a null value for a missing variable succeeds.

Registry, text
- `RegQueryValueEx` of a missing value zeroes `*lpType`.
- `wsprintf` is not printf: the flags are only `-`, `#`, `0`; there is no `*`; an unknown conversion drops the `%` and prints the character.
- Collation: `CompareString` flags 0x1001 and sort keys are compared over 6,000 pairs; `SORT_STRINGSORT` ignored-character positions need a position-encoded sort key.
- Best-fit tables are binary-searched on the sorted key (an off-by-one on the upper bound was found by the differential test).

Files and memory
- A file opened with `fopen` on Windows does not share by default in every mode: open with `_wfsopen(..., _SH_DENYNO)` and enforce share modes in the Win32 layer.
- Resizing a file by name while it is open fails on Windows: resize through the open descriptor (`_chsize_s`).
- `VirtualFree` with an address that is not the region base fails with 487 (ERROR_INVALID_ADDRESS).
- The size of the free region reported by `VirtualQuery` is not compared (it depends on the host layout).
- Real `HeapSize` on a foreign pointer and `HeapDestroy` of the process heap crash the process: they are not in the differential test (undefined on Windows).
- Heap blocks are 8-byte aligned; `HEAP_ZERO_MEMORY` zeroes on allocation and on growth in `HeapReAlloc`.

Windows and messages
- `DestroyWindow` sends WM_DESTROY then WM_NCDESTROY, children first; message `0x90` is undocumented and must be ignored; a loop that drains to WM_QUIT must use `PeekMessage`, because
  `GetMessage` blocks.
- Class names are case-insensitive; `UnregisterClass` with windows alive fails with ERROR_BUSY (170).
- Platform input (key, mouse, window events) becomes `WM_*` messages in `pump_platform`, called by get/peek: the same guest loop runs on a host with no Win32 window system.

DirectInput
- Check order and error codes of every device call follow dinput8 (e.g. `Acquire` before `SetCooperativeLevel` fails with E_INVALIDARG 0x80070057).
- The buffered ring keeps **one slot free** (a buffer of N holds N-1 events); overflow is reported once and the flag is cleared by the read that returned it.
- Keyboard keys are DIK codes = set-1 scancodes (+0x80 for extended keys); mouse buttons are ordered left, right, middle; `SysKeyboard` and `SysMouse` are the system GUIDs.
- `DIPROP_*` property identifiers are passed as small integers, not pointers.
- `DirectInput8Create` with a version below 0x0800 fails with 0x8007047E (DIERR_OLDDIRECTINPUTVERSION). The facade also **zeroes** `*ppv` before every check; that is the author's choice and the real DLL was not compared on it (a test that reuses the output variable sees 0).

Process
- `GetVersionEx` is a configured value; the facade copies the host's so that live behaviour does not change. Processor feature bits come from `platform::SystemInfo::pf_mask`.

Environment hazards
- Heredocs and `sed` mangle backslashes in this environment: write C++ string literals with the editor tool.
- Evaluation order of call arguments is unspecified (the MSVC ABI evaluates right to left): never write `f(g(e), e)` in an observation.
- `DrawState`, `small` and `min` are macros in `windows.h`: a header that includes it must not use those names.

## 6. Deviations from real Windows (documented, deliberate)

- `GetTimeZoneInformation` has no daylight-saving schedule: only the offset in effect now.
- `GetTickCount` / `timeGetTime` start at the process start plus five minutes, not at the machine's boot.
- Text: the repertoire is Latin, Greek and Cyrillic; everything else is "defined, not a letter" (case mapping above U+04FF: 573 of 62,208 code points differ, measured). Locale data is en-US for every LCID. `SORT_STRINGSORT` differs in rare cases.
- `GlobalAlloc` with `GMEM_MOVEABLE` is treated as fixed.
- Threads: `SuspendThread` and `TerminateThread` of a running thread are unsupported (the platform cannot stop a thread from outside).
- DirectInput: no force feedback or effects, no configuration UI (`ConfigureDevices` returns DIERR_UNSUPPORTED), and **action maps map keyboard and mouse semantics only**: the genres of
  game controllers stay unmapped.
- Toolhelp lists only the game's own process; drive queries answer for the mapped game folder.
- File mappings and resources are partial (the facade leaves them to the real system).

## 7. What is missing

1. **Live validation.** Everything above was run headless. Nobody has run the game with `--win32-layer` on; whether each facade group is behaviour-neutral in the real game is unverified. (The boot tests of the 32-bit build pass, but they drive `fast_dynamic_tests`, not the launcher, so they do not exercise the layer.)
2. **Registry facade wiring (done for hosts without a real OS: group `registry`, tested).** On 32-bit Windows it is not in `all` and the mods' policy still answers. Original note: `legacy_win32::Registry` is complete and tested, but the game's registry calls are still answered by the mods' settings-file policy (`RegistryWrapper`, `RegistryFallback`) in the bridge. NUE should use `ConfigRegistry`.
3. **Window group on Windows** stays real by design (the game owns a real `HWND` with a real procedure; NUE's ABI says to attach it). The Level A window system is for hosts without Win32; it is tested but not driven by the game.
4. **Thread, timer, SEH, module and exit**: Level A has them complete (section 11); nothing connects them to the bridge yet, so the port's bridge still runs its own copies on Windows.
5. **Game-controller genre mappings.** `BuildActionMap` / `SetActionMap` log the game's `DIACTIONFORMAT` when `NFSU2_TRACE_INPUT=1` (lines `DI action format ...` and `DI action[i] semantic=... objid=... how=...`).
   Someone must play with a controller and send that log: the game's semantics (genre, action numbers) are not known without it. Until then controller actions are unmapped.
6. **`HostAddressSpace` (Windows VirtualAlloc backend) and `WindowsInput` (polling keyboard, mouse, XInput)** are written and compiled, never checked against a real device by this project's tests (they use the scripted null input). Limits of `WindowsInput`: the cursor stops at the screen edge, the wheel is not seen, keys are sampled at the pump rate.
7. **Mixed-kind waits** (library handle plus real handle in one `WaitForMultipleObjects`) are polled, not atomic.
8. **GDI and network traps** (`status trap`, facade group `traps`): 41 network/serial/TAPI and 11 GDI functions were never called in the traced sessions; they answer with a logged failure when the group is on; with the group off the bridge passes them to the host. NUE must answer them with a logged failure.
9. **Statuses:** the table has no `pending`. `partial`/`stub` rows say what differs.

## 8. Tests and how to verify

All headless, deterministic (virtual clock, in-memory storage, scripted input); the `_windows` ones are the differential tests against the host OS.

| ctest name | What it checks |
|---|---|
| `legacy_win32_handles` | handle table: generations, types, duplication, bias range |
| `legacy_win32_time`, `_time_windows` | tick / QPC / FILETIME rules; 80,000 random conversions against kernel32 |
| `legacy_win32_text`, `_text_codepages_windows`, `_text_classes_windows`, `_text_collation_windows`, `_text_locale_windows`, `_text_wsprintf_windows` | code pages, classes, case mapping, collation, locale data and `wsprintf` against Windows |
| `legacy_win32_registry`, `_registry_windows` | registry rules over `Config`; scripted run against the real registry |
| `legacy_win32_sync`, `_sync_windows` | events, mutexes, waits, CS, TLS, threads, timers |
| `legacy_win32_files`, `_files_windows` | CreateFile rules, share modes, find, paths; 87 observations against kernel32 |
| `legacy_win32_window`, `_window_platform`, `_window_windows` | message queue, focus, destruction order; platform events as `WM_*`; sequences against user32 |
| `legacy_win32_dinput`, `_dinput_windows` | the device model (98 checks); call rules against the real DirectInput 8 |
| `legacy_win32_process`, `_process_windows` | modules, environment, version, system info, toolhelp; host-derived facts compared |
| `legacy_win32_memory`, `_memory_windows` | address space, heaps; 33 observations against VirtualAlloc / HeapAlloc |
| `legacy_win32_arena_memory` | `ArenaMemory` allocation zone (alignment, zero fill, reuse, coalescing, exhaustion), the address space over a `guest::Memory` window (refused when unmapped), software page protections, guest address != host address on 64 bits (23 checks) |
| `legacy_win32_virtual_api` | executable regions announced outside the image and retired on release; errors pass through (16 checks) |
| `legacy_win32_seh` | RaiseException chain walk, ContinueSearch / ContinueExecution, non-continuable and bad dispositions, unbalanced stack, top-level filter, RtlUnwind with and without target and given record, purge of dead states (56 checks) |
| `legacy_win32_guest_threads`, `_guest_timers` | CreateThread / ExitThread / suspended start / faults / shutdown (19 checks); timeSetEvent function and event callbacks, kill from the callback, one shared context, PulseEvent (24 checks) |
| `legacy_win32_event_input` | the portable event-fed input: snapshots follow `push` at `pump`, listeners see every event in order (14 checks) |
| `win32_facade` (32-bit build only) | the facade through the bridge: calls the imports as translated code does, first on the real OS and then with the layer on, results compared (text, version, system info, FILETIME, ticks, events, heaps, files, DirectInput through its COM vtables, and the registry and traps groups: defaults, persistence through the Config text, failures with the right stack cleanup) |

How to run:

```
# standalone library, any host with a C++20 compiler (x64 build, this is what NUE would run)
cmake -S legacy_win32 -B build_w64 -G Ninja -DCMAKE_CXX_COMPILER=clang++
cmake --build build_w64 && ctest --test-dir build_w64

# inside the game repository (32-bit build also runs the legacy tests and the facade test)
cmake --build build32 && (cd build32 && ctest)

# the layer in the game (NOT run by the author: the user plays)
nfsu2_launcher --win32-layer                      # every group
nfsu2_launcher --win32-layer=time,sync,dinput     # chosen groups
NFSU2_TRACE_INPUT=1 ...                            # log DirectInput action formats (see 7.5)

# regenerate the tables
python tools/win32_inventory.py --trace <native_trace.log> [--trace ...] --tbl docs/legacy --markdown docs/legacy/win32_inventory.md
```

## 9. Licences

Used: nothing copied. Read only: wibo (MIT), Xidi (BSD-3-Clause), the Wine tests (LGPL-2.1+, behaviour only, no text). Flags if NUE decides to copy later: **Wine LGPL**,
**ReactOS GPL**, **UnleashedRecomp GPL-3.0**, **ssbu-recomp GPL-3.0**, **SR and uwin have no licence file**. NUE has no `LICENSE` file in the local clone: the licence of NUE must be
checked against the borrowed set before anything is copied. Details: `LEGACY_WIN32_RESEARCH.md`.

## 10. Porting order suggested

1. `platform/*` interfaces <- NUE native headers (they are the same shape); keep `null` services as test fixtures.
2. `legacy_win32` (time, handles, sync, files, memory, process, text) with their tests: no facade needed to run them.
3. `legacy_dinput` (`dinput.hpp`) and a facade over NUE's `input.h`; then obtain the controller log (7.5) and add the genre tables.
4. Window system last: on Windows keep the real `HWND`; elsewhere use `WindowSystem` fed by NUE's window and input events.

## 11. The 64-bit guest model and the engine interfaces (added 2026-10-27)

The guest is a 32-bit program in a 4 GiB arena on any 64-bit little-endian host (guest address `a` is host address `base + a`; no host pointer is ever visible to the guest).
What that changed in this library:

- **Level A never treated a guest address as a host address** (strings and structures enter as `std::string` / spans / values); what held host pointers was `AddressSpace::host()`
  and the facade. `AddressSpace` is now built over a `guest::Memory` (`guest_memory.hpp`: `alloc`, `free`, `host(address, bytes)`, `address_of`): `make_arena_address_space(memory, window)`
  manages the window `[0x10000, 0x10000 + window)` of the guest range, and keeps region state and page protections in a software table of 4 KiB guest pages, so hosts with 16 KiB
  pages behave the same. `ArenaMemory` is a portable `guest::Memory` (one block of host memory) used by the tests; the port and NUE bring their own.
- **The facade** converts guest addresses only through `guest::to_host` / `guest::ptr` (identity on 32-bit Windows: same code, same behaviour), allocates guest-visible data (COM objects,
  callback strings) through the guest heap, takes the guest memory in `Win32LayerOptions::guest_memory` and contains `windows.h` only under `NFSU2_FACADE_REAL_OS`.
- **Engine interfaces** (what the execution engine implements; Level A executes no guest code): `GuestExecutor` / `GuestContext` (`create_context`, `call(function, args)`, `is_code`),
  `GuestCpuState` (`regs`, `set_regs`, `teb`) for SEH, `SehTokens` (guest return addresses routed to `Seh::handler_returned`), `VirtualApi` listeners (executable regions announced and released), and
  `ProcessApi::set_proc_resolver` (the import binder: an import token for `GetProcAddress` of a name that is not registered).
- **Input on any host**: `platform::null::NullInput::push(InputEvent)` is the portable input of every host that has an event pump (SDL3, NUE's `nue_event`, Android, iOS): the backend translates
  each OS event into an `InputEvent` (scancodes are USB HID usages as in SDL and NUE) and pushes it; snapshots and listeners follow at `pump()`. The Windows-only polling backend (`windows_input`) is the
  one OS-specific input in this repository; the per-OS backends for Linux, Android, macOS and iOS are a few lines each over `push` and live with the OS layer (NUE's input module), not here.
- **PulseEvent** now releases the threads blocked on the event at that moment (all for manual-reset, one for auto-reset), as Windows does (the library used to lose the pulse).
- **To move into NUE**: the library is C++20 (`std::span`, defaulted `operator==`); NUE's root sets C++17, so the legacy targets need `CXX_STANDARD 20` or a C++17 port. `platform/types.hpp` forwards
  to the d3d9 translator's `nue/types.hpp` (Status, Result, Id, Bytes): legacy_win32 should get its own tiny status header instead. The platform interfaces (Clock, Threads, Storage, Config, System, Input)
  need NUE-backed implementations calling the `nue_*` C API; the std and null ones stay as test fixtures.
