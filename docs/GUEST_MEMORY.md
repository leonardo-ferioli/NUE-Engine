# Guest memory in NUE: one interface for every legacy layer (PROPOSAL)

Status: **proposal, nothing implemented in `include/nue`**. Written by the core session for the port integration session (2026-10-27). The user decides the points
marked **DECISION**. Context: the port's 64-bit model (`docs/MEMORY_MODEL_64.md` and `guest_memory.hpp` in the port repository, commit 7e357af) and
`docs/ABI.md` section 2.2 (native vs legacy ABI).

## 1. Reconciling the two documents

| Port (`guest::Memory`, `MEMORY_MODEL_64.md`) | NUE `docs/ABI.md` 2.2 | Reconciled |
|---|---|---|
| host = `base + guest`, `base` 4 GiB aligned, 4 GiB reserved `PROT_NONE`/`MEM_RESERVE` | `WINDOW64`: same formula; text says the window is reserved "with low-address hints (`MAP_32BIT`...)" | **The hint text is wrong and is corrected.** `base` can be anywhere 4 GiB aligned; nothing needs the arena below 4 GiB (`MAP_32BIT` only covers 2 GiB anyway). Only `IDENTITY32` needs low addresses. |
| `NFSU2_GUEST_IDENTITY` when pointers are 32 bits | `IDENTITY32` | same thing, one name each |
| no host pointer visible to the guest (biased handles, method tokens, import tokens) | handle tables, call tokens, `nue_legacy_resolve` | same rule |
| guest sees 4 KiB pages, protections in a software table | not stated | **added**: the native VM layer works in host pages; the 4 KiB guest page rules live in the legacy memory module |
| `INDIRECT` fallback (every access translated) | `INDIRECT` for hosts that cannot reserve a window | the port's "smaller arena with a mask" is a middle step: see 4 |
| `dsound_facade.hpp` `GuestMemory` {`ptr`, `alloc`, `free`}; platform-api `AddressSpace`/`ArenaAddressSpace` | `docs/LEGACY_DSOUND.md` "Guest facade seam" | **one interface**: `guest::Memory` (section 3), the three become aliases/adapters |

Rule that stays: **the native C ABI never sees a guest address.** Guest memory is a *legacy* concept (C++ interface in `include/nue_legacy`, owned by `legacy_core`).
What the native ABI offers is only a *generic* virtual-memory service (section 2), which the arena is built from and which any other module may use.

## 2. Native additive API: `nue/vm.h` (virtual address space service)

Additive (API version bump minor/patch, ABI guard tables regenerated); implemented in the core with `VirtualAlloc2`/`VirtualAlloc` and `mmap`, so no platform session is involved.
Nothing is hard-coded: sizes come from the caller.

```c
typedef struct nue_vm_info { uint32_t struct_size; uint32_t page_size;        /* host MMU page: 4096, 16384 (Apple silicon, some Android) */
    uint32_t granularity;                                                        /* Windows 65536; elsewhere = page_size */
    uint32_t address_bits; uint32_t flags;                                       /* NUE_VM_CAN_ALIGN, NUE_VM_CAN_EXEC (never on iOS) */
    uint64_t address_limit_bytes; /* 0 = unknown/unlimited (RLIMIT_AS, job object) */ uint32_t reserved[4]; } nue_vm_info;
typedef struct nue_vm_reserve_desc { uint32_t struct_size; uint32_t flags; uint64_t size; uint64_t alignment; /* power of two, 0 = granularity */
    const void* preferred; /* hint */ uint32_t reserved[4]; } nue_vm_reserve_desc;
typedef uint32_t nue_vm_region;                                                  /* generation-checked id */
NUE_API nue_result nue_vm_get_info(nue_vm_info*);
NUE_API nue_result nue_vm_reserve(const nue_vm_reserve_desc*, nue_vm_region* out, void** out_base);   /* no commit charge; NUE_ERR_OUT_OF_MEMORY when refused */
NUE_API nue_result nue_vm_commit(nue_vm_region, uint64_t offset, uint64_t size, uint32_t prot);       /* prot: NUE_VM_READ|WRITE|EXEC; EXEC may return UNSUPPORTED */
NUE_API nue_result nue_vm_protect(nue_vm_region, uint64_t offset, uint64_t size, uint32_t prot);
NUE_API nue_result nue_vm_decommit(nue_vm_region, uint64_t offset, uint64_t size);                    /* back to reserved, contents dropped */
NUE_API nue_result nue_vm_release(nue_vm_region);
```
Offsets/sizes must be multiples of `page_size`; the memory budget system accounts **committed** bytes only (`NUE_BUDGET_*`), a reservation is not charged.

## 3. Legacy interface: one `guest::Memory` for win32, dsound, dinput and d3d9 (`include/nue_legacy/guest_memory.hpp`)

```cpp
namespace nue::legacy { class GuestMemory {                       // the port's guest::Memory plus what the libraries asked for
public:
  virtual uint32_t alloc(uint32_t bytes, uint32_t align = 16) = 0;       // zeroed, 0 on failure
  virtual void     free(uint32_t address) = 0;
  virtual uint8_t* host(uint32_t address, uint32_t bytes) = 0;            // nullptr if any byte is not mapped (this is the dsound `ptr`)
  virtual uint32_t address_of(const void* host_pointer) = 0;              // 0 when foreign
  virtual bool     readable(uint32_t a, uint32_t n) = 0;                  // IsBadReadPtr family; software page table, 4 KiB
  virtual bool     writable(uint32_t a, uint32_t n) = 0;
  virtual uint32_t protect(uint32_t a, uint32_t n, uint32_t win32_prot) = 0;   // VirtualProtect on the 4 KiB table, returns old protection
  virtual nue_result commit(uint32_t a, uint32_t n, uint32_t win32_prot) = 0; // VirtualAlloc(MEM_COMMIT) inside the arena
  virtual nue_result decommit(uint32_t a, uint32_t n) = 0;
  virtual bool     identity() const = 0;                                   // IDENTITY32: host(a)==a
}; }
```
Implementations: `IdentityMemory` (32-bit host, wraps the process heap), `ArenaMemory` (WINDOW64, built on `nue_vm`, base kept in one global for the generated code),
`MaskedArenaMemory` (4). Libraries take a `GuestMemory&`; none casts an address. Hooks the Win32 library needs from the *host* (guest CPU side, also in `nue_legacy`):
`GuestCpu` { `call(entry, args, n)` run a guest function on a pooled context (callbacks, timers, thread entries), `new_context(stack_bytes)`, `teb(thread)` address,
`raise/unwind(seh)`, `set_fpu_state/get`, `halt_request()` }. They are implemented by the *port's* runtime (generated code) and passed in at `legacy_core` init; the NUE side
only defines the abstract class.

## 4. What the arena needs per host (measured where stated)

| Host | Reservation | Commit / protect | Remarks |
|---|---|---|---|
| Windows x64 | **measured on this machine**: `VirtualAlloc2` with `MEM_ADDRESS_REQUIREMENTS{Alignment=4 GiB}` gives an aligned 4 GiB range in 0.02 ms; the fallback (reserve 8 GiB, release, re-reserve at the aligned address) also works but can race; commit/protect/decommit at `base+0x400000` worked; a second arena and a 64 GiB reservation also succeeded | `VirtualAlloc(MEM_COMMIT)`, `VirtualProtect`; granularity 64 KiB, page 4 KiB | Windows 10 1803+ for `VirtualAlloc2`; older: the fallback |
| Linux x64/arm64 | `mmap(NULL, 8 GiB, PROT_NONE, MAP_PRIVATE\|MAP_ANONYMOUS\|MAP_NORESERVE)`, then `munmap` head/tail to align (no race) | `mprotect`, `madvise(MADV_DONTNEED)` for decommit | **not run here**. PROT_NONE is not charged by overcommit modes 0/1 and not in mode 2 until writable. `ulimit -v` (RLIMIT_AS) smaller than ~5 GiB makes it fail |
| Android arm64 | same as Linux | same; 16 KiB pages on newer devices | **not run**. Reservation should work in an app process (no AS limit in normal apps); to be verified on a device. Hardened/vendor kernels with `vm.max_map_count` are not a problem for one mapping |
| macOS x64/arm64 | `mmap` PROT_NONE (`MAP_NORESERVE` ignored, harmless); 16 KiB pages on arm64 | `mprotect` | **not run**. No JIT entitlement needed because the arena is data |
| iOS arm64 | `mmap` PROT_NONE of 8 GiB then trim. `__PAGEZERO` being 4 GiB does **not** matter for WINDOW64 (base is anywhere); it only kills IDENTITY | `mprotect`; **no PROT_EXEC for writable pages** | **not run, the main risk.** Reserved PROT_NONE memory is not counted by jetsam (committed/dirty memory is), but nobody here has verified that an app may hold a 4 GiB reservation on every iPhone model; test first on a device. Fallback `MaskedArenaMemory` (below). 16 KiB pages: software table for the guest's 4 KiB protections |
| any 32-bit host | no arena | IDENTITY32 only, the image must sit at 0x00400000 | armv7 is out of scope |

**Hosts where 4 GiB is a problem:** iOS (unverified), Linux/containers with `RLIMIT_AS`, very old Windows (no `VirtualAlloc2`: fallback, small race window). Mitigation already in the port's
plan: `MaskedArenaMemory` reserves `arena_size` < 4 GiB (the game budget is 2 GiB committed; its image, stack and heaps are placed by our allocator, so every guest address we hand out
fits); guest addresses above `arena_size` are simply never handed out and are not mapped. The generated code does not change (`base + a`); only `alloc` and `host` know the size.
**DECISION 1:** make the arena size a configuration value from the start (default 4 GiB, minimum = image + stack + heap budget), not a constant.

## 5. Answers to the legacy sessions

* **nue-dsound** (`GuestMemory`, `HostServices`, call tokens): `ptr(addr,bytes)` becomes `host(addr,bytes)`; `alloc/free` identical; add `address_of` (needed to give DirectSound `Lock` pointers
  back as guest addresses: the mirror buffer is allocated with `alloc`, so the guest sees a guest address and the mixer a host pointer). `HostServices::set_event` is a Win32-library service
  (`legacy_win32` event table); it runs on the mix thread, so the event table must be thread safe (it is, `sync.hpp`). Call tokens: owned by `legacy_core`, same token ranges as the port
  (`0xf1000000+` methods, `0xf0000000+` imports): **DECISION 2:** freeze these ranges in `docs/ABI.md` 2.3/2.5 so that the three facades agree.
* **platform-api** (moving `legacy_win32`/`dinput` into `src/legacy/win32`): take `ArenaAddressSpace` as the implementation of `GuestMemory::commit/decommit/protect/readable/writable`
  on top of `nue_vm`; its Win32 region rules stay in `legacy_win32` (not in core). Threads, `timeSetEvent`, SEH, module and exit stay behind the abstract `GuestCpu` (section 3).
  Native services it should keep using: `nue_thread_*`, `nue_timer_*` (virtual clock in tests), `nue_file_*` (32-bit ids), `nue_config_*` for the registry, `nue_file_map` (real mapping now).
* **d3d9**: `LockedRegion` memory for a Lock on a 64-bit host is `GuestMemory::alloc`ed (guest address returned to the game, host pointer to the layer) and mirrored on Unlock; this is the
  same call the dsound ring uses, so the 4 GiB arena is the only place guest-visible buffers live.
* **ABI guard:** `include/nue_legacy/*.hpp` is C++ and is not read by `tools/abi`. Its stability rule: `GuestMemory` and `GuestCpu` get a `NUE_LEGACY_MEMORY_ABI` integer that changes
  when a virtual function is added or reordered (append-only, like the vtable orders in 2.6).

## 6. Order of work (needs the user's go)

1. **DECISION 3:** approve `nue/vm.h` (additive, small, core implements it in about a day with Windows x64 and POSIX tests; arena reservation tested on Windows, POSIX compile-checked).
2. Add `include/nue_legacy/guest_memory.hpp` + `IdentityMemory`/`ArenaMemory` with tests (Windows x64 and x86 here; Linux/macOS/Android/iOS by their sessions or CI).
3. Platform sessions run the reservation test on their hosts (Linux CI, Android emulator/device, iOS device): `nue_test_vm` reserves 4 GiB aligned, commits, protects, releases.
4. Correct `docs/ABI.md` 2.2 text (done with this proposal) and freeze the token ranges.
