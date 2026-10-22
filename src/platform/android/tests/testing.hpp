// Minimal test registry: TEST(suite, name) { CHECK(...); }  Run: nue_android_tests <suite>   (no arg = all).
#pragma once
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace nue_test {

struct Case {
    const char* suite;
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Registrar {
    Registrar(const char* s, const char* n, std::function<void()> f) { registry().push_back({s, n, std::move(f)}); }
};

} // namespace nue_test

#define TEST(suite, name)                                                                      \
    static void test_##suite##_##name();                                                       \
    static nue_test::Registrar reg_##suite##_##name(#suite, #name, test_##suite##_##name);     \
    static void test_##suite##_##name()

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
            ++nue_test::failures();                                                            \
        }                                                                                      \
    } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        auto va_ = (a);                                                                        \
        auto vb_ = (b);                                                                        \
        if (!(va_ == vb_)) {                                                                   \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);      \
            ++nue_test::failures();                                                            \
        }                                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                  \
    do {                                                                                       \
        double va_ = (a), vb_ = (b);                                                           \
        if (!(va_ - vb_ <= (eps) && vb_ - va_ <= (eps))) {                                     \
            std::fprintf(stderr, "  FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++nue_test::failures();                                                            \
        }                                                                                      \
    } while (0)
