// SEH helpers for diag_tests: functions with __try/__except cannot own C++ objects, so they live in their own translation unit.
// Faults are raised with RaiseException (a call), which every compiler's __try tables cover; clang additionally needs -fasync-exceptions to catch a
// real hardware fault inside __try and, on x86, that option corrupted callee-saved registers of the caller in our measurements (clang 23): the real
// fault paths are tested in crashing child processes instead (diag_tests --crash-child).
#include <windows.h>

void raise_handled(unsigned long code) {
    __try { RaiseException(code, 0, 0, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
// Raises exactly what the CPU raises for a write to `address`: EXCEPTION_ACCESS_VIOLATION with parameters {1 = write, address}.
void write_bad_address(int address) {
    const ULONG_PTR args[2] = {1, ULONG_PTR(address)};
    __try { RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 2, args); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void throw_and_catch_cxx() {
    try { throw 42; } catch (int) {}
}
