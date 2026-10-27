// Virtual address space service (nue/vm.h): reserve / commit / protect / decommit / release, alignment, bookkeeping, error cases, threads.
#include "nue_test.h"
#include "nue/nue.h"
#include "nue/vm.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace {

nue_vm_info info() {
    nue_vm_info i;
    NUE_STRUCT_INIT(&i);
    nue_vm_get_info(&i);
    return i;
}

nue_vm_reserve_desc desc(uint64_t size, uint64_t alignment = 0) {
    nue_vm_reserve_desc d;
    NUE_STRUCT_INIT(&d);
    d.size = size;
    d.alignment = alignment;
    return d;
}

uint64_t committed_of(nue_vm_region r) {
    nue_vm_region_info ri;
    NUE_STRUCT_INIT(&ri);
    nue_vm_region_get_info(r, &ri);
    return ri.committed_bytes;
}

#ifdef _WIN32
// Windows reports the real protection of a page: lets the test see that protect() reached the OS
DWORD os_protection(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
    return mbi.State == MEM_COMMIT ? mbi.Protect : (mbi.State == MEM_RESERVE ? 0xFFFFu : 0);
}
#endif

TEST_CASE(info_is_sane) {
    nue_vm_info i = info();
    CHECK(i.page_size >= 4096 && (i.page_size & (i.page_size - 1)) == 0);
    CHECK(i.granularity >= i.page_size && (i.granularity & (i.granularity - 1)) == 0);
    CHECK(i.flags & NUE_VM_CAN_ALIGN);
    CHECK(i.address_bits >= 31);
    CHECK_EQ(nue_vm_get_info(nullptr), NUE_ERR_INVALID_ARGUMENT);
}

TEST_CASE(reserve_commit_protect_decommit_release) {
    const nue_vm_info i = info();
    const uint64_t page = i.page_size, align = 16ull << 20;
    nue_vm_region r = 0;
    void* base = nullptr;
    nue_vm_reserve_desc d = desc(64ull << 20, align);
    CHECK_EQ(nue_vm_reserve(&d, &r, &base), NUE_OK);
    CHECK(r != 0 && base != nullptr);
    CHECK_EQ(reinterpret_cast<uintptr_t>(base) % align, 0u);
    nue_vm_region_info ri;
    NUE_STRUCT_INIT(&ri);
    CHECK_EQ(nue_vm_region_get_info(r, &ri), NUE_OK);
    CHECK(ri.base == base);
    CHECK_EQ(ri.size, 64ull << 20);
    CHECK_EQ(ri.committed_bytes, 0u);
#ifdef _WIN32
    CHECK_EQ(os_protection(base), 0xFFFFu);                 // reserved only
#endif

    uint8_t* p = static_cast<uint8_t*>(base);
    CHECK_EQ(nue_vm_commit(r, page, 3 * page, NUE_VM_READ | NUE_VM_WRITE), NUE_OK);
    CHECK_EQ(committed_of(r), 3 * page);
    std::memset(p + page, 0xAB, size_t(3 * page));
    CHECK(p[page] == 0xAB && p[4 * page - 1] == 0xAB);
    // committing again keeps the contents and does not double count
    CHECK_EQ(nue_vm_commit(r, 2 * page, 3 * page, NUE_VM_READ | NUE_VM_WRITE), NUE_OK);
    CHECK_EQ(committed_of(r), 4 * page);
    CHECK(p[3 * page] == 0xAB);
    CHECK(p[4 * page] == 0);                                // newly committed: zero filled

    CHECK_EQ(nue_vm_protect(r, page, page, NUE_VM_READ), NUE_OK);
#ifdef _WIN32
    CHECK_EQ(os_protection(p + page), DWORD(PAGE_READONLY));
    CHECK_EQ(os_protection(p + 2 * page), DWORD(PAGE_READWRITE));
#endif
    CHECK(p[page] == 0xAB);                                 // still readable
    CHECK_EQ(nue_vm_protect(r, page, page, NUE_VM_READ | NUE_VM_WRITE), NUE_OK);

    // decommit in the middle: bookkeeping splits, contents are dropped on recommit
    CHECK_EQ(nue_vm_decommit(r, 2 * page, page), NUE_OK);
    CHECK_EQ(committed_of(r), 3 * page);
#ifdef _WIN32
    CHECK_EQ(os_protection(p + 2 * page), 0xFFFFu);
#endif
    CHECK_EQ(nue_vm_protect(r, page, 3 * page, NUE_VM_READ), NUE_ERR_INVALID_ARGUMENT);   // the hole is not committed
    CHECK_EQ(nue_vm_commit(r, 2 * page, page, NUE_VM_READ | NUE_VM_WRITE), NUE_OK);
    CHECK(p[2 * page] == 0);
    CHECK(p[page] == 0xAB && p[3 * page] == 0xAB);          // neighbours untouched
    CHECK_EQ(committed_of(r), 4 * page);
    CHECK_EQ(nue_vm_decommit(r, 0, 64ull << 20), NUE_OK);   // whole region, mostly not committed: fine
    CHECK_EQ(committed_of(r), 0u);

    CHECK_EQ(nue_vm_release(r), NUE_OK);
    CHECK_EQ(nue_vm_release(r), NUE_ERR_INVALID_HANDLE);    // stale id
    CHECK_EQ(nue_vm_commit(r, 0, page, NUE_VM_READ), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_vm_region_get_info(r, &ri), NUE_ERR_INVALID_HANDLE);
}

TEST_CASE(range_bookkeeping_merges_and_splits) {
    const uint64_t page = info().page_size;
    nue_vm_region r = 0;
    nue_vm_reserve_desc d = desc(1ull << 20);
    CHECK_EQ(nue_vm_reserve(&d, &r, nullptr), NUE_OK);
    const uint32_t rw = NUE_VM_READ | NUE_VM_WRITE;
    CHECK_EQ(nue_vm_commit(r, 0, page, rw), NUE_OK);
    CHECK_EQ(nue_vm_commit(r, 4 * page, page, rw), NUE_OK);
    CHECK_EQ(committed_of(r), 2 * page);
    CHECK_EQ(nue_vm_commit(r, page, 3 * page, rw), NUE_OK);              // bridges the gap, adjacent on both sides
    CHECK_EQ(committed_of(r), 5 * page);
    CHECK_EQ(nue_vm_protect(r, 0, 5 * page, NUE_VM_READ), NUE_OK);       // one coalesced range
    CHECK_EQ(nue_vm_decommit(r, 2 * page, page), NUE_OK);
    CHECK_EQ(committed_of(r), 4 * page);
    CHECK_EQ(nue_vm_protect(r, 0, 2 * page, rw), NUE_OK);
    CHECK_EQ(nue_vm_protect(r, 3 * page, 2 * page, rw), NUE_OK);
    CHECK_EQ(nue_vm_protect(r, 0, 5 * page, rw), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_vm_decommit(r, 0, page), NUE_OK);
    CHECK_EQ(nue_vm_decommit(r, 4 * page, page), NUE_OK);
    CHECK_EQ(committed_of(r), 2 * page);
    nue_vm_info i = info();
    CHECK(i.region_count >= 1 && i.reserved_bytes >= (1ull << 20) && i.committed_bytes >= 2 * page);
    CHECK_EQ(nue_vm_release(r), NUE_OK);
}

TEST_CASE(argument_errors) {
    const nue_vm_info i = info();
    const uint64_t page = i.page_size;
    nue_vm_region r = 0;
    nue_vm_reserve_desc d = desc(0);
    CHECK_EQ(nue_vm_reserve(&d, &r, nullptr), NUE_ERR_INVALID_ARGUMENT);          // size 0
    d = desc(1 << 20, 3 * 65536);
    CHECK_EQ(nue_vm_reserve(&d, &r, nullptr), NUE_ERR_INVALID_ARGUMENT);          // alignment not a power of two
    CHECK_EQ(nue_vm_reserve(nullptr, &r, nullptr), NUE_ERR_INVALID_ARGUMENT);
    d = desc(1 << 20);
    d.struct_size = 4;
    CHECK_EQ(nue_vm_reserve(&d, &r, nullptr), NUE_ERR_INVALID_ARGUMENT);          // truncated struct
    d = desc(~0ull - 1000);
    CHECK(NUE_FAILED(nue_vm_reserve(&d, &r, nullptr)));                            // absurd size: refused, never a crash
    CHECK_EQ(r, 0u);

    d = desc(1 << 20);
    CHECK_EQ(nue_vm_reserve(&d, &r, nullptr), NUE_OK);
    const uint32_t rw = NUE_VM_READ | NUE_VM_WRITE;
    CHECK_EQ(nue_vm_commit(r, 1, page, rw), NUE_ERR_INVALID_ARGUMENT);             // offset not page aligned
    CHECK_EQ(nue_vm_commit(r, 0, page + 1, rw), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_vm_commit(r, 0, 0, rw), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(nue_vm_commit(r, 0, 2ull << 20, rw), NUE_ERR_INVALID_ARGUMENT);       // beyond the region
    CHECK_EQ(nue_vm_commit(r, 1ull << 20, page, rw), NUE_ERR_INVALID_ARGUMENT);    // starts at the end
    CHECK_EQ(nue_vm_commit(r, ~0ull - page + 1, 2 * page, rw), NUE_ERR_INVALID_ARGUMENT);   // offset + size wraps
    CHECK_EQ(nue_vm_commit(r, 0, page, 0x80), NUE_ERR_INVALID_ARGUMENT);           // unknown protection bit
    CHECK_EQ(nue_vm_protect(r, 0, page, rw), NUE_ERR_INVALID_ARGUMENT);            // reserved only
    CHECK_EQ(nue_vm_commit(0, 0, page, rw), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_vm_commit(0xDEAD0001u, 0, page, rw), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(committed_of(r), 0u);
    if (!(i.flags & NUE_VM_CAN_EXEC)) CHECK_EQ(nue_vm_commit(r, 0, page, NUE_VM_READ | NUE_VM_EXEC), NUE_ERR_UNSUPPORTED);
    else {
        CHECK_EQ(nue_vm_commit(r, 0, page, NUE_VM_READ | NUE_VM_EXEC), NUE_OK);
        CHECK_EQ(nue_vm_protect(r, 0, page, rw), NUE_OK);
    }
    CHECK_EQ(nue_vm_release(r), NUE_OK);
}

TEST_CASE(placement_hint_and_exact) {
    nue_vm_region a = 0, b = 0;
    void* base_a = nullptr;
    nue_vm_reserve_desc d = desc(4ull << 20);
    CHECK_EQ(nue_vm_reserve(&d, &a, &base_a), NUE_OK);
    d.preferred = base_a;
    d.flags = NUE_VM_RESERVE_EXACT;
    CHECK(NUE_FAILED(nue_vm_reserve(&d, &b, nullptr)));          // occupied: an exact request fails
    CHECK_EQ(b, 0u);
    d.flags = 0;
    void* base_b = nullptr;
    CHECK_EQ(nue_vm_reserve(&d, &b, &base_b), NUE_OK);          // a hint only: placed elsewhere
    CHECK(base_b != base_a);
    CHECK_EQ(nue_vm_release(b), NUE_OK);
    CHECK_EQ(nue_vm_release(a), NUE_OK);
    // the freed range can normally be taken again exactly; another thread of the process could race for it, so only report
    d.flags = NUE_VM_RESERVE_EXACT;
    if (NUE_SUCCEEDED(nue_vm_reserve(&d, &a, &base_b))) {
        CHECK(base_b == base_a);
        CHECK_EQ(nue_vm_release(a), NUE_OK);
    }
}

TEST_CASE(arena_of_four_gib_on_64_bit_hosts) {
    nue_vm_region r = 0;
    void* base = nullptr;
    const uint64_t four_gib = 4ull << 30;
    nue_vm_reserve_desc d = desc(four_gib, four_gib);
    nue_result res = nue_vm_reserve(&d, &r, &base);
    if (sizeof(void*) == 4) {
        CHECK_EQ(res, NUE_ERR_INVALID_ARGUMENT);                // a 32-bit process cannot even name the size
        return;
    }
    if (NUE_FAILED(res)) {
        // address space limited (container, ulimit -v): reported honestly, nothing else to check
        std::printf("  4 GiB arena refused here (%s): %s\n", nue_result_name(res), nue_get_last_error_message());
        CHECK(res == NUE_ERR_OUT_OF_MEMORY);
        return;
    }
    CHECK_EQ(reinterpret_cast<uintptr_t>(base) % four_gib, 0u);
    const uint64_t page = info().page_size;
    // the guest image address of the 32-bit games: base + 0x400000; the stack/heap region near the top
    CHECK_EQ(nue_vm_commit(r, 0x400000, 4 * page, NUE_VM_READ | NUE_VM_WRITE), NUE_OK);
    *reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(base) + 0x400000) = 0x12345678u;
    CHECK_EQ(*reinterpret_cast<volatile uint32_t*>(static_cast<uint8_t*>(base) + 0x400000), 0x12345678u);
    CHECK_EQ(nue_vm_commit(r, four_gib - page, page, NUE_VM_READ | NUE_VM_WRITE), NUE_OK);   // last page of the arena
    CHECK_EQ(nue_vm_commit(r, four_gib, page, NUE_VM_READ | NUE_VM_WRITE), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(committed_of(r), 5 * page);
    // a second arena at the same time (two guests in one process, tests)
    nue_vm_region r2 = 0;
    void* base2 = nullptr;
    if (NUE_SUCCEEDED(nue_vm_reserve(&d, &r2, &base2))) {
        CHECK(base2 != base);
        CHECK_EQ(reinterpret_cast<uintptr_t>(base2) % four_gib, 0u);
        CHECK_EQ(nue_vm_release(r2), NUE_OK);
    }
    CHECK_EQ(nue_vm_release(r), NUE_OK);
}

TEST_CASE(used_from_several_threads) {
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&failures] {
            const uint64_t page = info().page_size;
            for (int n = 0; n < 40; ++n) {
                nue_vm_region r = 0;
                void* base = nullptr;
                nue_vm_reserve_desc d = desc(1ull << 20, 1ull << 20);
                if (NUE_FAILED(nue_vm_reserve(&d, &r, &base))) { ++failures; continue; }
                if (NUE_FAILED(nue_vm_commit(r, 0, 2 * page, NUE_VM_READ | NUE_VM_WRITE))) ++failures;
                static_cast<uint8_t*>(base)[0] = 1;
                if (NUE_FAILED(nue_vm_decommit(r, 0, page))) ++failures;
                if (NUE_FAILED(nue_vm_release(r))) ++failures;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(failures.load(), 0);
}

TEST_CASE(works_without_the_engine_and_ids_are_not_reused_blindly) {
    // no nue_init in this test binary: the service is independent of the engine lifetime
    nue_vm_region first = 0, second = 0;
    nue_vm_reserve_desc d = desc(1 << 20);
    CHECK_EQ(nue_vm_reserve(&d, &first, nullptr), NUE_OK);
    CHECK_EQ(nue_vm_release(first), NUE_OK);
    CHECK_EQ(nue_vm_reserve(&d, &second, nullptr), NUE_OK);
    CHECK(second != first);                                  // same slot, new generation
    CHECK_EQ(nue_vm_release(first), NUE_ERR_INVALID_HANDLE);
    CHECK_EQ(nue_vm_release(second), NUE_OK);
}

}  // namespace

TEST_MAIN_BEGIN
    RUN(info_is_sane);
    RUN(reserve_commit_protect_decommit_release);
    RUN(range_bookkeeping_merges_and_splits);
    RUN(argument_errors);
    RUN(placement_hint_and_exact);
    RUN(arena_of_four_gib_on_64_bit_hosts);
    RUN(used_from_several_threads);
    RUN(works_without_the_engine_and_ids_are_not_reused_blindly);
TEST_MAIN_END
