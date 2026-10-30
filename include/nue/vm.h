/*
 * NUE engine - virtual address space service: reserve a large range of address space, commit and protect pages inside it, give pages back.
 *
 * What it is for. Anything that needs address space it controls: the 4 GiB arena a 32-bit guest lives in (docs/GUEST_MEMORY.md), a gigantic
 * sparse table, a pool whose pointers must never move. It is a thin, portable layer over VirtualAlloc2 / VirtualAlloc (Windows) and mmap (Linux,
 * Android, macOS, iOS). It knows nothing about guests: the native ABI never sees a guest address.
 *
 * Model
 *   reserve   claims address space only (no memory is charged, nothing can be touched). Optional alignment (power of two, 4 GiB for an arena)
 *             and a placement hint. The region id is 32-bit and generation checked (0 invalid).
 *   commit    makes pages inside a region usable (zero filled when first committed, contents kept when committed again) with a protection.
 *   protect   changes the protection of COMMITTED pages; reserved-only pages are rejected on every OS (NUE_ERR_INVALID_ARGUMENT).
 *   decommit  gives the pages back to the OS (contents dropped, address space kept reserved).
 *   release   gives the whole region back. Regions are owned by the caller and are NOT released by nue_shutdown.
 *
 * Units. Offsets and sizes of commit / protect / decommit are relative to the region base and must be multiples of the HOST page size
 * (nue_vm_info.page_size: 4096 on x86 and most ARM64, 16384 on Apple silicon and some Android devices). A guest with 4 KiB pages on a 16 KiB host
 * keeps its own page table and only commits host pages (see docs/GUEST_MEMORY.md). The region size is rounded up to the allocation granularity.
 *
 * Protections: NUE_VM_READ | NUE_VM_WRITE | NUE_VM_EXEC (0 = no access). EXEC is refused with NUE_ERR_UNSUPPORTED where the platform gives an
 * application no executable memory it can write (iOS); NUE_VM_CAN_EXEC in nue_vm_info says whether it can work.
 *
 * Memory accounting: a reservation is free; committed bytes are reported (nue_vm_info.committed_bytes, per region in nue_vm_region_info) and are the
 * caller's to budget (include/nue/memory.h). Nothing here has a built-in size limit; the OS refuses what does not fit (NUE_ERR_OUT_OF_MEMORY).
 *
 * Thread: [any]. Works before nue_init and after nue_shutdown.
 */
#ifndef NUE_VM_H
#define NUE_VM_H

#include "nue/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t nue_vm_region;    /* 0 invalid; slot | generation << 16 */

enum {
    NUE_VM_READ  = 1u << 0,
    NUE_VM_WRITE = 1u << 1,
    NUE_VM_EXEC  = 1u << 2
};

enum {
    NUE_VM_CAN_ALIGN = 1u << 0,    /* alignments above the allocation granularity are supported (always set) */
    NUE_VM_CAN_EXEC  = 1u << 1     /* NUE_VM_EXEC can be requested (clear on iOS) */
};

typedef struct nue_vm_info {
    uint32_t struct_size;
    uint32_t page_size;            /* host MMU page */
    uint32_t granularity;          /* reservation granularity: 65536 on Windows, the page size elsewhere */
    uint32_t address_bits;         /* usable user address bits of this process (47 on 64-bit desktop/mobile; at least) */
    uint32_t flags;                /* NUE_VM_CAN_* */
    uint32_t region_count;         /* live regions */
    uint64_t address_limit_bytes;  /* RLIMIT_AS style cap when the OS reports one, else 0 (unknown or none) */
    uint64_t reserved_bytes;       /* sum of the live regions' sizes */
    uint64_t committed_bytes;      /* sum of the committed bytes of the live regions */
    uint32_t reserved[4];
} nue_vm_info;

enum {
    NUE_VM_RESERVE_EXACT = 1u << 0 /* fail unless the range is placed exactly at `preferred` (otherwise it is only a hint) */
};

typedef struct nue_vm_reserve_desc {
    uint32_t struct_size;
    uint32_t flags;                /* NUE_VM_RESERVE_* */
    uint64_t size;                 /* bytes; rounded up to the granularity */
    uint64_t alignment;            /* power of two (0 = granularity); the base address is a multiple of it */
    const void* preferred;         /* placement hint, may be NULL */
    uint32_t reserved[4];
} nue_vm_reserve_desc;

typedef struct nue_vm_region_info {
    uint32_t struct_size;
    uint32_t reserved0;
    void* base;
    uint64_t size;
    uint64_t committed_bytes;
    uint32_t reserved[4];
} nue_vm_region_info;

NUE_API nue_result NUE_CALL nue_vm_get_info(nue_vm_info* out);                                   /* [any] */

/* Reserve address space. NUE_ERR_OUT_OF_MEMORY when the OS refuses (address space limit, no aligned hole), NUE_ERR_INVALID_ARGUMENT for a size of 0,
 * an alignment that is not a power of two, or a size that does not fit the address space of this process (32-bit hosts). out_base may be NULL. */
NUE_API nue_result NUE_CALL nue_vm_reserve(const nue_vm_reserve_desc* desc, nue_vm_region* out_region, void** out_base);
NUE_API nue_result NUE_CALL nue_vm_region_get_info(nue_vm_region region, nue_vm_region_info* out);

NUE_API nue_result NUE_CALL nue_vm_commit(nue_vm_region region, uint64_t offset, uint64_t size, uint32_t protection);
NUE_API nue_result NUE_CALL nue_vm_protect(nue_vm_region region, uint64_t offset, uint64_t size, uint32_t protection);   /* committed pages only */
NUE_API nue_result NUE_CALL nue_vm_decommit(nue_vm_region region, uint64_t offset, uint64_t size);
NUE_API nue_result NUE_CALL nue_vm_release(nue_vm_region region);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_VM_H */
