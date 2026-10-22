// Internal declarations shared by the core's translation units (not installed).
#pragma once

#include "nue/nue.h"
#include "internal/backend.h"
#include "internal/platform_ops.h"

#include <string>
#include <vector>

namespace nuei {

// ---- context ---------------------------------------------------------------------------------------------------------------
struct InitInfo {
    std::string app_name, app_id, org_name, app_version;
    std::string game_data_path, user_data_path, config_file, log_file;
    nue_backend_kind backend = NUE_BACKEND_AUTO;
    nue_loop_model loop_model = NUE_LOOP_HOST;
    nue_subsystem_mask subsystems = NUE_SUBSYSTEM_ALL_DEFAULT;
    uint32_t flags = 0;
    uint32_t log_level = NUE_LOG_INFO;
    uint32_t event_queue_capacity = 4096;
    nue_init_desc desc{};  // raw copy (pointers inside are not valid after nue_init returns; use the strings above)
};

bool initialized();
const InitInfo& init_info();
const nue_backend* backend();         // nullptr before init
const nue_platform_ops* platform();   // never nullptr
bool subsystem_enabled(nue_subsystem_mask mask);
bool headless();
bool deterministic();

// Records `message` as the thread's last error text and returns `code`.
nue_result fail(nue_result code, const char* fmt, ...) NUE_PRINTF_LIKE(2, 3);
nue_result require_init();            // NUE_OK or NUE_ERR_NOT_INITIALIZED (with message)

// Writes `text` into a caller buffer following the NUE_STRING_OUT convention. Returns NUE_OK.
nue_result copy_out_string(const char* text, size_t length, char* buffer, size_t capacity, size_t* out_size);
inline nue_result copy_out_string(const std::string& s, char* buffer, size_t capacity, size_t* out_size) {
    return copy_out_string(s.c_str(), s.size(), buffer, capacity, out_size);
}

// ---- allocation ------------------------------------------------------------------------------------------------------------
void* alloc_mem(size_t size, size_t alignment = 0);
void free_mem(void* p);

// ---- time ------------------------------------------------------------------------------------------------------------------
int64_t now_ns();                     // honours the backend clock (deterministic mode)
uint64_t current_thread_id();
void set_thread_name(const char* name);

// ---- subsystem life cycle (called by context.cpp in this order / reverse) ----------------------------------------------------
void log_init(const InitInfo& info);
void log_open_file_sink(const InitInfo& info);
void log_shutdown();

void config_init();
void config_shutdown();

nue_result events_init(uint32_t capacity);
void events_shutdown();

void vfs_init(const InitInfo& info);
void vfs_shutdown();

void io_shutdown();
void audio_shutdown();                // streams
void mixer_shutdown_all();
void windows_shutdown();              // destroys windows still open
void gfx_shutdown_all();
void power_init();
void power_shutdown();
void power_poll_and_notify();         // compare power/thermal/game-mode state with the last poll and raise events
void diag_init(const InitInfo& info);
void diag_shutdown();
void extensions_shutdown();


// ---- events / input state (events.cpp, input_state.cpp) -----------------------------------------------------------------------
// Updates the polled-state tables from an event; may append derived events (INPUT_DEVICE_CHANGED) to `derived`.
void input_on_event(const nue_event& event, std::vector<nue_event>& derived);
void input_state_reset();
void windows_after_pump();            // refresh window snapshots after the backend pumped
void* window_impl_or_null(nue_window w);   // backend private pointer of a live window, else nullptr
void windows_add_destroy_hook(void (*fn)(nue_window));   // called before a window is destroyed (gfx contexts bound to it let go)

// ---- storage helpers shared with log/config/diag ----------------------------------------------------------------------------
// Resolve `path` (VFS or native) to a native path for writing. Empty on failure.
std::string resolve_native_path(const char* path, bool for_write);

}  // namespace nuei
