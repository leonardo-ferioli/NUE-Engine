// nue_legacy/guest_memory.hpp: ArenaMemory (64-bit hosts), IdentityMemory (32-bit hosts) and the GuestCpu contract compile-check.
#include "nue_test.h"
#include "nue/nue.h"
#include "nue_legacy/guest_cpu.hpp"
#include "nue_legacy/guest_memory.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

using namespace nue::legacy;

namespace {

std::unique_ptr<ArenaMemory> make_arena(ArenaConfig c = ArenaConfig()) {
    nue_result r = NUE_OK;
    auto a = ArenaMemory::create(c, &r);
    if (!a) std::printf("  arena not created here: %s (%s)\n", nue_result_name(r), nue_get_last_error_message());
    return a;
}

// the smallest arena that exercises everything: 256 MiB masked arena, allocator range 16 MiB .. 64 MiB
ArenaConfig small_config() {
    ArenaConfig c;
    c.arena_size = 256ull << 20;
    c.alloc_begin = 0x01000000u;
    c.alloc_end = 0x04000000u;
    return c;
}

uint32_t host_page() {
    nue_vm_info i;
    NUE_STRUCT_INIT(&i);
    nue_vm_get_info(&i);
    return i.page_size;
}

TEST_CASE(protection_helpers) {
    CHECK(protection_allows_read(kPageReadOnly) && !protection_allows_write(kPageReadOnly));
    CHECK(protection_allows_read(kPageReadWrite) && protection_allows_write(kPageReadWrite));
    CHECK(!protection_allows_read(kPageNoAccess) && !protection_allows_write(kPageNoAccess));
    CHECK(!protection_allows_read(kPageReadWrite | kPageGuard));
    CHECK(protection_allows_read(kPageExecuteRead) && !protection_allows_write(kPageExecuteRead));
    CHECK(protection_allows_write(kPageExecuteReadWrite) && protection_allows_write(kPageWriteCopy));
    CHECK(!protection_allows_read(0));
    CHECK(is_valid_protection(kPageReadWrite) && is_valid_protection(kPageReadWrite | kPageGuard));
    CHECK(!is_valid_protection(0) && !is_valid_protection(0x3) && !is_valid_protection(kPageNoAccess | kPageGuard));
}

TEST_CASE(arena_basics_alloc_free_host_address_of) {
    auto a = make_arena();
    if (!a) return;
    CHECK(!a->identity());
    CHECK_EQ(reinterpret_cast<uintptr_t>(a->base()) % (4ull << 30), 0u);
    CHECK(a->host(0, 1) == nullptr);                       // nothing is mapped yet, page 0 never
    CHECK(a->host(0x400000, 4) == nullptr);

    uint32_t p = a->alloc(100);
    CHECK(p >= 0x10000000u && p < 0xE0000000u);
    CHECK_EQ(p % 16, 0u);
    uint8_t* h = a->host(p, 100);
    CHECK(h != nullptr);
    CHECK(h == a->base() + p);
    CHECK(a->address_of(h) == p);
    CHECK(a->address_of(h + 50) == p + 50);
    int foreign = 0;
    CHECK_EQ(a->address_of(&foreign), 0u);
    for (int i = 0; i < 100; ++i) CHECK(h[i] == 0);        // zero filled
    std::memset(h, 0x5A, 100);
    CHECK(a->write_u32(p, 0xCAFEBABEu));
    uint32_t back = 0;
    CHECK(a->read_u32(p, &back) && back == 0xCAFEBABEu);
    CHECK(a->readable(p, 100) && a->writable(p, 100));
    CHECK_EQ(a->protection(p), uint32_t(kPageReadWrite));
    CHECK_EQ(a->protection(0), 0u);

    uint32_t q = a->alloc(0);                              // HeapAlloc(0) style: a valid minimal block
    CHECK(q != 0 && q != p);
    uint32_t r = a->alloc(1000, 4096);
    CHECK(r != 0 && r % 4096 == 0);
    uint32_t big = a->alloc(40000, 65536);
    CHECK(big != 0 && big % 65536 == 0);
    CHECK_EQ(a->alloc(100, 3), 0u);                        // alignment must be a power of two
    CHECK_EQ(a->alloc(100, 1u << 20), 0u);
    CHECK_EQ(a->alloc(0xFFFFFFF0u), 0u);                   // does not fit
    ArenaStats s = a->stats();
    CHECK_EQ(s.live_blocks, 4u);

    a->free(p);
    a->free(p);                                            // double free ignored
    a->free(0x12345670u);                                  // foreign address ignored
    uint32_t again = a->alloc(100);
    CHECK_EQ(again, p);                                    // first fit reuses the hole
    h = a->host(again, 100);
    for (int i = 0; i < 100; ++i) CHECK(h[i] == 0);        // zeroed again after reuse
    a->free(again); a->free(q); a->free(r); a->free(big);
    CHECK_EQ(a->stats().live_blocks, 0u);
    CHECK_EQ(a->stats().free_ranges, 1u);                  // everything coalesced back into one range
}

TEST_CASE(arena_exhaustion_and_coalescing) {
    ArenaConfig c = small_config();
    c.alloc_end = c.alloc_begin + (1u << 20);              // 1 MiB of allocator space
    auto a = make_arena(c);
    if (!a) return;
    std::vector<uint32_t> blocks;
    for (;;) {
        uint32_t b = a->alloc(60 * 1024);
        if (!b) break;
        blocks.push_back(b);
    }
    CHECK_EQ(blocks.size(), 17u);                          // 17 * 60 KiB fit in 1 MiB, the 18th does not
    CHECK_EQ(a->alloc(120 * 1024), 0u);
    // free two neighbours: a 120 KiB request now fits only because the holes were coalesced
    a->free(blocks[3]);
    a->free(blocks[4]);
    uint32_t joined = a->alloc(120 * 1024);
    CHECK(joined >= blocks[3] && joined < blocks[3] + 60 * 1024);
    for (size_t i = 0; i < blocks.size(); ++i) if (i != 3 && i != 4) a->free(blocks[i]);
    a->free(joined);
    CHECK_EQ(a->stats().free_ranges, 1u);
    uint32_t whole = a->alloc(1u << 20);
    CHECK(whole != 0);
}

TEST_CASE(arena_page_table_commit_protect_decommit) {
    auto a = make_arena(small_config());
    if (!a) return;
    const uint32_t hp = host_page();
    const uint32_t image = 0x00400000u;
    CHECK_EQ(a->commit(image, hp, kPageReadOnly), NUE_OK);
    CHECK(a->readable(image, hp) && !a->writable(image, 1));
    CHECK_EQ(a->protection(image), uint32_t(kPageReadOnly));
    CHECK(a->host(image, hp) != nullptr);
    CHECK(a->host(image + hp, 1) == nullptr);              // next page not committed
    CHECK(!a->readable(image, hp + 1));                    // spans into the hole
    CHECK_EQ(a->protect(image, 100, kPageReadWrite), uint32_t(kPageReadOnly));   // returns the old protection
    CHECK(a->writable(image, hp));
    CHECK_EQ(a->protect(image + hp, 16, kPageReadWrite), 0u);                    // not committed: fails
    CHECK_EQ(a->protect(image, 16, 0x3), 0u);                                    // invalid protection value
    // guard and no-access pages are mapped but not readable
    CHECK_EQ(a->protect(image, 16, kPageReadWrite | kPageGuard), uint32_t(kPageReadWrite));
    CHECK(!a->readable(image, 4) && a->host(image, 4) != nullptr);
    CHECK_EQ(a->protect(image, 16, kPageNoAccess), uint32_t(kPageReadWrite | kPageGuard));
    CHECK(!a->writable(image, 4));
    CHECK_EQ(a->protect(image, 16, kPageReadWrite), uint32_t(kPageNoAccess));

    // contents are dropped by decommit and recommit gives zeros (a whole host page so that the host really releases it)
    uint8_t* h = a->host(image, hp);
    std::memset(h, 0xAA, hp);
    CHECK_EQ(a->decommit(image, hp), NUE_OK);
    CHECK(a->host(image, 1) == nullptr);
    CHECK_EQ(a->protection(image), 0u);
    CHECK_EQ(a->commit(image, hp, kPageReadWrite), NUE_OK);
    h = a->host(image, hp);
    bool zero = true;
    for (uint32_t i = 0; i < hp; ++i) zero = zero && h[i] == 0;
    CHECK(zero);

    // 4 KiB granularity: committing 10 bytes at an unaligned address commits the guest page that holds them
    CHECK_EQ(a->commit(0x00800123u, 10, kPageReadWrite), NUE_OK);
    CHECK(a->readable(0x00800000u, 4096) && a->host(0x00800FFFu, 1) != nullptr);
    // a guest page that becomes committed inside an already committed host page is zero too (matters on 16 KiB hosts; trivially true on 4 KiB)
    a->host(0x00800000u, 1)[0] = 0x77;
    CHECK_EQ(a->decommit(0x00800000u, 4096), NUE_OK);
    CHECK_EQ(a->commit(0x00800000u, 4096, kPageReadWrite), NUE_OK);
    CHECK(a->host(0x00800000u, 1)[0] == 0);

    // limits
    CHECK_EQ(a->commit(0x0FFFF000u, 8192, kPageReadWrite), NUE_ERR_INVALID_ARGUMENT);   // beyond the 256 MiB arena
    CHECK_EQ(a->commit(0x1000, 0, kPageReadWrite), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(a->commit(0x1000, 16, 0x3), NUE_ERR_INVALID_ARGUMENT);
    CHECK_EQ(a->decommit(0x0FFFF000u, 8192), NUE_ERR_INVALID_ARGUMENT);
    CHECK(a->host(0x20000000u, 1) == nullptr);             // masked arena: addresses above its size are not mapped
    CHECK_EQ(a->alloc(16, 16) != 0, true);
}

TEST_CASE(arena_large_blocks_give_pages_back_and_reuse_is_zeroed) {
    auto a = make_arena(small_config());
    if (!a) return;
    const uint32_t size = 1u << 20;
    uint32_t b = a->alloc(size);
    CHECK(b != 0);
    uint8_t* h = a->host(b, size);
    CHECK(h != nullptr);
    std::memset(h, 0xEE, size);
    const uint64_t before = a->stats().committed_guest_bytes;
    CHECK(before >= size);
    a->free(b);
    CHECK(a->stats().committed_guest_bytes < before);      // threshold (256 KiB) reached: whole pages decommitted
    CHECK(a->host(b + 8192, 1) == nullptr);                // a freed big block is no longer mapped: use after free is detectable
    uint32_t c = a->alloc(size);
    CHECK_EQ(c, b);
    h = a->host(c, size);
    bool zero = true;
    for (uint32_t i = 0; i < size; i += 4096) zero = zero && h[i] == 0 && h[i + 4095] == 0;
    CHECK(zero);
    // a small block inside the same pages as a live neighbour is not decommitted
    uint32_t s1 = a->alloc(100), s2 = a->alloc(100);
    a->free(s1);
    CHECK(a->host(s2, 100) != nullptr);
}

TEST_CASE(arena_configuration_errors) {
    nue_result r = NUE_OK;
    ArenaConfig c;
    c.arena_size = 0;
    CHECK(ArenaMemory::create(c, &r) == nullptr);
    CHECK_EQ(r, NUE_ERR_INVALID_ARGUMENT);
    c.arena_size = (4ull << 30) + 4096;
    CHECK(ArenaMemory::create(c, &r) == nullptr);
    CHECK_EQ(r, NUE_ERR_INVALID_ARGUMENT);
    c.arena_size = 1000;                                   // not a multiple of the guest page
    CHECK(ArenaMemory::create(c, &r) == nullptr);
    if (sizeof(void*) == 4) {
        r = NUE_OK;
        CHECK(ArenaMemory::create(ArenaConfig(), &r) == nullptr);
        CHECK_EQ(r, NUE_ERR_UNSUPPORTED);                  // 32-bit hosts use IdentityMemory
    }
}

TEST_CASE(arena_two_arenas_and_release) {
    nue_vm_info before;
    NUE_STRUCT_INIT(&before);
    nue_vm_get_info(&before);
    {
        auto a = make_arena(small_config());
        auto b = make_arena(small_config());
        if (!a || !b) return;
        CHECK(a->base() != b->base());
        uint32_t pa = a->alloc(64), pb = b->alloc(64);
        CHECK_EQ(pa, pb);                                  // same guest address, two independent memories
        std::memset(a->host(pa, 64), 1, 64);
        CHECK(b->host(pb, 64)[0] == 0);
        nue_vm_info during;
        NUE_STRUCT_INIT(&during);
        nue_vm_get_info(&during);
        CHECK_EQ(during.region_count, before.region_count + 2);
    }
    nue_vm_info after;
    NUE_STRUCT_INIT(&after);
    nue_vm_get_info(&after);
    CHECK_EQ(after.region_count, before.region_count);     // the destructors released the regions
}

TEST_CASE(arena_threads_get_distinct_blocks) {
    auto a = make_arena(small_config());
    if (!a) return;
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::vector<uint32_t> mine;
            for (int n = 0; n < 300; ++n) {
                uint32_t p = a->alloc(48 + uint32_t(n % 7) * 16);
                if (!p) { ++bad; break; }
                uint8_t* h = a->host(p, 48);
                if (!h) { ++bad; break; }
                std::memset(h, t + 1, 48);
                mine.push_back(p);
                if (n % 3 == 0 && !mine.empty()) {
                    uint32_t victim = mine[size_t(n) % mine.size()];
                    uint8_t* v = a->host(victim, 48);
                    if (!v || v[0] != t + 1 || v[47] != t + 1) ++bad;   // nobody else wrote into my block
                    a->free(victim);
                    mine.erase(mine.begin() + long(size_t(n) % mine.size()));
                }
            }
            for (uint32_t p : mine) {
                uint8_t* v = a->host(p, 48);
                if (!v || v[0] != t + 1 || v[47] != t + 1) ++bad;
                a->free(p);
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK_EQ(bad.load(), 0);
    CHECK_EQ(a->stats().live_blocks, 0u);
}

TEST_CASE(identity_memory) {
    IdentityMemory m;
    CHECK(m.identity());
    CHECK_EQ(m.commit(0x1000, 4096, kPageReadWrite), NUE_ERR_UNSUPPORTED);
    CHECK_EQ(m.decommit(0x1000, 4096), NUE_ERR_UNSUPPORTED);
    uint32_t p = m.alloc(64);
    if (sizeof(void*) == 8) {
        // a 64-bit process cannot give a 32-bit guest identity addresses: either 0 or (rarely) a low block that works the same way
        if (p == 0) return;
    }
    CHECK(p != 0);
    CHECK_EQ(p % 16, 0u);
    uint8_t* h = m.host(p, 64);
    CHECK(h == reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(p)));
    CHECK_EQ(m.address_of(h + 8), p + 8);
    for (int i = 0; i < 64; ++i) CHECK(h[i] == 0);
    CHECK(m.host(p, 65) == nullptr);                       // beyond the block
    CHECK(m.readable(p, 64) && m.writable(p, 64));
    CHECK_EQ(m.protect(p, 16, kPageReadOnly), uint32_t(kPageReadWrite));
    CHECK(!m.writable(p, 1));
    CHECK_EQ(m.protection(p), uint32_t(kPageReadOnly));
    int local = 0;
    CHECK_EQ(m.address_of(&local), 0u);                    // not registered: foreign
    uint8_t image[256] = {};
    m.adopt(uint32_t(reinterpret_cast<uintptr_t>(image)), sizeof(image), kPageExecuteRead);
    CHECK(m.readable(uint32_t(reinterpret_cast<uintptr_t>(image)), 256));
    CHECK(!m.writable(uint32_t(reinterpret_cast<uintptr_t>(image)), 1));
    m.free(uint32_t(reinterpret_cast<uintptr_t>(image)));  // adopted memory is never freed by us
    CHECK(m.host(uint32_t(reinterpret_cast<uintptr_t>(image)), 1) != nullptr);
    m.free(p);
    CHECK(m.host(p, 1) == nullptr);
}

TEST_CASE(guest_cpu_contract_compiles_and_can_be_implemented) {
    struct FakeCpu final : GuestCpu {
        bool halt = false;
        GuestResult call(const GuestCall& c) override { GuestResult r; r.eax = c.entry + c.arg_count; r.completed = !halt; return r; }
        uint32_t create_context(uint32_t, uint32_t, uint32_t, bool, uint32_t* teb) override { if (teb) *teb = 0x7FFDF000u; return 1; }
        void destroy_context(uint32_t) override {}
        uint32_t current_context() override { return 1; }
        uint32_t teb(uint32_t) override { return 0x7FFDF000u; }
        bool resume_context(uint32_t) override { return true; }
        bool raise_exception(uint32_t, uint32_t, const uint32_t*, uint32_t) override { return false; }
        bool unwind(uint32_t, uint32_t, uint32_t) override { return false; }
        uint16_t fpu_control() override { return 0x27F; }
        void set_fpu_control(uint16_t) override {}
        uint32_t mxcsr() override { return 0x1F80; }
        void set_mxcsr(uint32_t) override {}
        void request_halt(int) override { halt = true; }
        bool halt_requested() override { return halt; }
    } cpu;
    uint32_t args[2] = {1, 2};
    GuestCall call;
    call.entry = 0x401000;
    call.args = args;
    call.arg_count = 2;
    CHECK_EQ(cpu.call(call).eax, 0x401002u);
    cpu.request_halt(3);
    CHECK(!cpu.call(call).completed);
    CHECK_EQ(int(NUE_LEGACY_CPU_ABI), 1);
    CHECK_EQ(int(NUE_LEGACY_MEMORY_ABI), 1);
}

}  // namespace

TEST_MAIN_BEGIN
    RUN(protection_helpers);
    RUN(arena_basics_alloc_free_host_address_of);
    RUN(arena_exhaustion_and_coalescing);
    RUN(arena_page_table_commit_protect_decommit);
    RUN(arena_large_blocks_give_pages_back_and_reuse_is_zeroed);
    RUN(arena_configuration_errors);
    RUN(arena_two_arenas_and_release);
    RUN(arena_threads_get_distinct_blocks);
    RUN(identity_memory);
    RUN(guest_cpu_contract_compiles_and_can_be_implemented);
TEST_MAIN_END
