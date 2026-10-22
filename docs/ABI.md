# The two ABIs of the NUE engine

NUE exposes two ABIs. They have different audiences, different stability rules and live in different libraries. The legacy ABI is built **on top of**
the native ABI, never the other way round, so a native game never links (or pays for) any legacy code.

```
 native game / tool                       recompiled Windows-era game (32-bit guest)
        |                                            |
        |                                  +---------v-----------------------------+
        |                                  | LEGACY ABI   libnue_legacy              |
        |                                  |  legacy_win32   kernel32/user32/gdi32/  |
        |                                  |                 advapi32/winmm subset   |
        |                                  |  legacy_d3d9    IDirect3D9* (nue_d3d9)  |
        |                                  |  legacy_dsound  IDirectSound*           |
        |                                  |  legacy_dinput  IDirectInput8*          |
        |                                  |  legacy_ddraw   stubs                   |
        |                                  |  legacy_core    guest memory, handle    |
        |                                  |                 tables, call tokens,    |
        |                                  |                 callback trampolines    |
        |                                  +---------+-------------------------------+
 +------v-------------------------------------------v---------+
 | NATIVE ABI   libnue      (include/nue/*.h, nuepp C++ wrapper) |
 +---------------------------------------------------------------+
```

## 1. Native ABI

The engine's own modern C ABI (`include/nue/*.h`, prefix `nue_`, enums/macros `NUE_`).

**Rules** (already enforced by the headers, see `API.md` section 1): C99, `cdecl` everywhere (`NUE_CALL`), opaque handles or 32-bit ids, `struct_size` first
in every struct that crosses the boundary, fixed-width types, `uint64_t` for handle-like values, explicit `nue_result` codes, no exceptions across the
boundary, thread rules documented per function, identical API for 32- and 64-bit builds, static or shared linking. No Windows (or any OS) types appear in
public headers. Guest pointers never appear in the native ABI.

**Stability promise** (semantic versioning of `NUE_API_VERSION`):

| Change | Version bump | Allowed |
|---|---|---|
| bug fix, behaviour that was documented stays | patch | always |
| new function, new enum value, new struct field appended (guarded by `struct_size`), new event type, new platform-ops entry (guarded by `struct_size`) | minor | always; recorded in `API_CHANGES.md` |
| removing or changing a function/field/enum value, changing a struct layout, changing documented behaviour | major | only with a deprecation period of one minor version, announced to all consumers |

Before 1.0.0 the API is a draft: additive changes only, breaking changes only by explicit decision of the core session and a message to every consumer.
`nue_init` refuses a caller whose `api_version` major differs or whose minor is newer than the library's. Optional or OS-specific features are published
as **named extensions** (`nue_get_extension("nue.<name>.vN")`): a table whose first member is `struct_size`; a new revision gets a new name, so older
consumers keep working. Symbols are exported through an explicit list (`NUE_API`), with a linker version script on ELF platforms once 1.0 is cut.

## 2. Legacy ABI

The compatibility ABI that lets a **recompiled legacy game** (a Windows-era binary translated to C++) run without any system DLL: the calling
conventions, handle types and COM-style object models the translated code expects, implemented over the native ABI.

### 2.1 What the guest is

* The *guest* is the translated program: its code is generated C++, but its data model is the original one: **32-bit pointers, 32-bit handles**, stdcall /
  thiscall / cdecl calling conventions on a guest stack, a synthetic TEB with `LastError` at +0x34 and a SEH chain, x87/SSE state, the original PE sections.
* The *host* is the process running the engine. Today (Windows x86 launcher) host and guest pointers are the same 32-bit values.
* Everything the guest calls outside its own code goes through the **import table**; the legacy ABI is what answers those calls.

### 2.2 Guest memory model (where pointer translation happens)

Pointer translation exists in exactly one place: the **legacy boundary** (`legacy_core`). The native ABI never sees a guest pointer, and translated
code never sees a host pointer.

| Model | Used when | Guest address -> host address | Notes |
|---|---|---|---|
| `IDENTITY32` | 32-bit host (Windows x86 today) | `host = guest` | the fast backend reads/writes guest memory directly; guest image and heaps are real mappings at their addresses |
| `WINDOW64` | 64-bit host | `host = base + guest`, `base` = a reserved 4 GiB window | generated code adds the base (one register); Windows x64 / Linux / Android reserve the window with low-address hints (`MAP_32BIT`, `MAP_FIXED_NOREPLACE`, `VirtualAlloc` low range) |
| `INDIRECT` | hosts that cannot reserve a window (iOS arm64: nothing is mapped below 4 GiB, `__PAGEZERO` is 4 GiB; a reduced page zero via linker flags is unverified) | every access goes through a translation | slowest; correctness fallback, decided at translation time |

The model is a compile-time property of the generated code (`NUE_GUEST_MEM_MODEL`), not a run-time switch. **Open item (not implemented, not verified on
any device): the iOS and Android arm64 behaviour of `WINDOW64`.** No JIT: `dynamic_code` (run-time generated guest code) is interpreter-only on iOS.

### 2.3 Handle tables (guest handles are never host pointers)

Every object the guest holds is a 32-bit value that the legacy layer maps through a **generation-checked handle table**: `HANDLE`, `HWND`, `HDC`, `HKEY`,
`HMODULE`, thread/event/semaphore handles, file handles (these map 1:1 to `nue_file` ids, which are 32-bit by design), timer ids, DirectX objects. A stale or
foreign handle fails with the Win32 error the original API would give, never a host crash.

**COM-style objects.** A COM interface pointer in the guest is a guest address of a small object `{ vtable_ptr, token }` allocated in guest memory.
`vtable_ptr` points at an array of **call tokens**: guest addresses inside a reserved stub region. When translated code calls through the vtable, the
dispatcher recognises the token, pops/decodes the arguments from the guest stack (stdcall callee cleanup of N bytes; `thiscall` takes `this` in ECX),
translates guest pointers and handles, and invokes the host implementation. `AddRef/Release/QueryInterface` follow COM rules on the handle table entry.
Interface **IIDs and vtable layouts are versioned** per API (2.6).

### 2.4 Conventions the layer reproduces

| Topic | Rule |
|---|---|
| Calling conventions | the generated code already follows the guest conventions; the import dispatcher pops stdcall arguments (size known from the import descriptor) and leaves cdecl to the caller |
| Return values | EAX (EDX:EAX for 64-bit), ST0 for floating point; `HRESULT` 32-bit with the standard facility/severity layout |
| Errors | `GetLastError` is stored per guest thread in the synthetic TEB (+0x34) and is isolated from host `errno`/`GetLastError`; native `nue_result` values are translated by a documented table per API |
| Callbacks into the guest | window procedures, timer callbacks (`timeSetEvent`, `SetTimer`), thread entry points, enumeration callbacks (`DirectInput EnumDevices`), `DllMain`-style entries: run on a pooled guest CPU context (registers, stack, synthetic TEB) through trampolines; return addresses use reserved token ranges |
| Threads | each guest thread = a native thread (`nue_thread`) with an independent guest CPU context; guest globals are shared |
| Exceptions | guest SEH (RaiseException, RtlUnwind, UnhandledExceptionFilter) is a state machine in the dispatcher; host C++ exceptions never cross into guest frames |
| FPU / SSE control | x87 control word and MXCSR are mirrored to the host FPU on write and refreshed on read |
| Priorities | requests are capped by policy (e.g. `TIME_CRITICAL` -> above normal) through `nue_thread_set_priority`; recorded in the trace |

The existing implementation of these assumptions (guest CPU state, TEB, SEH state machine, native call/return, dispatcher) lives in the game repository
today (`native32.cpp`, `runtime.cpp`); migrating it into `legacy_core` is the legacy sessions' work.

### 2.5 How a recompiled game binds to the legacy ABI

1. At translation time the import table is emitted as data: `{ module, name or ordinal, descriptor }` (descriptor: argument bytes, convention, flags).
2. At startup the game calls `nue_legacy_resolve(module, name, api_version)` for each import and stores the returned **function descriptor** (host function
   pointer + argument size + convention + flags) in its import slot table. **No system DLL is ever loaded and no real `kernel32`/`d3d9` symbol is reached.**
3. An import that is not implemented resolves to a **trap stub** that logs the module/function and fails explicitly (never a silent no-op).
4. The resolver also reports, per API, which version table applies (2.6), so a game can refuse to start on an incomplete layer.

### 2.6 Versioning of the legacy ABI

There is no single legacy version number. Each emulated API has its own **version table** (`docs/legacy/<api>.tbl`, one line per function/method):

```
d3d9.dll   Direct3DCreate9                         since 1  status implemented
d3d9.dll   IDirect3DDevice9::DrawIndexedPrimitive  since 1  status implemented
kernel32   CreateEventA                            since 1  status implemented
kernel32   CreateWaitableTimerA                    since -  status stub (returns ERROR_CALL_NOT_IMPLEMENTED)
```

`status` is `implemented`, `partial` (documented deviations), `stub` or `trap`. Adding functions bumps the table revision; changing the behaviour of an
implemented entry requires a note in the table. Vtable orders and IIDs follow the original SDK headers exactly and never change. The legacy library has
its own semantic version (`NUE_LEGACY_VERSION`) that bumps minor for new entries and major only if the guest-facing descriptor format changes.

### 2.7 What the legacy layer asks of the native ABI

| Legacy need | Native feature | State |
|---|---|---|
| `CreateFile/ReadFile/WriteFile/SetFilePointer/FindFirstFile`, case-insensitive `\` paths | `nue_file_*` with 32-bit ids, VFS roots, `nue_vfs_list` | available |
| threads, TLS, critical sections, events, semaphores, mutexes, `Sleep`, `WaitForSingleObject/Multiple` | `nue_thread_*`, `nue_mutex/cond/semaphore`, `nue_sleep_ns`; multi-object waits are built in the legacy layer on `nue_cond` | available (waits are legacy-side) |
| `GetTickCount`, `QueryPerformanceCounter`, `timeGetTime` | `nue_time_ns`, `nue_time_counter*` | available |
| `timeSetEvent` / `SetTimer` / waitable timers | `nue_timer_*` | available (0.1.1) |
| window class / message loop / `PeekMessage` / `GetCursorPos` / `ClipCursor` | window and input **events** and snapshots; the legacy layer synthesises `WM_*` messages from them; on Windows the real HWND and its procedure keep priority (attached window) | available |
| registry (`RegOpenKey/QueryValue/SetValue`) | `nue_config_*` store under a `registry.` prefix, persisted in the settings root | available |
| DirectSound (primary + secondary buffers, play/write cursor, `SetFrequency/Volume/Pan`, 3D buffers, notifications) | `nue_mixer` ring voices, static buffers, 3D voices, buses | mixer engine pending; **notification positions on ring voices still to be added to mixer.h** |
| DirectInput (device enumeration, action maps, polled state, force feedback) | key/mouse/gamepad snapshots and events, `nue_gamepad_rumble`, raw joystick API; action mapping is legacy-side | available (force-feedback effects beyond rumble: not planned) |
| Direct3D 9 | `nue_gfx` + bgfx; `nue_gfx_add_listener` for `Reset`/`TestCooperativeLevel`; `nue_gpu_probe` for caps | PREPARE mode available; MANAGED pending |
| textures: `D3DFMT_*` / FOURCC (DXT1-5, ...) | `nue_texture` formats (nue-texture session) plus `nue_gpu_caps` format families; the legacy layer maps `D3DFMT`/FOURCC <-> `nue_texture_format` and asks the texture module to transcode when the GPU cannot sample the format | texture module in progress |
| `GetSystemInfo`, `GlobalMemoryStatus`, `GetVersionEx` | `nue_system_get_info`, `nue_memory_get_info` (reports are shaped to what the guest expects) | available |
| `VirtualAlloc/Protect` for run-time generated code | guest memory model (2.2); executable pages exist only for the interpreter-free paths allowed by the platform | legacy-side |
| crash reporting | `nue_diag_*`, `nue_crash_desc.guest_pc` provider | available |

## 3. Package layout

```
include/nue/*.h              native C headers          (always installed)
include/nuepp/*.hpp          C++ wrapper over the native ABI
include/nue_legacy/*.h       legacy headers: resolver, descriptors, guest memory helpers   (installed with libnue_legacy)
lib/libnue.{a,so,dll}        native ABI                CMake target nue::nue, nue::entry
lib/libnue_legacy.{a,so,dll} legacy ABI                CMake target nue::legacy (depends on nue::nue)
   components (optional, same package): nue::legacy_win32, nue::legacy_d3d9, nue::legacy_dsound, nue::legacy_dinput
share/doc/nue/*.md           API.md, ABI.md, ARCHITECTURE.md, PORTING.md, STATUS.md
lib/cmake/nue/               find_package(nue [COMPONENTS legacy]) configuration and version files
```

Source layout in the repository: `include/nue`, `include/nuepp`, `include/nue_legacy`, `src/core`, `src/platform/*`, `src/texture`, `src/legacy/{core,win32,d3d9,dsound,dinput}`.
`NUE_BUILD_LEGACY=OFF` builds the engine without the legacy layer; native consumers use `find_package(nue)` and link `nue::nue` only.

## 4. What changes for the platform sessions

* Nothing in their platform-ops contract: the legacy layer sits **above** the native API. Platform layers must keep the native API honest because the
  legacy layer maps it one to one: audio streams/mixer (DirectSound), input events and snapshots (DirectInput), window events (Win32 messages), file ids
  (HANDLEs), timers/threads/sync, memory information (`GlobalMemoryStatus`).
* Windows keeps the real-window path: the game's own HWND and WndProc are attached, not replaced.
* GPU capability probing (`gpu_probe`) and memory information (`memory_info_ex`) feed both the texture module and the legacy D3D9 caps.
* Platforms that cannot map a low 4 GiB window (iOS) are flagged in `STATUS.md`; the guest memory model decision lives in the legacy layer, not in the platform layer.
