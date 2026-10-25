// Tiny test harness: console only, no dependencies. Each test executable runs its cases in order and exits non-zero on failure.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace nue_test {
inline int& failures() { static int n = 0; return n; }
inline int& checks() { static int n = 0; return n; }
inline void on_terminate() {
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
    // exceptions are enabled in this translation unit: report what was thrown
    try { throw; } catch (const std::exception& e) { std::printf("  UNCAUGHT EXCEPTION: %s\n", e.what()); } catch (...) { std::printf("  UNCAUGHT NON-STD EXCEPTION\n"); }
#else
    std::printf("  terminate called (this test is built without exceptions)\n");
#endif
    std::abort();
}
}  // namespace nue_test

#define CHECK(cond) \
    do { ++nue_test::checks(); if (!(cond)) { ++nue_test::failures(); std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) \
    do { ++nue_test::checks(); auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { ++nue_test::failures(); \
         std::printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)va_, (long long)vb_); } } while (0)
#define CHECK_STR(a, b) \
    do { ++nue_test::checks(); const char* sa_ = (a); const char* sb_ = (b); if (std::strcmp(sa_, sb_) != 0) { ++nue_test::failures(); \
         std::printf("  FAIL %s:%d: \"%s\" vs \"%s\"\n", __FILE__, __LINE__, sa_, sb_); } } while (0)
#define CHECK_NEAR(a, b, eps) \
    do { ++nue_test::checks(); double da_ = (double)(a), db_ = (double)(b); double d_ = da_ > db_ ? da_ - db_ : db_ - da_; \
         if (!(d_ <= (eps))) { ++nue_test::failures(); std::printf("  FAIL %s:%d: %s ~ %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, da_, db_); } } while (0)
#define TEST_CASE(name) static void name()

#define TEST_MAIN_BEGIN int main() { int status_ = 0; std::setvbuf(stdout, nullptr, _IONBF, 0); std::set_terminate(nue_test::on_terminate);
#define RUN(name) do { std::printf("[ test] %s\n", #name); name(); } while (0)
#define TEST_MAIN_END \
    std::printf("%d checks, %d failures\n", nue_test::checks(), nue_test::failures()); \
    return nue_test::failures() ? 1 : status_; }
