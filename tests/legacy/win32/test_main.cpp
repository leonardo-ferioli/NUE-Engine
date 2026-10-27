#include "test_support.hpp"
#include <cstring>

int main(int argc, char** argv) {
    using namespace nue::legacy::win32::test;
    int ran = 0;
    for (const auto& [name, fn] : registry()) {
        if (argc > 1 && name != argv[1]) continue;
        const int before = failures(), checks_before = checks();
        fn();
        std::printf("%s %s: %d checks, %d failures\n", failures() == before ? "PASS" : "FAIL", name.c_str(), checks() - checks_before, failures() - before);
        ++ran;
    }
    if (!ran) { std::fprintf(stderr, "no test named '%s'\n", argc > 1 ? argv[1] : ""); return 2; }
    return failures() ? 1 : 0;
}
