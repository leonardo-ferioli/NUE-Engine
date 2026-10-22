#include <cstdio>
#include <cstring>

#include "testing.hpp"

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_cases = 0;
    for (const nue_test::Case& c : nue_test::registry()) {
        if (only && std::strcmp(only, c.suite) != 0) continue;
        int before = nue_test::failures();
        c.fn();
        ++ran;
        bool ok = nue_test::failures() == before;
        if (!ok) ++failed_cases;
        std::printf("[%s] %s.%s\n", ok ? " ok " : "FAIL", c.suite, c.name);
    }
    std::printf("%d test(s), %d failed\n", ran, failed_cases);
    if (ran == 0) {
        std::fprintf(stderr, "no tests matched\n");
        return 2;
    }
    return failed_cases ? 1 : 0;
}
