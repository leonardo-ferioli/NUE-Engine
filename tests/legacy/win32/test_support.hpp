// A tiny test harness: CHECK / CHECK_EQ count failures and print the line; a module registers its test with REGISTER_TEST("name", function) and
// `legacy_win32_tests <name>` runs it (no argument runs all). The function returns nothing: failures are counted by the CHECK macros.
#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace nue::legacy::win32::test {
inline int& failures() { static int n = 0; return n; }
inline int& checks() { static int n = 0; return n; }
using TestFn = void (*)();
inline std::map<std::string, TestFn>& registry() { static std::map<std::string, TestFn> r; return r; }
struct Registrar { Registrar(const char* name, TestFn fn) { registry()[name] = fn; } };
}  // namespace nue::legacy::win32::test

#define CHECK(cond) do { ++::nue::legacy::win32::test::checks(); if (!(cond)) { ++::nue::legacy::win32::test::failures(); std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++::nue::legacy::win32::test::checks(); const auto va_ = (a); const auto vb_ = (b); if (!(va_ == vb_)) { ++::nue::legacy::win32::test::failures(); std::fprintf(stderr, "FAIL %s:%d: %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)va_, (long long)vb_); } } while (0)
#define REGISTER_TEST(name, fn) static ::nue::legacy::win32::test::Registrar registrar_##fn(name, fn)
