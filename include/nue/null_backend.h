/*
 * NUE engine - control surface of the NULL (headless) backend, for tests and tools.
 *
 * The null backend implements the whole API without any window, display or audio device: one virtual 1920x1080 display, windows are
 * bookkeeping objects, audio streams are consumed by a virtual clock, input is whatever the test injects. With NUE_INIT_DETERMINISTIC the
 * clock only moves when the test advances it, so runs are repeatable bit for bit.
 * All functions fail with NUE_ERR_UNSUPPORTED unless the active backend is the null backend.
 */
#ifndef NUE_NULL_BACKEND_H
#define NUE_NULL_BACKEND_H

#include "nue/app.h"
#include "nue/audio.h"
#include "nue/input.h"
#include "nue/window.h"
#include "nue/power.h"
#include "nue/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

NUE_API nue_result NUE_CALL nue_null_advance_time(int64_t ns);                  /* deterministic mode: moves nue_time_ns() and consumes audio */
NUE_API nue_result NUE_CALL nue_null_inject_event(const nue_event* event);       /* goes through the normal producer path (state tables, watchers, queue) */
NUE_API nue_result NUE_CALL nue_null_press_key(nue_scancode scancode, nue_bool down);
NUE_API nue_result NUE_CALL nue_null_move_mouse(float x, float y);
NUE_API nue_result NUE_CALL nue_null_press_mouse(nue_mouse_button button, nue_bool down);
NUE_API nue_result NUE_CALL nue_null_add_gamepad(const char* name, nue_gamepad_type type, nue_gamepad_id* out_id);
NUE_API nue_result NUE_CALL nue_null_remove_gamepad(nue_gamepad_id id);
NUE_API nue_result NUE_CALL nue_null_set_gamepad_button(nue_gamepad_id id, nue_gamepad_button button, nue_bool down);
NUE_API nue_result NUE_CALL nue_null_set_gamepad_axis(nue_gamepad_id id, nue_gamepad_axis axis, int16_t value);
NUE_API nue_result NUE_CALL nue_null_touch(uint64_t finger_id, float x, float y, int32_t phase /* 0 down, 1 move, 2 up */);
NUE_API nue_result NUE_CALL nue_null_set_app_state(nue_app_state state);         /* produces the PAUSING/PAUSED/RESUMING/RESUMED sequence */
NUE_API nue_result NUE_CALL nue_null_set_thermal_state(nue_thermal_state state);
NUE_API nue_result NUE_CALL nue_null_set_power(nue_power_state state, int32_t battery_percent);
/* Simulate the machine's memory (budgets resolve against it) and the OS signals around it. Pass NULL to go back to the real machine. */
NUE_API nue_result NUE_CALL nue_null_set_memory(const nue_memory_info* info_or_null);
NUE_API nue_result NUE_CALL nue_null_signal_memory_pressure(nue_memory_pressure level, uint64_t available_bytes);
NUE_API nue_result NUE_CALL nue_null_system_suspend(nue_bool suspending);          /* NUE_EVENT_SYSTEM_SUSPENDING / NUE_EVENT_SYSTEM_RESUMED */
NUE_API nue_result NUE_CALL nue_null_audio_interruption(nue_bool begin);           /* NUE_EVENT_AUDIO_INTERRUPTED / RESUMED */
NUE_API nue_result NUE_CALL nue_null_set_display(uint32_t width, uint32_t height, float refresh_hz, float content_scale);
NUE_API nue_result NUE_CALL nue_null_resize_window(nue_window window, int32_t width, int32_t height);   /* as if the user dragged the border */
NUE_API nue_result NUE_CALL nue_null_request_close(nue_window window);
/* Audio: when the virtual clock runs, streams consume frames at their sample rate. A stream keeps the last `capture_frames`
 * frames the virtual device "played" (set 0 to disable); this reads them (oldest first) in the stream's own format. */
NUE_API nue_result NUE_CALL nue_null_audio_set_capture(nue_audio_stream stream, uint32_t capture_frames);
NUE_API nue_result NUE_CALL nue_null_audio_read_played(nue_audio_stream stream, void* out, uint32_t max_frames, uint32_t* out_frames);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* NUE_NULL_BACKEND_H */
