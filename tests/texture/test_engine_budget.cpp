// nue_tex_budget_* behind the engine budget manager (memory.h, NUE_BUDGET_TEXTURE). Engine builds only: the standalone library reports NUE_ERR_UNSUPPORTED.
#include "tex_common.hpp"
#include <cstdio>
#include <cstring>

#ifdef NUE_TEXTURE_WITH_ENGINE
#include "nue/nue.h"
#include "nue/null_backend.h"

static nue_tex_budget g_budget;
static std::vector<nue_tex_resource_id> g_first_candidates;
static int g_calls;

static uint64_t evict(void*, const nue_tex_resource_id* c, uint32_t n, uint64_t need) {
    if (!g_calls++) g_first_candidates.assign(c, c + n);
    uint64_t freed = 0;
    for (uint32_t i = 0; i < n && freed < need; ++i) {
        // the owner knows its sizes: every resource of this test is 400 bytes except the pinned one, which is never offered
        freed += 400;
        nue_tex_budget_remove(g_budget, c[i]);
    }
    return freed;
}

TEST_CASE(texture_budget_behind_the_engine_budget_manager) {
    nue_init_desc d; NUE_STRUCT_INIT(&d);
    d.api_version = NUE_API_VERSION; d.app_name = "texture test"; d.app_id = "tex-test"; d.org_name = "NUE";
    d.backend = NUE_BACKEND_NULL; d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_CRASH_HANDLER | NUE_INIT_NO_LOG_FILE | NUE_INIT_DETERMINISTIC;
    d.log_level = NUE_LOG_ERROR;
    CHECK(NUE_SUCCEEDED(nue_init(&d)));

    nue_tex_budget_policy none; NUE_STRUCT_INIT(&none);
    nue_tex_budget b = nullptr;
    CHECK_EQ(nue_tex_budget_create(&none, evict, nullptr, &b), NUE_OK);
    g_budget = b; g_calls = 0;
    nue_tex_resource_id a, c, e, pinned, big;
    CHECK_EQ(nue_tex_budget_add(b, 400, 0, &a), NUE_OK);                         // accounted locally before the attach
    uint32_t eid = 0;
    CHECK_EQ(nue_tex_budget_attach_engine(b, "texture", &eid), NUE_OK);
    CHECK(eid != 0);
    CHECK_EQ(nue_tex_budget_attach_engine(b, "texture", &eid), NUE_ERR_ALREADY_EXISTS);

    nue_budget_policy p; std::memset(&p, 0, sizeof p); p.struct_size = sizeof p; p.fixed_bytes = 1000;
    CHECK_EQ(nue_budget_set_policy(eid, &p), NUE_OK);
    nue_budget_state st; NUE_STRUCT_INIT(&st);
    CHECK_EQ(nue_budget_get_state(eid, &st), NUE_OK);
    CHECK_EQ(st.limit_bytes, 1000u); CHECK_EQ(st.used_bytes, 400u);              // the pre-attach use was reported

    CHECK_EQ(nue_tex_budget_add(b, 400, 0, &c), NUE_OK);                         // 800
    nue_tex_budget_touch(b, a);                                                   // c is now the least recently used
    CHECK_EQ(nue_tex_budget_add(b, 400, 0, &e), NUE_OK);                         // 1200 > 1000: the engine asks us, we evict c
    CHECK_EQ(g_calls, 1);
    CHECK(!g_first_candidates.empty() && g_first_candidates[0] == c);
    nue_budget_get_state(eid, &st);
    CHECK_EQ(st.used_bytes, 800u); CHECK(st.evictions >= 1u);
    CHECK_EQ(nue_tex_budget_remove(b, c), NUE_ERR_INVALID_HANDLE);                // already gone

    CHECK_EQ(nue_tex_budget_add(b, 300, NUE_TEX_RES_PINNED, &pinned), NUE_OK);   // 1100: evicts the oldest unpinned (a)
    nue_budget_get_state(eid, &st);
    CHECK_EQ(st.used_bytes, 700u);
    nue_tex_budget_stats ts; NUE_STRUCT_INIT(&ts); nue_tex_budget_get_stats(b, &ts);
    CHECK_EQ(ts.used, 700u); CHECK_EQ(ts.pinned, 1u);

    CHECK_EQ(nue_tex_budget_add(b, 5000, 0, &big), NUE_ERR_OVERFLOW);            // cannot fit even after evicting everything offered; nothing accounted
    nue_budget_get_state(eid, &st);
    CHECK(st.used_bytes <= 700u);
    nue_tex_budget_get_stats(b, &ts);
    CHECK_EQ(ts.pinned, 1u);

    // memory pressure from the OS reaches the owner through the same hook; pinned resources stay
    nue_tex_resource_id extra;
    CHECK_EQ(nue_tex_budget_add(b, 100, 0, &extra), NUE_OK);
    const uint64_t freed = nue_budget_trim(NUE_PRESSURE_CRITICAL);
    CHECK(freed > 0);
    nue_tex_budget_get_stats(b, &ts);
    CHECK_EQ(ts.resources, 1u); CHECK_EQ(ts.pinned, 1u);
    nue_budget_get_state(eid, &st);
    CHECK_EQ(st.used_bytes, ts.used);

    CHECK_EQ(nue_tex_budget_resize(b, pinned, 100), NUE_OK);                      // shrinking is reported
    nue_budget_get_state(eid, &st);
    CHECK_EQ(st.used_bytes, 100u);
    CHECK_EQ(nue_tex_budget_detach_engine(b), NUE_OK);
    nue_tex_budget_destroy(b);
    nue_shutdown();
}
#else
TEST_CASE(texture_budget_behind_the_engine_budget_manager) {
    nue_tex_budget_policy none; NUE_STRUCT_INIT(&none);
    nue_tex_budget b = nullptr;
    CHECK_EQ(nue_tex_budget_create(&none, nullptr, nullptr, &b), NUE_OK);
    CHECK_EQ(nue_tex_budget_attach_engine(b, "texture", nullptr), NUE_ERR_UNSUPPORTED);      // the standalone library has no engine to attach to
    nue_tex_budget_destroy(b);
}
#endif

TEST_MAIN_BEGIN
    RUN(texture_budget_behind_the_engine_budget_manager);
TEST_MAIN_END
