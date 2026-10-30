#define PSAPI_VERSION 2
#include "nue_win/diag.hpp"

#include <dbghelp.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <TraceLoggingProvider.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <memory>
#include <mutex>

#include "nue_win/power.hpp"
#include "nue_win/storage.hpp"
#include "nue_win/window.hpp"

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "advapi32.lib")

namespace nue::win {

// ---- Exception names --------------------------------------------------------------------------------------------------------------------------

const char* exception_code_name(uint32_t c) {
    switch (c) {
        case 0xC0000005: return "ACCESS_VIOLATION";
        case 0xC0000006: return "IN_PAGE_ERROR";
        case 0xC000001D: return "ILLEGAL_INSTRUCTION";
        case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
        case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
        case 0xC0000095: return "INT_OVERFLOW";
        case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
        case 0xC00000FD: return "STACK_OVERFLOW";
        case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
        case 0xC000008D: return "FLT_DENORMAL_OPERAND";
        case 0xC000008E: return "FLT_DIVIDE_BY_ZERO";
        case 0xC0000090: return "FLT_INVALID_OPERATION";
        case 0xC0000091: return "FLT_OVERFLOW";
        case 0xC0000374: return "HEAP_CORRUPTION";
        case 0xC0000409: return "STACK_BUFFER_OVERRUN (fast fail)";
        case 0x80000001: return "GUARD_PAGE_VIOLATION";
        case 0x80000002: return "DATATYPE_MISALIGNMENT";
        case 0x80000003: return "BREAKPOINT";
        case 0x80000004: return "SINGLE_STEP";
        case 0xE06D7363: return "C++ exception";
        default: return "unknown";
    }
}

// ---- dbghelp (loaded on demand: only a crash needs it) -------------------------------------------------------------------------------------------

namespace {
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
using MiniDumpReadDumpStreamFn = BOOL(WINAPI*)(PVOID, ULONG, PMINIDUMP_DIRECTORY*, PVOID*, ULONG*);

MiniDumpWriteDumpFn dump_writer() {
    static MiniDumpWriteDumpFn fn = load_proc<MiniDumpWriteDumpFn>(L"dbghelp.dll", "MiniDumpWriteDump");
    return fn;
}

std::string format_time_local() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[64];
    std::snprintf(b, sizeof b, "%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);   // sorts chronologically
    return b;
}
}  // namespace

namespace {
// Opens the dump for writing. A freshly written dump is often held open for a moment by an anti-virus scanner (Windows Defender on CI runners), which makes an
// exclusive open fail with a sharing violation; retry for up to ~1 s instead of failing the report.
HANDLE open_dump_file(const wchar_t* path) {
    for (int attempt = 0;; ++attempt) {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) return h;
        const DWORD e = GetLastError();
        if (attempt >= 20 || (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION)) return h;
        Sleep(50);
    }
}
}  // namespace

uint32_t reserve_crash_stack(uint32_t bytes) {
    ULONG size = bytes;
    if (!SetThreadStackGuarantee(&size)) return 0;
    return uint32_t(size);
}

Status write_minidump(EXCEPTION_POINTERS* ex, const std::wstring& path, bool full) {
    auto write = dump_writer();
    if (!write) return Status::fail("dbghelp.dll / MiniDumpWriteDump unavailable", ERROR_PROC_NOT_FOUND);
    HANDLE file = open_dump_file(to_extended_path(path).c_str());
    if (file == INVALID_HANDLE_VALUE) return Status::last_error("CreateFile(dump)");
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ex;
    info.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = MINIDUMP_TYPE((full ? MiniDumpWithFullMemory : MiniDumpWithIndirectlyReferencedMemory) | MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                                              MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo);
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, ex ? &info : nullptr, nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(file);
    if (!ok) { DeleteFileW(to_extended_path(path).c_str()); return Status::fail("MiniDumpWriteDump", uint32_t(err)); }
    return Status::success();
}

bool read_minidump_summary(const std::wstring& path, DumpSummary& out) {
    out = DumpSummary{};
    auto read = load_proc<MiniDumpReadDumpStreamFn>(L"dbghelp.dll", "MiniDumpReadDumpStream");
    if (!read) return false;
    HANDLE f = CreateFileW(to_extended_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* base = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : nullptr;
    bool ok = false;
    if (base) {
        PMINIDUMP_DIRECTORY dir = nullptr;
        void* stream = nullptr;
        ULONG size = 0;
        if (read(base, ExceptionStream, &dir, &stream, &size) && stream) {
            auto* e = static_cast<MINIDUMP_EXCEPTION_STREAM*>(stream);
            out.has_exception = true;
            out.exception_code = e->ExceptionRecord.ExceptionCode;
            out.exception_address = e->ExceptionRecord.ExceptionAddress;
            out.crashing_thread_id = e->ThreadId;
        }
        if (read(base, ModuleListStream, &dir, &stream, &size) && stream) out.module_count = static_cast<MINIDUMP_MODULE_LIST*>(stream)->NumberOfModules;
        if (read(base, ThreadListStream, &dir, &stream, &size) && stream) out.thread_count = static_cast<MINIDUMP_THREAD_LIST*>(stream)->NumberOfThreads;
        if (read(base, SystemInfoStream, &dir, &stream, &size) && stream) { out.has_system_info = true; out.processor_architecture = static_cast<MINIDUMP_SYSTEM_INFO*>(stream)->ProcessorArchitecture; }
        out.has_handle_data = read(base, HandleDataStream, &dir, &stream, &size) != FALSE;
        out.has_thread_info = read(base, ThreadInfoListStream, &dir, &stream, &size) != FALSE;
        out.valid = std::memcmp(base, "MDMP", 4) == 0;
        ok = out.valid;
        UnmapViewOfFile(base);
    }
    if (map) CloseHandle(map);
    CloseHandle(f);
    return ok;
}

// ---- Exception log ---------------------------------------------------------------------------------------------------------------------------------

namespace {
std::unique_ptr<ExceptionLogEntry[]> g_log;
size_t g_log_capacity = 0;
std::atomic<uint64_t> g_log_total{0};
PVOID g_log_handle = nullptr;
bool g_log_cxx = false;

LONG CALLBACK exception_log_handler(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    const uint32_t code = r->ExceptionCode;
    if (!g_log_cxx && (code == 0xE06D7363 || code == 0x406D1388 || code == 0x40010006 || code == 0x4001000A || code == 0x40010005)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_log) {
        const uint64_t i = g_log_total.fetch_add(1, std::memory_order_relaxed);
        ExceptionLogEntry& e = g_log[i % g_log_capacity];
        e.code = code;
        e.flags = r->ExceptionFlags;
        e.thread_id = GetCurrentThreadId();
        e.address = uint64_t(reinterpret_cast<uintptr_t>(r->ExceptionAddress));
        e.info0 = r->NumberParameters > 0 ? uint64_t(r->ExceptionInformation[0]) : 0;
        e.info1 = r->NumberParameters > 1 ? uint64_t(r->ExceptionInformation[1]) : 0;
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        e.qpc = q.QuadPart;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

Status install_exception_log(size_t capacity, bool include_cxx) {
    remove_exception_log();
    g_log_capacity = std::max<size_t>(16, capacity);
    g_log.reset(new ExceptionLogEntry[g_log_capacity]());
    g_log_total = 0;
    g_log_cxx = include_cxx;
    g_log_handle = AddVectoredExceptionHandler(0, exception_log_handler);   // 0 = called last
    return g_log_handle ? Status::success() : Status::last_error("AddVectoredExceptionHandler");
}

void remove_exception_log() {
    if (g_log_handle) { RemoveVectoredExceptionHandler(g_log_handle); g_log_handle = nullptr; }
    // the buffer stays allocated: a handler that is running right now on another thread may still be writing into it
}

std::vector<ExceptionLogEntry> exception_log_snapshot() {
    std::vector<ExceptionLogEntry> out;
    if (!g_log) return out;
    const uint64_t total = g_log_total.load();
    const size_t cap = g_log_capacity;
    const size_t n = total < cap ? size_t(total) : cap;
    out.reserve(n);
    size_t at = total < cap ? 0 : size_t(total % cap);   // index of the oldest entry once the ring has wrapped
    for (size_t k = 0; k < n; ++k) {
        out.push_back(g_log[at]);
        if (++at == cap) at = 0;
    }
    return out;
}

uint64_t exception_log_total() { return g_log_total.load(); }

// ---- Crash handler ---------------------------------------------------------------------------------------------------------------------------------

namespace {

CrashOptions g_crash;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
bool g_crash_installed = false;
std::atomic<bool> g_crashing{false};
std::wstring g_last_dump;

std::string module_of(const void* address, uint64_t* offset) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &m) || !m) return "<unknown>";
    wchar_t name[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, name, MAX_PATH);
    if (offset) *offset = uint64_t(reinterpret_cast<const uint8_t*>(address) - reinterpret_cast<const uint8_t*>(m));
    std::wstring w(name, n);
    size_t k = w.find_last_of(L"\\/");
    return to_utf8(k == std::wstring::npos ? w : w.substr(k + 1));
}

void append(std::string& s, const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    s += b;
}

std::string build_report(EXCEPTION_POINTERS* ep, DWORD thread_id, const std::wstring& dump_path) {
    std::string r;
    const EXCEPTION_RECORD* x = ep ? ep->ExceptionRecord : nullptr;
    append(r, "NUE crash report\r\nbuild: %s\r\n", g_crash.build_id.c_str());
    const OsVersion os = os_version();
    append(r, "windows: %u.%u build %u, %s process\r\n", os.major, os.minor, os.build, sizeof(void*) == 4 ? "32-bit" : "64-bit");
    append(r, "time: %s (local), pid %lu, crashing thread %lu\r\n", format_time_local().c_str(), GetCurrentProcessId(), thread_id);
    append(r, "dump: %s\r\n", to_utf8(dump_path).c_str());
    if (x) {
        uint64_t off = 0;
        const std::string mod = module_of(x->ExceptionAddress, &off);
        append(r, "exception: 0x%08lX %s at %p (%s+0x%llX)\r\n", x->ExceptionCode, exception_code_name(x->ExceptionCode), x->ExceptionAddress, mod.c_str(), (unsigned long long)off);
        if ((x->ExceptionCode == 0xC0000005 || x->ExceptionCode == 0xC0000006) && x->NumberParameters >= 2)
            append(r, "access violation: %s address 0x%llX\r\n", x->ExceptionInformation[0] == 0 ? "reading" : (x->ExceptionInformation[0] == 8 ? "executing (DEP)" : "writing"), (unsigned long long)x->ExceptionInformation[1]);
        const CONTEXT* c = ep->ContextRecord;
#ifdef _WIN64
        append(r, "rip=%016llX rsp=%016llX rbp=%016llX\r\nrax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\r\nrsi=%016llX rdi=%016llX r8=%016llX r9=%016llX\r\n",
               (unsigned long long)c->Rip, (unsigned long long)c->Rsp, (unsigned long long)c->Rbp, (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx,
               (unsigned long long)c->Rdx, (unsigned long long)c->Rsi, (unsigned long long)c->Rdi, (unsigned long long)c->R8, (unsigned long long)c->R9);
#else
        append(r, "eip=%08lX esp=%08lX ebp=%08lX\r\neax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX\r\n", c->Eip, c->Esp, c->Ebp, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
#endif
    } else {
        append(r, "exception: none (requested dump)\r\n");
    }
    MemorySample m = sample_memory();
    if (m.valid) append(r, "memory: working set %llu MiB (peak %llu), private %llu MiB, handles %lu\r\n", (unsigned long long)(m.working_set >> 20), (unsigned long long)(m.peak_working_set >> 20),
                        (unsigned long long)(m.private_bytes >> 20), (unsigned long)m.handle_count);
    const auto log = exception_log_snapshot();
    if (!log.empty()) {
        append(r, "\r\nlast first-chance exceptions (%llu recorded in total):\r\n", (unsigned long long)exception_log_total());
        for (size_t i = log.size() > 16 ? log.size() - 16 : 0; i < log.size(); ++i)
            append(r, "  0x%08X %s at 0x%llX thread %u info 0x%llX 0x%llX\r\n", log[i].code, exception_code_name(log[i].code), (unsigned long long)log[i].address, log[i].thread_id, (unsigned long long)log[i].info0,
                   (unsigned long long)log[i].info1);
    }
    HMODULE mods[512];
    DWORD needed = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
        append(r, "\r\nmodules:\r\n");
        for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 512); ++i) {
            wchar_t name[MAX_PATH];
            MODULEINFO mi{};
            K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
            DWORD n = GetModuleFileNameW(mods[i], name, MAX_PATH);
            append(r, "  %p %8lX %s\r\n", mi.lpBaseOfDll, mi.SizeOfImage, to_utf8(std::wstring(name, n)).c_str());
        }
    }
    return r;
}

void prune_old_dumps(const std::wstring& dir, const std::wstring& app, uint32_t keep) {
    if (keep == 0) return;
    for (const wchar_t* ext : {L".dmp", L".txt"}) {
        std::vector<std::wstring> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(to_extended_path(dir + L"\\" + app + L"-*" + ext).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        do { files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(files.begin(), files.end());   // names start with a sortable timestamp
        while (files.size() >= keep) { DeleteFileW(to_extended_path(dir + L"\\" + files.front()).c_str()); files.erase(files.begin()); }
    }
}

struct DumpJob {
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD crashing_thread = 0;
    Status status;
    bool terminate = true;
};
CrashHook g_hook;
bool g_hook_active = false;
CrashInfo g_hook_info;

// Builds the NUE crash facts from an exception without heap allocation.
void fill_crash_info(CrashInfo& info, const EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* x = ep->ExceptionRecord;
    info.code = x->ExceptionCode;
    info.fault_pc = uint64_t(reinterpret_cast<uintptr_t>(x->ExceptionAddress));
    info.fault_address = info.fault_pc;
    if ((x->ExceptionCode == 0xC0000005 || x->ExceptionCode == 0xC0000006) && x->NumberParameters >= 2) {
        info.fault_address = uint64_t(x->ExceptionInformation[1]);
        std::snprintf(info.reason, sizeof info.reason, "access violation %s 0x%llX", x->ExceptionInformation[0] == 0 ? "reading" : (x->ExceptionInformation[0] == 8 ? "executing" : "writing"),
                      (unsigned long long)info.fault_address);
    } else {
        std::snprintf(info.reason, sizeof info.reason, "%s (0x%08lX)", exception_code_name(x->ExceptionCode), x->ExceptionCode);
    }
}

DumpJob g_job;   // static: the filter may run on an exhausted stack (stack overflow), so it allocates nothing and keeps nothing there

// Everything heavy happens on this fresh thread (1 MiB stack).
DWORD WINAPI hook_thread(LPVOID) {
    DumpJob& j = g_job;
    if (g_hook.release_input) emergency_restore_desktop();
    fill_crash_info(g_hook_info, j.ep);
    g_hook_info.report_dir = g_hook.begin ? g_hook.begin(g_hook_info, g_hook.user) : nullptr;
    if (g_hook.write_dump && g_hook_info.report_dir) {
        auto write = dump_writer();
        wchar_t dir[MAX_PATH * 2];
        int n = MultiByteToWideChar(CP_UTF8, 0, g_hook_info.report_dir, -1, dir, int(sizeof dir / sizeof dir[0]) - 16);
        if (write && n > 0) {
            wchar_t path[MAX_PATH * 2 + 16];
            std::swprintf(path, sizeof path / sizeof path[0], L"%ls\\crash.dmp", dir);
            HANDLE file = open_dump_file(path);
            if (file != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION info{};
                info.ThreadId = j.crashing_thread;
                info.ExceptionPointers = j.ep;
                info.ClientPointers = FALSE;
                const MINIDUMP_TYPE type = MINIDUMP_TYPE((g_hook.full_memory ? MiniDumpWithFullMemory : MiniDumpWithIndirectlyReferencedMemory) | MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                                          MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo);
                write(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
                CloseHandle(file);
            }
        }
    }
    j.status = Status::success();
    j.terminate = g_hook.end ? g_hook.end(g_hook_info, g_hook.user) : true;
    return 0;
}

DWORD WINAPI dump_thread(LPVOID) {
    DumpJob& j = g_job;
    if (g_crash.restore_desktop) emergency_restore_desktop();
    create_directories(g_crash.dump_dir);
    prune_old_dumps(g_crash.dump_dir, g_crash.app_name, g_crash.max_dumps_kept);
    const std::wstring stem = g_crash.dump_dir + L"\\" + g_crash.app_name + L"-" + to_wide(format_time_local()) + L"-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring dump_path = stem + L".dmp", report_path = stem + L".txt";

    auto write = dump_writer();
    if (!write) { j.status = Status::fail("dbghelp.dll unavailable", ERROR_PROC_NOT_FOUND); return 1; }
    HANDLE file = open_dump_file(to_extended_path(dump_path).c_str());
    if (file == INVALID_HANDLE_VALUE) { j.status = Status::last_error("CreateFile(dump)"); return 1; }
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = j.crashing_thread;
    info.ExceptionPointers = j.ep;
    info.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = MINIDUMP_TYPE((g_crash.full_memory ? MiniDumpWithFullMemory : MiniDumpWithIndirectlyReferencedMemory) | MiniDumpWithDataSegs | MiniDumpWithHandleData |
                                              MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo);
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, j.ep ? &info : nullptr, nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(file);
    j.status = ok ? Status::success() : Status::fail("MiniDumpWriteDump", uint32_t(err));
    if (ok) g_last_dump = dump_path;
    if (g_crash.write_text_report) {
        const std::string text = build_report(j.ep, j.crashing_thread, dump_path);
        write_file_atomic(report_path, text.data(), text.size());
    }
    if (g_crash.on_crash) g_crash.on_crash(ok ? dump_path : std::wstring(), g_crash.write_text_report ? report_path : std::wstring());
    return 0;
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (g_hook_active) {
        if (g_hook.skip_when_debugger_attached && IsDebuggerPresent()) return EXCEPTION_CONTINUE_SEARCH;
    } else if (g_crash.skip_when_debugger_attached && IsDebuggerPresent()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crashing.exchange(true)) TerminateProcess(GetCurrentProcess(), ep->ExceptionRecord->ExceptionCode);   // crashed again while crashing
    g_job.ep = ep;
    g_job.crashing_thread = GetCurrentThreadId();
    HANDLE t = CreateThread(nullptr, 1 << 20, g_hook_active ? hook_thread : dump_thread, nullptr, 0, nullptr);
    if (t) { WaitForSingleObject(t, 120000); CloseHandle(t); }
    if (g_hook_active && !g_job.terminate) { g_crashing = false; return EXCEPTION_CONTINUE_SEARCH; }   // the caller wants the OS default handling
    if ((g_hook_active || g_crash.chain_previous_filter) && g_previous_filter) return g_previous_filter(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

Status install_crash_handler(const CrashOptions& o) {
    if (o.dump_dir.empty()) return Status::fail("dump_dir is empty", ERROR_INVALID_PARAMETER);
    if (g_crash_installed) uninstall_crash_handler();
    g_crash = o;
    g_crashing = false;
    reserve_crash_stack();
    g_previous_filter = SetUnhandledExceptionFilter(crash_filter);
    g_crash_installed = true;
    return Status::success();
}

Status install_crash_hook(const CrashHook& h) {
    if (g_crash_installed) uninstall_crash_handler();
    g_hook = h;
    g_hook_active = true;
    g_crashing = false;
    reserve_crash_stack();
    g_previous_filter = SetUnhandledExceptionFilter(crash_filter);
    g_crash_installed = true;
    return Status::success();
}

void uninstall_crash_handler() {
    if (!g_crash_installed) return;
    g_hook_active = false;
    SetUnhandledExceptionFilter(g_previous_filter);
    g_previous_filter = nullptr;
    g_crash_installed = false;
}

bool crash_handler_installed() { return g_crash_installed; }
std::wstring last_crash_dump_path() { return g_last_dump; }

// ---- ETW -----------------------------------------------------------------------------------------------------------------------------------------

TRACELOGGING_DEFINE_PROVIDER(g_nue_provider, "NUE.Engine", (0xc8e1a4b2, 0x3d57, 0x4f6a, 0x9b, 0x0e, 0x71, 0xd2, 0xf5, 0xa8, 0xc3, 0xe4));

namespace { std::atomic<bool> g_etw_registered{false}; }

Status etw_register() {
    if (g_etw_registered.exchange(true)) return Status::success();
    const LONG r = TraceLoggingRegister(g_nue_provider);
    if (r != ERROR_SUCCESS) { g_etw_registered = false; return Status::fail("TraceLoggingRegister", uint32_t(r)); }
    return Status::success();
}
void etw_unregister() { if (g_etw_registered.exchange(false)) TraceLoggingUnregister(g_nue_provider); }
bool etw_enabled() { return g_etw_registered && TraceLoggingProviderEnabled(g_nue_provider, 0, 0); }
void etw_event(const char* name, const char* detail, int64_t value) {
    if (!g_etw_registered) return;
    TraceLoggingWrite(g_nue_provider, "Event", TraceLoggingString(name, "name"), TraceLoggingString(detail, "detail"), TraceLoggingInt64(value, "value"));
}
void etw_frame(uint64_t index, double cpu_ms, double frame_ms) {
    if (!g_etw_registered) return;
    TraceLoggingWrite(g_nue_provider, "Frame", TraceLoggingLevel(5 /*TRACE_LEVEL_VERBOSE*/), TraceLoggingUInt64(index, "frame"), TraceLoggingFloat64(cpu_ms, "cpu_ms"), TraceLoggingFloat64(frame_ms, "frame_ms"));
}
void etw_mark(const char* label) {
    if (!g_etw_registered) return;
    TraceLoggingWrite(g_nue_provider, "Mark", TraceLoggingString(label, "label"));
}

// ---- Sampling -----------------------------------------------------------------------------------------------------------------------------------

namespace {
double filetime_ms(const FILETIME& f) { ULARGE_INTEGER u; u.LowPart = f.dwLowDateTime; u.HighPart = f.dwHighDateTime; return double(u.QuadPart) / 10000.0; }
}

ProcessSample sample_process() {
    ProcessSample s;
    s.wall_ms = qpc_to_ms(qpc_now());
    FILETIME create, exit_, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &create, &exit_, &kernel, &user)) s.process_cpu_ms = filetime_ms(kernel) + filetime_ms(user);
    if (GetThreadTimes(GetCurrentThread(), &create, &exit_, &kernel, &user)) s.thread_cpu_ms = filetime_ms(kernel) + filetime_ms(user);
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles)) s.thread_cycles = cycles;
    return s;
}

CpuUsage cpu_usage_between(const ProcessSample& a, const ProcessSample& b, uint32_t logical) {
    CpuUsage u;
    const double wall = b.wall_ms - a.wall_ms;
    if (wall <= 0) return u;
    u.cores = (b.process_cpu_ms - a.process_cpu_ms) / wall;
    u.thread_cores = (b.thread_cpu_ms - a.thread_cpu_ms) / wall;
    u.machine_fraction = logical ? u.cores / double(logical) : 0;
    return u;
}

MemorySample sample_memory(MemoryDetail detail) {
    MemorySample m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        m.valid = true;
        m.working_set = pmc.WorkingSetSize;
        m.peak_working_set = pmc.PeakWorkingSetSize;
        m.private_bytes = pmc.PrivateUsage;
        m.pagefile_usage = pmc.PagefileUsage;
    }
    if (detail == MemoryDetail::Quick) return m;
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) m.handle_count = handles;
    m.gdi_objects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    m.user_objects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof te;
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) if (te.th32OwnerProcessID == pid) ++m.threads;
        CloseHandle(snap);
    }
    return m;
}

// ---- PDH: GPU engines and thermal zones ---------------------------------------------------------------------------------------------------

namespace {

struct CounterItem { std::wstring name; double value; };

bool read_counter_array(PDH_HCOUNTER counter, std::vector<CounterItem>& out) {
    out.clear();
    DWORD size = 0, count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (DWORD(st) != DWORD(PDH_MORE_DATA) || size == 0) return false;
    std::unique_ptr<uint8_t[]> buffer(new uint8_t[size]);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.get());
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
    if (st != ERROR_SUCCESS) return false;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) out.push_back({items[i].szName, items[i].FmtValue.doubleValue});
    return true;
}

std::wstring engine_type_of(const std::wstring& instance) {
    const size_t k = instance.rfind(L"engtype_");
    if (k == std::wstring::npos) return {};
    std::wstring t = instance.substr(k + 8);
    const size_t us = t.find(L'_');
    return us == std::wstring::npos ? t : t.substr(0, us);
}

}  // namespace

struct GpuEngineSampler::Impl {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER util = nullptr, dedicated = nullptr, shared = nullptr;
    std::wstring pid_tag;
};

GpuEngineSampler::GpuEngineSampler() : impl_(new Impl) { impl_->pid_tag = L"pid_" + std::to_wstring(GetCurrentProcessId()) + L"_"; }
GpuEngineSampler::~GpuEngineSampler() {
    if (impl_->query) PdhCloseQuery(impl_->query);
    delete impl_;
}
bool GpuEngineSampler::available() const { return impl_->util != nullptr; }

Status GpuEngineSampler::open() {
    if (impl_->query) return Status::success();
    if (PdhOpenQueryW(nullptr, 0, &impl_->query) != ERROR_SUCCESS) return Status::fail("PdhOpenQuery", ERROR_NOT_SUPPORTED);
    // English counter names: the localised paths differ per Windows language.
    if (PdhAddEnglishCounterW(impl_->query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &impl_->util) != ERROR_SUCCESS) {
        impl_->util = nullptr;
        return Status::fail("GPU Engine performance counters not available", ERROR_NOT_SUPPORTED);
    }
    PdhAddEnglishCounterW(impl_->query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &impl_->dedicated);
    PdhAddEnglishCounterW(impl_->query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &impl_->shared);
    PdhCollectQueryData(impl_->query);   // first collection: rates need two
    return Status::success();
}

GpuEngineSampler::Sample GpuEngineSampler::sample() {
    Sample s;
    if (!impl_->query || !impl_->util) return s;
    if (PdhCollectQueryData(impl_->query) != ERROR_SUCCESS) return s;
    std::vector<CounterItem> items;
    if (read_counter_array(impl_->util, items)) {
        s.valid = true;
        for (const auto& it : items) {
            if (it.name.find(impl_->pid_tag) == std::wstring::npos) continue;
            ++s.instances;
            const std::wstring type = engine_type_of(it.name);
            if (type == L"3D") s.engine_3d_percent += it.value;
            else if (type == L"Copy") s.engine_copy_percent += it.value;
            else if (type == L"Compute") s.engine_compute_percent += it.value;
            else if (type.rfind(L"Video", 0) == 0) s.engine_video_percent += it.value;
            s.any_engine_percent = std::max(s.any_engine_percent, it.value);
        }
    }
    if (impl_->dedicated && read_counter_array(impl_->dedicated, items))
        for (const auto& it : items) if (it.name.find(impl_->pid_tag) != std::wstring::npos) s.dedicated_bytes += uint64_t(it.value);
    if (impl_->shared && read_counter_array(impl_->shared, items))
        for (const auto& it : items) if (it.name.find(impl_->pid_tag) != std::wstring::npos) s.shared_bytes += uint64_t(it.value);
    return s;
}

std::vector<ThermalZone> read_thermal_zones() {
    std::vector<ThermalZone> zones;
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return zones;
    PDH_HCOUNTER temp = nullptr, limit = nullptr;
    if (PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\Temperature", 0, &temp) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\% Passive Limit", 0, &limit);
        if (PdhCollectQueryData(q) == ERROR_SUCCESS) {
            std::vector<CounterItem> t, l;
            if (read_counter_array(temp, t)) {
                if (limit) read_counter_array(limit, l);
                for (const auto& it : t) {
                    ThermalZone z;
                    z.name = to_utf8(it.name);
                    z.celsius = it.value - 273.15;   // the counter is in Kelvin
                    for (const auto& li : l) if (li.name == it.name) z.passive_limit_percent = li.value;
                    zones.push_back(z);
                }
            }
        }
    }
    PdhCloseQuery(q);
    return zones;
}

// ---- Frame statistics ------------------------------------------------------------------------------------------------------------------------

void FrameStats::add(double ms) {
    if (samples_.size() < window_) samples_.push_back(ms);
    else { samples_[next_] = ms; next_ = (next_ + 1) % window_; }
}

FrameStats::Result FrameStats::compute() const {
    Result r;
    r.count = samples_.size();
    if (samples_.empty()) return r;
    std::vector<double> v = samples_;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double d : v) sum += d;
    r.avg = sum / double(v.size());
    r.min = v.front();
    r.max = v.back();
    auto pct = [&](double p) { return v[std::min<size_t>(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))]; };
    r.p50 = pct(0.50);
    r.p95 = pct(0.95);
    r.p99 = pct(0.99);
    r.fps = r.avg > 0 ? 1000.0 / r.avg : 0;
    const size_t worst = std::max<size_t>(1, v.size() / 100);
    double worst_sum = 0;
    for (size_t i = v.size() - worst; i < v.size(); ++i) worst_sum += v[i];
    const double worst_avg = worst_sum / double(worst);
    r.one_percent_low_fps = worst_avg > 0 ? 1000.0 / worst_avg : 0;
    return r;
}

}  // namespace nue::win
