#include <cstdio>
#include <cstring>

#include "lx_test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (auto& c : lxtest::cases()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        int before = lxtest::failures();
        c.fn();
        ++ran;
        std::printf("[%s] %s\n", lxtest::failures() == before ? " ok " : "FAIL", c.name);
    }
    std::printf("%d tests, %d checks, %d failures\n", ran, lxtest::checks(), lxtest::failures());
    return lxtest::failures() == 0 ? 0 : 1;
}
