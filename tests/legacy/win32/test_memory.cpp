// Memory: the arena address space and the heap manager, then the same operations through the real VirtualAlloc / HeapAlloc with outcomes compared.
#include "nue_legacy/win32/arena_memory.hpp"
#include "nue_legacy/win32/memory.hpp"
#include "test_support.hpp"
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace nue::legacy::win32;

static void test_memory() {
    ArenaMemory guest_memory(96u << 20, 64u << 20);
    auto space = make_arena_address_space(guest_memory, 64u << 20);
    uint32_t base = 0;
    CHECK_EQ(space->virtual_alloc(0, 0, mem::commit, page::readwrite, base), uint32_t(err::invalid_parameter));           // zero size
    CHECK_EQ(space->virtual_alloc(0, 4096, 0, page::readwrite, base), uint32_t(err::invalid_parameter));                   // neither reserve nor commit
    CHECK_EQ(space->virtual_alloc(0, 4096, mem::commit, 0x3, base), uint32_t(err::invalid_parameter));                     // not a protection
    CHECK_EQ(space->virtual_alloc(0, 3 * 65536, mem::reserve, page::noaccess, base), 0u);
    CHECK(base != 0 && base % 65536 == 0);
    CHECK(space->host(base, 16) == nullptr);                                                                               // reserved, not committed
    uint32_t committed = 0;
    CHECK_EQ(space->virtual_alloc(base + 4096, 8192, mem::commit, page::readwrite, committed), 0u);
    CHECK_EQ(committed, base + 4096);
    std::byte* p = space->host(base + 4096, 8192);
    CHECK(p != nullptr);
    std::memset(p, 0x5A, 8192);
    CHECK(space->host(base, 16) == nullptr && space->host(base + 4096, 8193) == nullptr);
    RegionInfo r;
    CHECK_EQ(space->virtual_query(base + 100, r), 0u);
    CHECK(r.state == mem::reserve && r.base_address == base && r.allocation_base == base && r.region_size == 4096 && r.allocation_protect == page::noaccess);
    CHECK_EQ(space->virtual_query(base + 4096, r), 0u);
    CHECK(r.state == mem::commit && r.protect == page::readwrite && r.region_size == 8192 && r.type == mem::private_);
    uint32_t old = 0;
    CHECK_EQ(space->virtual_protect(base + 4096, 4096, page::readonly, old), 0u);
    CHECK_EQ(old, uint32_t(page::readwrite));
    CHECK_EQ(space->virtual_protect(base, 4096, page::readonly, old), 487u);                                               // not committed
    CHECK(is_bad_write_ptr(*space, base + 4096, 16) && !is_bad_read_ptr(*space, base + 4096, 16) && is_bad_read_ptr(*space, base, 1));
    CHECK_EQ(space->virtual_free(base + 4096, 4096, mem::decommit), 0u);
    CHECK_EQ(space->virtual_alloc(base + 4096, 4096, mem::commit, page::readwrite, committed), 0u);
    CHECK(*reinterpret_cast<uint8_t*>(space->host(base + 4096, 1)) == 0);                                                  // recommitted pages are zero
    CHECK_EQ(space->virtual_free(base + 4096, 0, mem::release), 487u);                                       // not the base (ERROR_INVALID_ADDRESS)
    CHECK_EQ(space->virtual_free(base, 4096, mem::release), uint32_t(err::invalid_parameter));                             // size must be 0
    CHECK_EQ(space->virtual_free(base, 0, mem::release), 0u);
    CHECK_EQ(space->virtual_query(base, r), 0u);
    CHECK_EQ(r.state, uint32_t(mem::free_));
    CHECK_EQ(space->virtual_alloc(base, 4096, mem::reserve, page::noaccess, committed), 0u);                               // the same address can be asked for again
    CHECK_EQ(space->virtual_alloc(base, 4096, mem::reserve, page::noaccess, committed), 487u);                             // but not while it is reserved

    // ---- heaps
    HeapManager heaps(*space);
    const HeapId process = heaps.get_process_heap();
    auto a = heaps.heap_alloc(process, 0, 100);
    CHECK(a && a.value % 8 == 0);
    CHECK_EQ(heaps.heap_size(process, 0, a.value).value, 100u);
    std::memset(space->host(a.value, 100), 0x11, 100);
    auto grown = heaps.heap_realloc(process, heap_flag::zero_memory, a.value, 5000);
    CHECK(grown);
    CHECK_EQ(heaps.heap_size(process, 0, grown.value).value, 5000u);
    const auto* data = reinterpret_cast<const uint8_t*>(space->host(grown.value, 5000));
    CHECK(data[0] == 0x11 && data[99] == 0x11 && data[100] == 0 && data[4999] == 0);                                      // contents kept, the new part zeroed
    CHECK(heaps.heap_free(process, 0, grown.value).value);
    CHECK(!heaps.heap_free(process, 0, grown.value).value);                                                                // a second free is refused
    CHECK(heaps.heap_free(process, 0, 0).value);                                                                           // NULL is fine
    CHECK_EQ(heaps.heap_size(process, 0, 0x12345678).value, 0xFFFFFFFFu);
    auto zero = heaps.heap_alloc(process, 0, 0);
    CHECK(zero && heaps.heap_size(process, 0, zero.value).value == 0);
    // Reuse and zeroing of freed blocks.
    auto b1 = heaps.heap_alloc(process, 0, 64);
    std::memset(space->host(b1.value, 64), 0xEE, 64);
    heaps.heap_free(process, 0, b1.value);
    auto b2 = heaps.heap_alloc(process, heap_flag::zero_memory, 64);
    CHECK(b2.value == b1.value);                                                                                           // the block comes back
    CHECK(*reinterpret_cast<uint8_t*>(space->host(b2.value, 1)) == 0 && *reinterpret_cast<uint8_t*>(space->host(b2.value + 63, 1)) == 0);
    // Large blocks, a second heap with a ceiling, destroying heaps.
    auto big = heaps.heap_alloc(process, 0, 3u << 20);
    CHECK(big && heaps.heap_size(process, 0, big.value).value == (3u << 20));
    CHECK(heaps.heap_free(process, 0, big.value).value);
    auto limited = heaps.heap_create(0, 0, 8192);
    CHECK(limited);
    CHECK(heaps.heap_alloc(limited.value, 0, 100));
    CHECK_EQ(heaps.heap_alloc(limited.value, 0, 100000).error, uint32_t(err::not_enough_memory));
    CHECK(!heaps.heap_free(process, 0, heaps.heap_alloc(limited.value, 0, 8).value).value);                                // a block of another heap
    CHECK(heaps.heap_destroy(limited.value).value);
    CHECK(!heaps.heap_destroy(process).value);
    CHECK_EQ(heaps.heap_alloc(limited.value, 0, 8).error, uint32_t(err::invalid_handle));
    // GlobalAlloc.
    auto g = heaps.global_alloc(gmem::zeroinit, 256);
    CHECK(g && *reinterpret_cast<uint8_t*>(space->host(g.value, 1)) == 0);
    CHECK_EQ(heaps.global_free(g.value).value, 0u);
    // Concurrent allocation keeps the books straight.
    const uint64_t before = heaps.live_blocks(process);
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) workers.emplace_back([&] { for (int i = 0; i < 2000; ++i) { auto x = heaps.heap_alloc(process, 0, 16 + (i % 200)); heaps.heap_free(process, 0, x.value); } });
    for (auto& w : workers) w.join();
    CHECK_EQ(heaps.live_blocks(process), before);
}
REGISTER_TEST("memory", test_memory);

// The guest memory under the address space: allocation zone, window, host pointers, and a layout where no guest address is a host address.
static void test_arena_memory() {
    ArenaMemory memory(96u << 20, 64u << 20);
    CHECK(memory.host(0, 16) == nullptr && memory.host(0xFFFF, 1) == nullptr);                                  // the null guard
    CHECK(memory.host(ArenaMemory::kWindowBase, 64u << 20) != nullptr);                                         // the window the address space manages
    CHECK(memory.host(96u << 20, 1) == nullptr && memory.host(0xFFFFFFF0u, 32) == nullptr);                     // beyond the block
    const uint32_t a = memory.alloc(100), b = memory.alloc(1), c = memory.alloc(4096);
    CHECK(a && b && c && a % 16 == 0 && b % 16 == 0 && c % 16 == 0);
    CHECK(a >= ArenaMemory::kWindowBase + (64u << 20));                                                          // above the window
    CHECK(b >= a + 112 && c >= b + 16);
    uint8_t* pa = memory.host(a, 100);
    CHECK(pa != nullptr);
    bool zero = true;
    for (int i = 0; i < 100; ++i) zero &= pa[i] == 0;
    CHECK(zero);
    std::memset(pa, 0xAB, 100);
    CHECK_EQ(memory.address_of(pa + 7), a + 7);
    int foreign = 0;
    CHECK_EQ(memory.address_of(&foreign), 0u);
    memory.free(a);
    const uint32_t again = memory.alloc(100);
    CHECK_EQ(again, a);                                                                                          // the freed range is reused
    CHECK(memory.host(again, 100)[50] == 0);                                                                     // and handed out zeroed
    memory.free(again); memory.free(b); memory.free(c);                                                          // coalescing: one big block again
    const uint32_t whole = memory.alloc(30u << 20);
    CHECK(whole != 0);
    CHECK_EQ(memory.alloc(30u << 20), 0u);                                                                       // exhausted: fails, does not crash
    memory.free(whole);
    memory.free(0x1234);                                                                                         // a foreign address is ignored
    // The address space over its window: a window the memory does not map is refused.
    bool refused = false;
    try { auto bad = make_arena_address_space(memory, 100u << 20); (void)bad; } catch (const std::exception&) { refused = true; }
    CHECK(refused);
    // Page protections live in the software table: they do not change what the host can read, they change what the library reports and allows.
    auto space = make_arena_address_space(memory, 64u << 20);
    uint32_t base = 0, old = 0;
    CHECK_EQ(space->virtual_alloc(0, 16384, mem::reserve | mem::commit, page::readwrite, base), 0u);
    CHECK(!is_bad_write_ptr(*space, base, 16384) && !is_bad_read_ptr(*space, base, 16384));
    CHECK_EQ(space->virtual_protect(base + 4096, 4096, page::readonly, old), 0u);
    CHECK(is_bad_write_ptr(*space, base, 16384) && !is_bad_write_ptr(*space, base, 4096) && !is_bad_read_ptr(*space, base, 16384));
    CHECK_EQ(space->virtual_protect(base + 8192, 4096, page::noaccess, old), 0u);
    CHECK(is_bad_read_ptr(*space, base + 8192, 1) && !is_bad_read_ptr(*space, base + 12288, 4096));
    CHECK(reinterpret_cast<uint8_t*>(space->host(base, 4096)) == memory.host(base, 4096));                                                   // one mapping, two views of it
    CHECK(sizeof(void*) == 4 || reinterpret_cast<uintptr_t>(memory.host(base, 1)) != base);                      // on a 64-bit host the guest address is not the host address
}
REGISTER_TEST("arena_memory", test_arena_memory);

// VirtualApi: memory that becomes executable outside the main image is announced (register only), released regions are retired.
static void test_virtual_api() {
    ArenaMemory memory(96u << 20, 64u << 20);
    auto space = make_arena_address_space(memory, 64u << 20);
    std::vector<std::pair<uint32_t, uint32_t>> announced, released;
    VirtualApi api(*space, 0x400000, 0x300000, [&](uint32_t b, uint32_t s) { announced.push_back({b, s}); }, [&](uint32_t b, uint32_t s) { released.push_back({b, s}); });
    auto data = api.virtual_alloc(0, 8192, mem::reserve | mem::commit, page::readwrite);
    CHECK(data && data.value != 0 && announced.empty());                                                       // data is not announced
    auto code = api.virtual_alloc(0, 3 * 4096, mem::reserve | mem::commit, page::execute_readwrite);
    CHECK(code && announced.size() == 1 && announced[0].first == code.value && announced[0].second == 3 * 4096u);
    CHECK(api.in_executable_region(code.value + 100) && !api.in_executable_region(data.value));
    // Data that is made executable later is announced at the VirtualProtect (a decompressed module).
    uint32_t old = 0;
    CHECK(api.virtual_protect(data.value, 4096, page::execute_read, &old));
    CHECK_EQ(old, uint32_t(page::readwrite));
    CHECK(announced.size() == 2 && announced[1].first == data.value);
    // The same inside the main image is not announced (translated code covers it).
    auto in_image = api.virtual_alloc(0x400000, 4096, mem::reserve | mem::commit, page::execute_readwrite);
    CHECK(!in_image || announced.size() == 2);                                                                  // the window starts below the image, so this may be refused: never announced
    // Errors pass through.
    auto bad = api.virtual_protect(0x1000, 4096, page::readwrite, &old);
    CHECK(!bad && bad.error != 0);
    CHECK(!api.virtual_alloc(0, 0, mem::commit, page::readwrite));
    // Releasing the allocation retires its regions, including one made by VirtualProtect inside it.
    CHECK(api.virtual_free(code.value, 0, mem::release));
    CHECK(released.size() == 1 && released[0].first == code.value);
    CHECK(!api.in_executable_region(code.value));
    CHECK(api.virtual_free(data.value, 0, mem::release));
    CHECK_EQ(released.size(), 2u);
    CHECK(!api.virtual_free(data.value, 0, mem::release));                                                     // twice
    auto info = api.virtual_query(0x20000);
    CHECK(info);
}
REGISTER_TEST("virtual_api", test_virtual_api);

#if defined(_WIN32)
namespace {
// An operation script over a region: `base` is whatever the allocator returns, every address in the log is relative to it.
template <class Alloc, class Free, class Protect, class Query>
std::vector<std::string> virtual_script(Alloc alloc, Free free_, Protect protect, Query query) {
    std::vector<std::string> log;
    auto L = [&](const std::string& s) { log.push_back(s); };
    auto code = [](uint32_t e) { return std::to_string(e); };
    uint64_t base = 0;
    uint32_t e = alloc(0, 4 * 65536, mem::reserve, page::noaccess, base);
    L("reserve " + code(e));
    auto rel = [&](uint64_t a) { return a >= base && a < base + (1ull << 30) ? std::to_string(a - base) : std::string("outside"); };
    auto show = [&](uint64_t off) {
        RegionInfo r; uint64_t bs = 0, ab = 0;
        const uint32_t qe = query(base + off, r, bs, ab);
        L("query " + std::to_string(off) + " -> " + code(qe) + " base " + rel(bs) + " alloc " + (ab ? rel(ab) : "0") + " size " + (r.state == mem::free_ ? std::string("(free)") : std::to_string(r.region_size)) + " state " + std::to_string(r.state) + " protect " + std::to_string(r.protect) + " aprotect " + std::to_string(r.allocation_protect) + " type " + std::to_string(r.type));
    };
    show(0); show(70000);
    uint64_t out = 0;
    L("commit one page " + code(alloc(base + 4096, 4096, mem::commit, page::readwrite, out)) + " at " + rel(out));
    L("commit across " + code(alloc(base + 8192, 3 * 4096, mem::commit, page::readonly, out)) + " at " + rel(out));
    L("commit already committed " + code(alloc(base + 4096, 4096, mem::commit, page::execute_read, out)));
    show(0); show(4096); show(8192); show(20000); show(65536);
    L("commit past the reservation " + code(alloc(base + 3 * 65536, 2 * 65536, mem::commit, page::readwrite, out)));
    L("commit with a bad protection " + code(alloc(base, 4096, mem::commit, 3, out)));
    L("commit size zero " + code(alloc(base, 0, mem::commit, page::readwrite, out)));
    uint32_t old = 0;
    L("protect " + code(protect(base + 4096, 4096, page::readonly, old)) + " old " + std::to_string(old));
    L("protect uncommitted " + code(protect(base, 4096, page::readonly, old)));
    L("protect across committed and not " + code(protect(base + 4096, 8 * 4096, page::readwrite, old)));
    L("protect bad value " + code(protect(base + 4096, 4096, 0x77, old)));
    L("decommit " + code(free_(base + 4096, 4096, mem::decommit)));
    show(4096);
    L("decommit twice " + code(free_(base + 4096, 4096, mem::decommit)));
    L("decommit uncommitted reserved page " + code(free_(base, 4096, mem::decommit)));
    L("release with a size " + code(free_(base, 4096, mem::release)));
    L("release not at the base " + code(free_(base + 4096, 0, mem::release)));
    L("reserve at the same place " + code(alloc(base, 65536, mem::reserve, page::noaccess, out)));
    L("release " + code(free_(base, 0, mem::release)));
    show(0);
    L("release again " + code(free_(base, 0, mem::release)));
    // Reserve and commit in one go, then commit-only on free memory.
    L("reserve and commit " + code(alloc(0, 100000, mem::reserve | mem::commit, page::readwrite, base)));
    show(0); show(100000 - 1); show(4096 * 25);
    L("release " + code(free_(base, 0, mem::release)));
    return log;
}
}  // namespace

static void test_memory_against_windows() {
    ArenaMemory guest_memory(96u << 20, 64u << 20);
    auto space = make_arena_address_space(guest_memory, 64u << 20);
    // Real.
    const auto expected = virtual_script(
        [](uint64_t address, uint32_t size, uint32_t type, uint32_t protect, uint64_t& out) -> uint32_t {
            // The script is written for a library whose address 0 means "anywhere": real allocations come from the OS.
            static bool first = true;
            (void)first;
            SetLastError(0);
            void* p = VirtualAlloc(reinterpret_cast<void*>(address), size, type, protect);
            out = reinterpret_cast<uint64_t>(p);
            return p ? 0 : GetLastError();
        },
        [](uint64_t address, uint32_t size, uint32_t type) -> uint32_t { SetLastError(0); return VirtualFree(reinterpret_cast<void*>(address), size, type) ? 0 : GetLastError(); },
        [](uint64_t address, uint32_t size, uint32_t protect, uint32_t& old) -> uint32_t { DWORD o = 0; SetLastError(0); const BOOL ok = VirtualProtect(reinterpret_cast<void*>(address), size, protect, &o); old = o; return ok ? 0 : GetLastError(); },
        [](uint64_t address, RegionInfo& r, uint64_t& base, uint64_t& alloc_base) -> uint32_t {
            MEMORY_BASIC_INFORMATION m;
            SetLastError(0);
            if (!VirtualQuery(reinterpret_cast<void*>(address), &m, sizeof m)) return GetLastError();
            base = reinterpret_cast<uint64_t>(m.BaseAddress); alloc_base = reinterpret_cast<uint64_t>(m.AllocationBase);
            r.region_size = uint32_t(m.RegionSize); r.state = m.State; r.protect = m.Protect; r.allocation_protect = m.AllocationProtect; r.type = m.Type;
            return 0;
        });
    const auto actual = virtual_script(
        [&](uint64_t address, uint32_t size, uint32_t type, uint32_t protect, uint64_t& out) -> uint32_t { uint32_t r = 0; const uint32_t e = space->virtual_alloc(uint32_t(address), size, type, protect, r); out = r; return e; },
        [&](uint64_t address, uint32_t size, uint32_t type) -> uint32_t { return space->virtual_free(uint32_t(address), size, type); },
        [&](uint64_t address, uint32_t size, uint32_t protect, uint32_t& old) -> uint32_t { return space->virtual_protect(uint32_t(address), size, protect, old); },
        [&](uint64_t address, RegionInfo& r, uint64_t& base, uint64_t& alloc_base) -> uint32_t { const uint32_t e = space->virtual_query(uint32_t(address), r); base = r.base_address; alloc_base = r.allocation_base; return e; });
    CHECK_EQ(expected.size(), actual.size());
    int differences = 0;
    for (size_t i = 0; i < (std::min)(expected.size(), actual.size()); ++i)
        if (expected[i] != actual[i]) { ++differences; std::fprintf(stderr, "windows: %s\nmine:    %s\n", expected[i].c_str(), actual[i].c_str()); }
    std::printf("  %zu observations, %d differ\n", expected.size(), differences);
    CHECK_EQ(differences, 0);

    // Heaps: sizes, zeroing, realloc, refusal of foreign pointers, a heap with a ceiling.
    HANDLE real_heap = HeapCreate(0, 0, 0);
    HeapManager heaps(*space);
    const HeapId mine = heaps.heap_create(0, 0, 0).value;
    for (uint32_t size : {0u, 1u, 7u, 8u, 100u, 4096u, 70000u, 1000000u}) {
        void* p = HeapAlloc(real_heap, HEAP_ZERO_MEMORY, size);
        auto m = heaps.heap_alloc(mine, heap_flag::zero_memory, size);
        CHECK(p != nullptr && m);
        CHECK_EQ(HeapSize(real_heap, 0, p), heaps.heap_size(mine, 0, m.value).value);
        bool zero = true;
        const uint8_t* mp = reinterpret_cast<const uint8_t*>(space->host(m.value, std::max(size, 1u)));
        for (uint32_t i = 0; i < size; ++i) zero = zero && mp[i] == 0 && static_cast<uint8_t*>(p)[i] == 0;
        CHECK(zero);
        void* q = HeapReAlloc(real_heap, 0, p, size + 500);
        auto n = heaps.heap_realloc(mine, 0, m.value, size + 500);
        CHECK(q != nullptr && n);
        CHECK_EQ(HeapSize(real_heap, 0, q), heaps.heap_size(mine, 0, n.value).value);
        CHECK(HeapFree(real_heap, 0, q) != 0 && heaps.heap_free(mine, 0, n.value).value);
    }
    CHECK(HeapFree(real_heap, 0, nullptr) != 0 && heaps.heap_free(mine, 0, 0).value);
    SetLastError(0);
    CHECK_EQ(heaps.heap_size(mine, 0, 0x12345678).value, 0xFFFFFFFFu);
    HANDLE limited = HeapCreate(0, 0, 65536);
    HeapManager limited_manager(*space);
    const HeapId lm = limited_manager.heap_create(0, 0, 65536).value;
    CHECK(HeapAlloc(limited, 0, 1000) != nullptr && limited_manager.heap_alloc(lm, 0, 1000));
    SetLastError(0);
    void* too_big = HeapAlloc(limited, 0, 1000000);
    const DWORD limit_error = GetLastError();
    auto mine_big = limited_manager.heap_alloc(lm, 0, 1000000);
    CHECK(!too_big && !mine_big);
    CHECK_EQ(limit_error, mine_big.error);
    SetLastError(0);
    HeapDestroy(limited);
    HeapDestroy(real_heap);
}
REGISTER_TEST("memory_windows", test_memory_against_windows);
#endif
