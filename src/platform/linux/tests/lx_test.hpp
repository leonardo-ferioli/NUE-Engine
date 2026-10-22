// Minimal self-registering test harness (no external dependency).
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace lxtest {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& cases() {
    static std::vector<Case> c;
    return c;
}
inline int& failures() {
    static int f = 0;
    return f;
}
inline int& checks() {
    static int c = 0;
    return c;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); }
};

template <class A, class B>
inline void check_eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    ++checks();
    if (!(a == b)) {
        ++failures();
        std::printf("  FAIL %s:%d: %s == %s\n", file, line, ea, eb);
    }
}
inline void check(bool ok, const char* expr, const char* file, int line) {
    ++checks();
    if (!ok) {
        ++failures();
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
}

}  // namespace lxtest

#define LX_CONCAT2(a, b) a##b
#define LX_CONCAT(a, b) LX_CONCAT2(a, b)
#define TEST(name)                                                        \
    static void LX_CONCAT(test_fn_, name)();                              \
    static ::lxtest::Reg LX_CONCAT(test_reg_, name)(#name, LX_CONCAT(test_fn_, name)); \
    static void LX_CONCAT(test_fn_, name)()
#define CHECK(x) ::lxtest::check((x), #x, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::lxtest::check_eq((a), (b), #a, #b, __FILE__, __LINE__)
