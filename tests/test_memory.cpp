// Memory info and configurable budgets: policies resolve against the (simulated) machine, config keys override, eviction hooks run.
#include "test_env.h"
#include "nue_test.h"

#include <algorithm>
#include <cstring>

using nue_test::Engine;

static const uint64_t MiB = 1024ull * 1024ull;

static nue_memory_info machine(uint64_t total_mb, uint64_t avail_mb, uint64_t limit_mb = 0, uint64_t used_mb = 0) {
    nue_memory_info m;
    NUE_STRUCT_INIT(&m);
    m.physical_total = total_mb * MiB;
    m.physical_available = avail_mb * MiB;
    m.process_limit = limit_mb * MiB;
    m.process_used = used_mb * MiB;
    return m;
}

struct Pool {
    nue_budget_id id = 0;
    uint64_t freeable = 0;
    int calls = 0;
    nue_memory_pressure last_level = NUE_PRESSURE_NONE;
    uint64_t last_needed = 0;
};

static uint64_t evict_cb(void* ud, nue_budget_id id, uint64_t needed, nue_memory_pressure level) {
    Pool* p = static_cast<Pool*>(ud);
    ++p->calls;
    p->last_level = level;
    p->last_needed = needed;
    uint64_t take = needed ? std::min<uint64_t>(needed, p->freeable) : p->freeable;
    p->freeable -= take;
    nue_budget_report(id, -int64_t(take));
    return take;
}

static nue_budget_id make(const char* name, nue_budget_kind kind, nue_budget_policy policy, Pool* pool = nullptr) {
    nue_budget_desc d;
    NUE_STRUCT_INIT(&d);
    d.name = name;
    d.kind = kind;
    d.policy = policy;
    d.evict = pool ? evict_cb : nullptr;
    d.userdata = pool;
    nue_budget_id id = 0;
    CHECK(NUE_SUCCEEDED(nue_budget_register(&d, &id)));
    if (pool) pool->id = id;
    return id;
}

static nue_budget_policy fraction(float f, uint64_t reserve_mb = 0) {
    nue_budget_policy p;
    std::memset(&p, 0, sizeof(p));
    p.struct_size = sizeof(p);
    p.fraction = f;
    p.reserve_bytes = reserve_mb * MiB;
    return p;
}

TEST_CASE(no_machine_knowledge_means_no_limit_never_a_guess) {
    Engine e;
    nue_memory_info zero = machine(0, 0);
    nue_null_set_memory(&zero);   // the platform could not tell anything
    nue_budget_id id = make("tex.unknown", NUE_BUDGET_TEXTURE, fraction(0.5f));
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_budget_get_state(id, &st), NUE_OK);
    CHECK(st.limit_bytes == UINT64_MAX);
    CHECK_EQ(nue_budget_request(id, 10000 * MiB), NUE_OK);   // accounted, never refused for lack of knowledge
    CHECK_EQ(nue_budget_find("tex.unknown", &id), NUE_OK);
}

TEST_CASE(policy_fraction_reserve_min_max_resolve_against_the_machine) {
    Engine e;
    nue_memory_info m = machine(8192, 4096);
    nue_null_set_memory(&m);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_id a = make("a", NUE_BUDGET_GAME, fraction(0.25f));
    nue_budget_get_state(a, &st);
    CHECK_EQ(st.limit_bytes, 1024 * MiB);   // 25% of the 4096 available, not of the total

    nue_budget_policy p = fraction(0.5f, 256);
    nue_budget_id b = make("b", NUE_BUDGET_GAME, p);
    nue_budget_get_state(b, &st);
    CHECK_EQ(st.limit_bytes, (2048 - 256) * MiB);

    p = fraction(0.5f);
    p.max_bytes = 512 * MiB;
    nue_budget_id c = make("c", NUE_BUDGET_GAME, p);
    nue_budget_get_state(c, &st);
    CHECK_EQ(st.limit_bytes, 512 * MiB);

    p = fraction(0.01f);
    p.min_bytes = 300 * MiB;
    nue_budget_id d = make("d", NUE_BUDGET_GAME, p);
    nue_budget_get_state(d, &st);
    CHECK_EQ(st.limit_bytes, 300 * MiB);

    p = fraction(0.9f);
    p.fixed_bytes = 77 * MiB;   // fixed wins over fraction
    nue_budget_id f = make("f", NUE_BUDGET_GAME, p);
    nue_budget_get_state(f, &st);
    CHECK_EQ(st.limit_bytes, 77 * MiB);

    // the same policy on a different machine gives a different limit: nothing is a constant of the engine
    nue_memory_info small = machine(2048, 800);
    nue_null_set_memory(&small);
    nue_budget_get_state(a, &st);
    CHECK_EQ(st.limit_bytes, 200 * MiB);
}

TEST_CASE(process_limit_bounds_the_base) {
    Engine e;
    nue_memory_info m = machine(16384, 12000, /*limit*/ 3000, /*used*/ 1000);   // a cgroup / jetsam style cap
    nue_null_set_memory(&m);
    nue_memory_info got;
    NUE_STRUCT_INIT(&got);
    nue_memory_get_info(&got);
    CHECK_EQ(got.process_available, 2000 * MiB);   // limit - used, below the free RAM
    nue_budget_id id = make("capped", NUE_BUDGET_TEXTURE, fraction(0.5f));
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.limit_bytes, 1000 * MiB);
}

TEST_CASE(texture_budget_prefers_dedicated_vram_when_present) {
    Engine e;
    nue_memory_info m = machine(16384, 8000);
    m.gpu_dedicated_total = 8192 * MiB;
    m.gpu_dedicated_available = 6000 * MiB;
    nue_null_set_memory(&m);
    nue_budget_id tex = make("tex", NUE_BUDGET_TEXTURE, fraction(0.5f));
    nue_budget_id geo = make("geo", NUE_BUDGET_GEOMETRY, fraction(0.5f));
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(tex, &st);
    CHECK_EQ(st.limit_bytes, 3000 * MiB);
    nue_budget_get_state(geo, &st);
    CHECK_EQ(st.limit_bytes, 4000 * MiB);
    m.unified_memory = NUE_TRUE;   // unified memory: one pool, VRAM numbers are not used
    nue_null_set_memory(&m);
    nue_budget_get_state(tex, &st);
    CHECK_EQ(st.limit_bytes, 4000 * MiB);
}

TEST_CASE(config_keys_override_policy_at_run_time) {
    Engine e;
    nue_memory_info m = machine(8192, 4000);
    nue_null_set_memory(&m);
    nue_budget_id id = make("audio.pool", NUE_BUDGET_AUDIO, fraction(0.125f));
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.limit_bytes, 500 * MiB);
    nue_config_set_double("budget.audio.pool.fraction", 0.5);
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.limit_bytes, 2000 * MiB);
    nue_config_set_int("budget.audio.pool.limit_mb", 64);   // absolute value wins
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.limit_bytes, 64 * MiB);
    nue_config_set_int("budget.audio.pool.limit_mb", 0);    // 0 = back to the policy
    nue_budget_get_state(id, &st);
    CHECK_EQ(st.limit_bytes, 2000 * MiB);
}

TEST_CASE(request_evicts_owner_then_others_and_accounts) {
    Engine e;
    nue_memory_info m = machine(1000, 1000);
    nue_null_set_memory(&m);
    Pool tex, geo;
    nue_budget_policy p = fraction(0);
    p.fixed_bytes = 100 * MiB;
    make("tex", NUE_BUDGET_TEXTURE, p, &tex);
    make("geo", NUE_BUDGET_GEOMETRY, p, &geo);
    CHECK_EQ(nue_budget_request(tex.id, 80 * MiB), NUE_OK);
    tex.freeable = 60 * MiB;
    CHECK_EQ(nue_budget_request(tex.id, 50 * MiB), NUE_OK);   // 130 > 100: the owner is asked for 30
    CHECK_EQ(tex.calls, 1);
    CHECK_EQ(tex.last_needed, 30 * MiB);
    CHECK_EQ(tex.last_level, NUE_PRESSURE_MODERATE);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(tex.id, &st);
    CHECK_EQ(st.used_bytes, 100 * MiB);   // 80 - 30 freed + 50
    CHECK_EQ(st.evictions, 1u);
    CHECK_EQ(st.bytes_evicted, 30 * MiB);
    CHECK_EQ(st.peak_bytes, 100 * MiB);   // the 130 never existed: eviction made room before the request was accounted
    CHECK_EQ(geo.calls, 0);

    // nothing left to free anywhere: the request fails and nothing is accounted
    tex.freeable = 0;
    CHECK_EQ(nue_budget_request(tex.id, 10 * MiB), NUE_ERR_OVERFLOW);
    CHECK_EQ(geo.calls, 1);   // the other pools were asked too
    CHECK_EQ(geo.last_level, NUE_PRESSURE_HIGH);
    nue_budget_get_state(tex.id, &st);
    CHECK_EQ(st.used_bytes, 100 * MiB);
    CHECK_EQ(nue_budget_request(9999, 1), NUE_ERR_INVALID_HANDLE);
}

TEST_CASE(pressure_event_and_low_memory_trim_all_pools) {
    Engine e;
    nue_memory_info m = machine(4096, 2048);
    nue_null_set_memory(&m);
    Pool a, b;
    a.freeable = 40 * MiB; b.freeable = 10 * MiB;
    make("pa", NUE_BUDGET_CACHE_RAM, fraction(0.5f), &a);
    make("pb", NUE_BUDGET_TEXTURE, fraction(0.5f), &b);
    nue_budget_report(a.id, 40 * MiB);
    nue_budget_report(b.id, 10 * MiB);
    nue_null_signal_memory_pressure(NUE_PRESSURE_CRITICAL, 100 * MiB);
    CHECK_EQ(a.calls, 1);
    CHECK_EQ(b.calls, 1);
    CHECK_EQ(a.last_level, NUE_PRESSURE_CRITICAL);
    CHECK_EQ(a.last_needed, 0u);   // "as much as is cheap"
    nue_memory_info got;
    NUE_STRUCT_INIT(&got);
    nue_memory_get_info(&got);
    CHECK_EQ(got.pressure, NUE_PRESSURE_CRITICAL);
    nue_event ev;
    bool saw = false;
    while (nue_poll_event(&ev)) if (ev.type == NUE_EVENT_MEMORY_PRESSURE) { saw = true; CHECK_EQ(ev.payload.memory.level, (uint32_t)NUE_PRESSURE_CRITICAL); }
    CHECK(saw);
    // NONE does not trim
    int before = a.calls;
    nue_null_signal_memory_pressure(NUE_PRESSURE_NONE, 0);
    CHECK_EQ(a.calls, before);
    // the generic low-memory event trims at HIGH
    nue_event lm;
    std::memset(&lm, 0, sizeof(lm));
    lm.type = NUE_EVENT_APP_LOW_MEMORY;
    nue_push_event(&lm);
    CHECK_EQ(a.calls, before + 1);
    CHECK_EQ(a.last_level, NUE_PRESSURE_HIGH);
}

TEST_CASE(lowering_the_limit_evicts_and_a_shrinking_machine_recomputes) {
    Engine e;
    nue_memory_info m = machine(4000, 4000);
    nue_null_set_memory(&m);
    Pool p;
    p.freeable = 1000 * MiB;
    make("shrink", NUE_BUDGET_TEXTURE, fraction(0.5f), &p);
    nue_budget_report(p.id, 1500 * MiB);   // within 2000
    CHECK_EQ(p.calls, 0);
    nue_budget_policy lower = fraction(0.25f);   // 1000 MiB: now 500 over
    CHECK_EQ(nue_budget_set_policy(p.id, &lower), NUE_OK);
    CHECK_EQ(p.calls, 1);
    CHECK_EQ(p.last_needed, 500 * MiB);
    nue_memory_info smaller = machine(4000, 1200);   // the OS needs memory elsewhere: 25% of 1200 = 300
    nue_null_set_memory(&smaller);
    CHECK_EQ(p.calls, 2);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(p.id, &st);
    CHECK_EQ(st.limit_bytes, 300 * MiB);
}

TEST_CASE(registration_rules_and_default_policies) {
    Engine e;
    nue_budget_desc d;
    NUE_STRUCT_INIT(&d);
    d.name = "Bad Name!";
    nue_budget_id id;
    CHECK_EQ(nue_budget_register(&d, &id), NUE_ERR_INVALID_ARGUMENT);
    d.name = "ok-name_1.x";
    d.kind = NUE_BUDGET_TEXTURE;
    CHECK_EQ(nue_budget_register(&d, &id), NUE_OK);
    nue_budget_id again;
    CHECK_EQ(nue_budget_register(&d, &again), NUE_OK_NOOP);
    CHECK_EQ(again, id);
    nue_budget_state st;
    NUE_STRUCT_INIT(&st);
    nue_budget_get_state(id, &st);
    CHECK(st.policy.fraction > 0.0f && st.policy.fraction <= 1.0f);   // the per-class default policy was applied
    CHECK_EQ(st.policy.fixed_bytes, 0u);                             // and it is a fraction, never an absolute size
    nue_budget_policy cache;
    NUE_STRUCT_INIT(&cache);
    nue_budget_default_policy(NUE_BUDGET_CACHE_DISK, &cache);
    CHECK_EQ(cache.fixed_bytes, 0u);
    CHECK_EQ(nue_budget_unregister(id), NUE_OK);
    CHECK_EQ(nue_budget_unregister(id), NUE_ERR_INVALID_HANDLE);
}

TEST_CASE(real_machine_query_is_sane_without_a_platform_layer) {
    Engine e;
    nue_memory_info m;
    NUE_STRUCT_INIT(&m);
    CHECK_EQ(nue_memory_get_info(&m), NUE_OK);
    CHECK(m.process_available <= m.physical_available || m.physical_available == 0);   // generic layer: nothing known, nothing invented
    CHECK_STR(nue_memory_pressure_name(NUE_PRESSURE_HIGH), "high");
}

TEST_MAIN_BEGIN
    RUN(no_machine_knowledge_means_no_limit_never_a_guess);
    RUN(policy_fraction_reserve_min_max_resolve_against_the_machine);
    RUN(process_limit_bounds_the_base);
    RUN(texture_budget_prefers_dedicated_vram_when_present);
    RUN(config_keys_override_policy_at_run_time);
    RUN(request_evicts_owner_then_others_and_accounts);
    RUN(pressure_event_and_low_memory_trim_all_pools);
    RUN(lowering_the_limit_evicts_and_a_shrinking_machine_recomputes);
    RUN(registration_rules_and_default_policies);
    RUN(real_machine_query_is_sane_without_a_platform_layer);
TEST_MAIN_END
