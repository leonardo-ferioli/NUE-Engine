// Minimal console test harness for the Windows backend (no dependencies; ctest runs each test executable).
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nuetest {
inline int& failures() { static int n = 0; return n; }
inline int& checks() { static int n = 0; return n; }
inline bool& verbose() { static bool v = false; return v; }
}  // namespace nuetest

#define CHECK(cond) do { ++nuetest::checks(); if (!(cond)) { ++nuetest::failures(); std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_MSG(cond, ...) do { ++nuetest::checks(); if (!(cond)) { ++nuetest::failures(); std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
#define CHECK_OK(status) do { ++nuetest::checks(); auto _s = (status); if (!_s.ok) { ++nuetest::failures(); std::printf("  FAIL %s:%d: %s -> %s (0x%08X)\n", __FILE__, __LINE__, #status, _s.what, _s.code); } } while (0)
#define INFO(...) do { std::printf("  " __VA_ARGS__); std::printf("\n"); } while (0)
#define SKIP(...) do { std::printf("  SKIP " __VA_ARGS__); std::printf("\n"); } while (0)
#define TEST_BEGIN(name) std::printf("[%s]\n", name)

inline int finish(const char* suite) {
    std::printf("%s: %d checks, %d failures\n", suite, nuetest::checks(), nuetest::failures());
    return nuetest::failures() ? 1 : 0;
}
