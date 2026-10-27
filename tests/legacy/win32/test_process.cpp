// The process: modules, environment, version and system facts, toolhelp, last error, debug output, exit. Windows comparisons are limited to what the
// library derives from the host (processor features, environment variable rules, error codes of the module functions).
#include "nue_legacy/win32/process.hpp"
#include "nue_legacy/platform/null_services.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "test_support.hpp"
#include <thread>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace nue::legacy::win32;
namespace null = nue::legacy::platform::null;

static void test_process() {
    null::NullSystem system;
    system.value.logical_processors = 4;
    HandleTable handles;
    ProcessConfig config;
    config.exe_path = "C:\\Games\\Example\\GAME.EXE";
    config.command_line = "GAME.EXE -windowed";
    ProcessApi p(system, handles, config);
    // Modules.
    CHECK_EQ(p.get_module_handle("").value, 0x400000u);
    CHECK_EQ(p.get_module_handle("game.exe").value, 0x400000u);
    CHECK_EQ(p.get_module_handle("kernel32.dll").error, uint32_t(err::mod_not_found));      // not registered yet
    const Handle k32 = p.register_module("kernel32.dll", 0x7C800000, {{"CreateEventA", 0xF0001000}});
    p.register_ordinal(k32, 1, 0xF0002000);
    CHECK_EQ(p.get_module_handle("KERNEL32").value, k32);                                    // case and extension are free
    CHECK_EQ(p.get_proc_address(k32, "CreateEventA").value, 0xF0001000u);
    CHECK_EQ(p.get_proc_address(k32, "NoSuchFunction").error, uint32_t(err::proc_not_found));
    p.set_proc_resolver([](std::string_view module, std::string_view name) -> uint32_t { return module == "kernel32" && name == "SetEvent" ? 0xF0003000u : 0u; });
    CHECK_EQ(p.get_proc_address(k32, "SetEvent").value, 0xF0003000u);                                       // not registered: the binder answers
    CHECK_EQ(p.get_proc_address(k32, "CreateEventA").value, 0xF0001000u);                                   // registered exports win
    CHECK_EQ(p.get_proc_address(k32, "NoSuchFunction").error, uint32_t(err::proc_not_found));               // the binder says no
    p.set_proc_resolver({});
    CHECK_EQ(p.get_proc_address_ordinal(k32, 1).value, 0xF0002000u);
    CHECK_EQ(p.get_proc_address(0x1234, "x").error, uint32_t(err::invalid_handle));
    CHECK_EQ(p.load_library("nonexistent.dll").error, uint32_t(err::mod_not_found));
    CHECK_EQ(p.load_library("kernel32.dll").value, k32);
    CHECK(p.free_library(k32).value);
    CHECK(p.get_module_file_name(0).value == "C:\\Games\\Example\\GAME.EXE");
    CHECK(p.get_module_file_name(k32).value == "C:\\WINDOWS\\system32\\kernel32.dll");
    // Environment.
    const std::string block = p.environment_block();
    CHECK(block.size() > 10 && block[block.size() - 1] == '\0' && block[block.size() - 2] == '\0');
    CHECK(p.set_environment_variable("MyVar", "1", false).value);
    CHECK(p.get_environment_variable("MYVAR").value == "1");                                 // names are case-insensitive
    CHECK(p.set_environment_variable("myvar", "2", false).value && p.get_environment_variable("MyVar").value == "2");
    CHECK(p.set_environment_variable("MyVar", "", true).value);
    CHECK_EQ(p.get_environment_variable("MyVar").error, uint32_t(err::envvar_not_found));
    CHECK(p.set_environment_variable("MyVar", "", true).value);                       // removing what is not there succeeds (observed on Windows)
    CHECK_EQ(p.set_environment_variable("a=b", "x", false).error, uint32_t(err::invalid_parameter));
    // Facts.
    CHECK_EQ(p.get_version().value.major, 5u);
    CHECK_EQ(p.get_system_info().number_of_processors, 4u);
    CHECK_EQ(p.get_system_info().active_processor_mask, 0xFu);
    CHECK(p.is_processor_feature_present(10) && !p.is_processor_feature_present(40));       // PF_XMMI64 yes, an unknown one no
    uint64_t proc = 0, sys = 0;
    CHECK(p.get_process_affinity_mask(proc, sys).value && proc == 0xF && sys == 0xF);
    CHECK_EQ(p.set_process_affinity_mask(0x30).error, uint32_t(err::invalid_parameter));      // a processor that does not exist
    CHECK(p.set_process_affinity_mask(0x3).value);
    CHECK(p.set_priority_class(0x80) && p.get_priority_class() == 0x80 && !p.set_priority_class(7));
    CHECK_EQ(p.set_error_mode(3), 0u);
    CHECK_EQ(p.set_error_mode(0), 3u);
    // Toolhelp.
    auto snap = p.create_toolhelp_snapshot(2);
    ProcessEntry e;
    CHECK(p.process32_first(snap.value, e).value && e.process_id == p.get_current_process_id() && e.exe_file == "GAME.EXE");
    CHECK_EQ(p.process32_next(snap.value, e).error, uint32_t(err::no_more_files));
    CHECK_EQ(p.process32_first(0x7777, e).error, uint32_t(err::invalid_handle));
    // Debug strings cost nothing and can be observed; exit and terminate go to the host.
    std::string last;
    p.set_debug_sink([&](std::string_view t) { last = std::string(t); });
    p.output_debug_string("hello");
    CHECK(last == "hello" && p.debug_strings() == 1);
    uint32_t exited = 99;
    p.set_exit_handler([&](uint32_t c) { exited = c; });
    p.exit_process(3);
    CHECK_EQ(exited, 3u);
    CHECK(p.terminate_process(kCurrentProcessPseudoHandle, 5).value && exited == 5);
    CHECK_EQ(p.terminate_process(0x1234, 1).error, uint32_t(err::access_denied));
    // The last error belongs to the calling thread.
    ProcessApi::set_last_error(42);
    uint32_t seen = 0;
    std::thread([&] { seen = ProcessApi::get_last_error(); ProcessApi::set_last_error(7); }).join();
    CHECK_EQ(seen, 0u);
    CHECK_EQ(ProcessApi::get_last_error(), 42u);
}
REGISTER_TEST("process", test_process);

#if defined(_WIN32)
static void test_process_against_windows() {
    auto system = nue::legacy::platform::stdlib::make_system();
    HandleTable handles;
    ProcessApi p(*system, handles);
    // Processor features: every PF_* number as the host reports it.
    int different = 0;
    for (uint32_t f = 0; f < 40; ++f) if ((IsProcessorFeaturePresent(f) != 0) != p.is_processor_feature_present(f)) ++different;
    CHECK_EQ(different, 0);
    CHECK_EQ(p.get_system_info().number_of_processors, std::min<uint32_t>(std::max(1u, std::thread::hardware_concurrency()), 32));
    // Environment variable rules.
    SetEnvironmentVariableA("NUE_LEGACY_TEST", "value1");
    char buffer[64];
    SetLastError(0);
    DWORD n = GetEnvironmentVariableA("nue_legacy_test", buffer, sizeof buffer);
    CHECK(n == 6);
    p.set_environment_variable("NUE_LEGACY_TEST", "value1", false);
    CHECK(p.get_environment_variable("nue_legacy_test").value == "value1");
    SetEnvironmentVariableA("NUE_LEGACY_TEST", nullptr);
    SetLastError(0);
    n = GetEnvironmentVariableA("NUE_LEGACY_TEST", buffer, sizeof buffer);
    const DWORD real_error = GetLastError();
    p.set_environment_variable("NUE_LEGACY_TEST", "", true);
    CHECK_EQ(n, 0u);
    CHECK_EQ(real_error, p.get_environment_variable("NUE_LEGACY_TEST").error);
    SetLastError(0);
    const BOOL removed_missing = SetEnvironmentVariableA("NUE_LEGACY_TEST", nullptr);
    const DWORD missing_error = GetLastError();
    const auto mine_missing = p.set_environment_variable("NUE_LEGACY_TEST", "", true);
    CHECK_EQ(removed_missing != 0, mine_missing.value);
    CHECK_EQ(missing_error, mine_missing.error);
    // Module function errors.
    SetLastError(0);
    HMODULE missing = LoadLibraryA("nue_no_such_module.dll");
    const DWORD load_error = GetLastError();
    CHECK(!missing);
    CHECK_EQ(load_error, p.load_library("nue_no_such_module.dll").error);
    SetLastError(0);
    FARPROC none = GetProcAddress(GetModuleHandleA("kernel32.dll"), "NoSuchFunctionAnywhere");
    const DWORD proc_error = GetLastError();
    const Handle k32 = p.register_module("kernel32.dll", 0x7C800000);
    CHECK(!none);
    CHECK_EQ(proc_error, p.get_proc_address(k32, "NoSuchFunctionAnywhere").error);
    // Toolhelp: the real process list contains this process; the library lists the game alone with the same field meanings.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 entry{sizeof entry};
    bool found = false;
    for (BOOL ok = Process32First(snapshot, &entry); ok; ok = Process32Next(snapshot, &entry)) if (entry.th32ProcessID == GetCurrentProcessId()) found = true;
    CloseHandle(snapshot);
    CHECK(found);
    // SetErrorMode returns the previous mode like the real one.
    const UINT before = SetErrorMode(0);
    CHECK_EQ(SetErrorMode(3), 0u);
    CHECK_EQ(p.set_error_mode(0), 0u);
    CHECK_EQ(p.set_error_mode(3), 0u);
    SetErrorMode(before);
}
REGISTER_TEST("process_windows", test_process_against_windows);
#endif
