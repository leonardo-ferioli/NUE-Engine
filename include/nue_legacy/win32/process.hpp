// The process the guest believes it is: last error, modules (GetModuleHandle / LoadLibrary / GetProcAddress / GetModuleFileName), command line, startup
// information, the environment block, Windows version, system information, processor features, process and thread identity, priorities and affinity,
// error mode, the toolhelp process list, OutputDebugString and process exit.
//
// Everything a program can ask the operating system about itself is *configured* here, not read from the host: the module table holds what the
// facade registers (the main image and the "system DLLs" that the legacy layers stand in for), the version is the Windows the game was made for, and the
// hardware numbers come from platform::System. That is what makes a guest run the same on every host.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/win32/handles.hpp"
#include "nue_legacy/platform/system.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nue::legacy::win32 {

struct OsVersion {                          // OSVERSIONINFO
    uint32_t major = 5, minor = 1, build = 2600, platform = 2 /* VER_PLATFORM_WIN32_NT */;
    std::string service_pack = "Service Pack 3";
};

struct SystemInfo32 {                       // SYSTEM_INFO (32-bit layout values)
    uint16_t processor_architecture = 0;    // PROCESSOR_ARCHITECTURE_INTEL
    uint32_t page_size = 4096, minimum_application_address = 0x10000, maximum_application_address = 0x7FFEFFFF, active_processor_mask = 1, number_of_processors = 1;
    uint32_t processor_type = 586, allocation_granularity = 65536;
    uint16_t processor_level = 6, processor_revision = 0;
};

struct ProcessEntry {                       // PROCESSENTRY32
    uint32_t process_id = 0, threads = 1, parent_process_id = 0;
    int32_t base_priority = 8;
    std::string exe_file;
};

struct ProcessConfig {
    std::string exe_path = "C:\\Game\\GAME.EXE";
    std::string command_line = "GAME.EXE";
    uint32_t image_base = 0x400000;
    uint32_t process_id = 0x1A4C;
    OsVersion version;
    std::vector<std::string> environment = {"OS=Windows_NT", "PATH=C:\\WINDOWS\\system32;C:\\WINDOWS", "SystemRoot=C:\\WINDOWS", "TEMP=C:\\WINDOWS\\Temp", "TMP=C:\\WINDOWS\\Temp"};
};

class ProcessApi {
public:
    ProcessApi(platform::System& system, HandleTable& handles, ProcessConfig config = {});

    // ---- last error (per calling thread)
    static uint32_t get_last_error();
    static void set_last_error(uint32_t);

    // ---- modules. A module is a name, a handle (its base address for the main image) and its exports.
    Handle register_module(std::string_view name, uint32_t base, std::map<std::string, uint32_t> exports = {});   // the facade fills in what a "DLL" offers
    Ret<Handle> get_module_handle(std::string_view name);              // empty name = the main image; the ".dll" extension is optional
    Ret<Handle> load_library(std::string_view name);                   // a registered module, else ERROR_MOD_NOT_FOUND
    Ret<bool> free_library(Handle);
    Ret<uint32_t> get_proc_address(Handle module, std::string_view name);
    Ret<uint32_t> get_proc_address_ordinal(Handle module, uint16_t ordinal);
    // What GetProcAddress answers for a name the module did not register (the module name is lower-case, without ".dll"): the port's import binder (an import token of the host for functions the
    // guest's import table has, a logged-failure token for anything else). Returning 0 keeps ERROR_PROC_NOT_FOUND.
    using ProcResolver = std::function<uint32_t(std::string_view module, std::string_view name)>;
    void set_proc_resolver(ProcResolver resolver) { proc_resolver_ = std::move(resolver); }
    void register_ordinal(Handle module, uint16_t ordinal, uint32_t address);
    Ret<std::string> get_module_file_name(Handle module);              // the main image's path; a registered DLL's C:\WINDOWS\system32 path

    // ---- command line, startup information, environment
    const std::string& command_line() const { return config_.command_line; }
    std::string environment_block() const;                             // "A=b\0C=d\0\0"
    Ret<bool> set_environment_variable(std::string_view name, std::string_view value_or_null, bool remove);
    Ret<std::string> get_environment_variable(std::string_view name) const;

    // ---- system and process facts
    Ret<OsVersion> get_version() const { return ok(config_.version); }
    SystemInfo32 get_system_info() const;
    bool is_processor_feature_present(uint32_t feature) const;         // PF_*
    uint32_t get_current_process_id() const { return config_.process_id; }
    uint32_t set_error_mode(uint32_t mode) { const uint32_t old = error_mode_; error_mode_ = mode; return old; }
    bool is_debugger_present() const { return false; }
    void output_debug_string(std::string_view text);                   // goes to the sink, if any (a program that logs here costs nothing)
    void set_debug_sink(std::function<void(std::string_view)> sink) { debug_sink_ = std::move(sink); }
    uint64_t debug_strings() const { return debug_strings_; }

    // ---- priorities and affinity of the process (the policy of the host decides what is applied)
    uint32_t get_priority_class() const { return priority_class_; }
    bool set_priority_class(uint32_t value);                           // NORMAL 0x20, IDLE 0x40, HIGH 0x80, REALTIME 0x100, BELOW 0x4000, ABOVE 0x8000
    Ret<bool> get_process_affinity_mask(uint64_t& process, uint64_t& system) const;
    Ret<bool> set_process_affinity_mask(uint64_t mask);

    // ---- toolhelp: a snapshot of the process list (only the game itself) walked with first/next
    Ret<Handle> create_toolhelp_snapshot(uint32_t flags);
    Ret<bool> process32_first(Handle snapshot, ProcessEntry&);
    Ret<bool> process32_next(Handle snapshot, ProcessEntry&);

    // ---- exit. The host decides what ExitProcess does (stop the guest); the code is kept.
    void set_exit_handler(std::function<void(uint32_t)> handler) { exit_handler_ = std::move(handler); }
    void exit_process(uint32_t code);
    Ret<bool> terminate_process(Handle process, uint32_t code);        // only the current process (pseudo handle) can be terminated

private:
    struct Module { std::string name; Handle handle = 0; uint32_t base = 0; std::map<std::string, uint32_t> exports; std::map<uint16_t, uint32_t> ordinals; int references = 0; };
    static std::string folded(std::string_view);
    static std::string stem(std::string_view);
    Module* find(std::string_view name);
    Module* by_handle(Handle);

    platform::System& system_;
    HandleTable& handles_;
    ProcessConfig config_;
    std::vector<Module> modules_;
    uint32_t error_mode_ = 0, priority_class_ = 0x20;
    uint64_t affinity_ = 0, debug_strings_ = 0;
    std::function<void(std::string_view)> debug_sink_;
    std::function<void(uint32_t)> exit_handler_;
    ProcResolver proc_resolver_;
};

}  // namespace nue::legacy::win32
