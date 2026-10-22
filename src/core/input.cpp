// Input functions that forward to the backend: gamepads/joysticks, sensors, text input, haptics, device presence.
#include "internal/core_internal.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

const nue_backend_input* inp() {
    const nue_backend* b = nuei::backend();
    return b ? b->input : nullptr;
}

#define NO_INPUT_OR_RETURN(i) \
    const nue_backend_input* i = inp(); \
    if (!i) return nuei::fail(NUE_ERR_UNSUPPORTED, "this backend has no input support")

nue_result copy_struct_out(const void* src, size_t src_size, void* dst, uint32_t* dst_struct_size_field) {
    uint32_t keep = *dst_struct_size_field;
    std::memcpy(dst, src, keep < src_size ? keep : src_size);
    *dst_struct_size_field = keep;
    return NUE_OK;
}

}  // namespace

extern "C" {

NUE_API nue_result NUE_CALL nue_gamepad_list(nue_gamepad_id* ids, uint32_t capacity, uint32_t* out_count) {
    NO_INPUT_OR_RETURN(i);
    if (!i->gamepad_enum) { if (out_count) *out_count = 0; return NUE_OK; }
    uint32_t count = 0;
    nue_gamepad_info probe;
    NUE_STRUCT_INIT(&probe);
    nue_result r = i->gamepad_enum(&probe, 0, &count);
    if (NUE_FAILED(r)) return r;
    std::vector<nue_gamepad_info> v(count);
    for (auto& g : v) NUE_STRUCT_INIT(&g);
    uint32_t got = 0;
    if (count) { r = i->gamepad_enum(v.data(), count, &got); if (NUE_FAILED(r)) return r; v.resize(std::min(got, count)); }
    if (out_count) *out_count = uint32_t(v.size());
    for (uint32_t k = 0; ids && k < capacity && k < v.size(); ++k) ids[k] = v[k].id;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gamepad_get_info(nue_gamepad_id id, nue_gamepad_info* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    NO_INPUT_OR_RETURN(i);
    if (!i->gamepad_enum) return NUE_ERR_NOT_FOUND;
    uint32_t count = 0;
    nue_gamepad_info probe;
    NUE_STRUCT_INIT(&probe);
    i->gamepad_enum(&probe, 0, &count);
    std::vector<nue_gamepad_info> v(count);
    for (auto& g : v) NUE_STRUCT_INIT(&g);
    uint32_t got = 0;
    if (count) { i->gamepad_enum(v.data(), count, &got); v.resize(std::min(got, count)); }
    for (auto& g : v) if (g.id == id) {
        if (nuei::glyph_override() >= 0) g.glyphs = nue_glyph_family(nuei::glyph_override());
        return copy_struct_out(&g, sizeof(g), out, &out->struct_size);
    }
    return nuei::fail(NUE_ERR_NOT_FOUND, "gamepad %u is not connected", id);
}

NUE_API nue_result NUE_CALL nue_gamepad_get_state(nue_gamepad_id id, nue_gamepad_state* out) {
    if (!out || out->struct_size < sizeof(uint32_t) * 2) return NUE_ERR_INVALID_ARGUMENT;
    NO_INPUT_OR_RETURN(i);
    if (!i->gamepad_state) return NUE_ERR_UNSUPPORTED;
    nue_gamepad_state s;
    NUE_STRUCT_INIT(&s);
    nue_result r = i->gamepad_state(id, &s);
    if (NUE_FAILED(r)) return r;
    return copy_struct_out(&s, sizeof(s), out, &out->struct_size);
}

NUE_API nue_result NUE_CALL nue_gamepad_add_mapping(const char* line) {
    if (!line || !*line) return NUE_ERR_INVALID_ARGUMENT;
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_add_mapping ? i->gamepad_add_mapping(line) : NUE_ERR_UNSUPPORTED;
}

NUE_API nue_result NUE_CALL nue_gamepad_add_mappings_from_text(const char* text, size_t length, uint32_t* out_added) {
    if (!text) return NUE_ERR_INVALID_ARGUMENT;
    NO_INPUT_OR_RETURN(i);
    if (!i->gamepad_add_mapping) return NUE_ERR_UNSUPPORTED;
    uint32_t added = 0;
    std::string all(text, length);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        std::string line = all.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? all.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (NUE_SUCCEEDED(i->gamepad_add_mapping(line.c_str()))) ++added;
    }
    if (out_added) *out_added = added;
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_gamepad_add_mappings_from_file(const char* path, uint32_t* out_added) {
    void* data = nullptr;
    size_t size = 0;
    nue_result r = nue_file_load(path, &data, &size);
    if (NUE_FAILED(r)) return r;
    r = nue_gamepad_add_mappings_from_text(static_cast<const char*>(data), size, out_added);
    nue_free(data);
    return r;
}

NUE_API nue_result NUE_CALL nue_gamepad_get_mapping(nue_gamepad_id id, char* buffer, size_t capacity, size_t* out_size) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_get_mapping ? i->gamepad_get_mapping(id, buffer, capacity, out_size) : NUE_ERR_UNSUPPORTED;
}

NUE_API nue_result NUE_CALL nue_gamepad_set_player_index(nue_gamepad_id id, int32_t index) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_set_player_index ? i->gamepad_set_player_index(id, index) : NUE_ERR_UNSUPPORTED;
}

NUE_API nue_result NUE_CALL nue_gamepad_rumble(nue_gamepad_id id, uint16_t low, uint16_t high, uint32_t ms) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_rumble ? i->gamepad_rumble(id, low, high, ms) : NUE_ERR_UNSUPPORTED;
}
NUE_API nue_result NUE_CALL nue_gamepad_rumble_triggers(nue_gamepad_id id, uint16_t l, uint16_t r, uint32_t ms) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_rumble_triggers ? i->gamepad_rumble_triggers(id, l, r, ms) : NUE_ERR_UNSUPPORTED;
}
NUE_API nue_result NUE_CALL nue_gamepad_set_led(nue_gamepad_id id, uint8_t r, uint8_t g, uint8_t b) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_led ? i->gamepad_led(id, r, g, b) : NUE_ERR_UNSUPPORTED;
}
NUE_API nue_result NUE_CALL nue_gamepad_set_sensor_enabled(nue_gamepad_id id, uint32_t kind, nue_bool enabled) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_sensor_enable ? i->gamepad_sensor_enable(id, kind, enabled) : NUE_ERR_UNSUPPORTED;
}
NUE_API nue_result NUE_CALL nue_gamepad_get_sensor(nue_gamepad_id id, uint32_t kind, float out_values[3]) {
    NO_INPUT_OR_RETURN(i);
    return i->gamepad_sensor_get ? i->gamepad_sensor_get(id, kind, out_values) : NUE_ERR_UNSUPPORTED;
}

NUE_API int16_t NUE_CALL nue_joystick_get_axis(nue_gamepad_id id, uint32_t axis) { const nue_backend_input* i = inp(); return i && i->joystick_axis ? i->joystick_axis(id, axis) : 0; }
NUE_API nue_bool NUE_CALL nue_joystick_get_button(nue_gamepad_id id, uint32_t button) { const nue_backend_input* i = inp(); return i && i->joystick_button ? i->joystick_button(id, button) : NUE_FALSE; }
NUE_API uint8_t NUE_CALL nue_joystick_get_hat(nue_gamepad_id id, uint32_t hat) { const nue_backend_input* i = inp(); return i && i->joystick_hat ? i->joystick_hat(id, hat) : 0; }

NUE_API nue_bool NUE_CALL nue_touch_is_present(void) { const nue_backend_input* i = inp(); return i && i->touch_present ? i->touch_present() : NUE_FALSE; }
NUE_API nue_bool NUE_CALL nue_mouse_is_present(void) { const nue_backend_input* i = inp(); return i && i->mouse_present ? i->mouse_present() : NUE_FALSE; }

NUE_API nue_bool NUE_CALL nue_sensor_is_available(nue_sensor_kind kind) { const nue_backend_input* i = inp(); return i && i->sensor_available ? i->sensor_available(kind) : NUE_FALSE; }
NUE_API nue_result NUE_CALL nue_sensor_set_enabled(nue_sensor_kind kind, nue_bool enabled, float rate_hz) {
    NO_INPUT_OR_RETURN(i);
    return i->sensor_enable ? i->sensor_enable(kind, enabled, rate_hz) : NUE_ERR_UNSUPPORTED;
}
NUE_API nue_result NUE_CALL nue_sensor_get(nue_sensor_kind kind, float out_values[3], int64_t* out_timestamp_ns) {
    NO_INPUT_OR_RETURN(i);
    return i->sensor_get ? i->sensor_get(kind, out_values, out_timestamp_ns) : NUE_ERR_UNSUPPORTED;
}

NUE_API nue_result NUE_CALL nue_text_input_show_screen_keyboard(nue_window window, nue_bool show) {
    nue_native_window nat;
    NUE_STRUCT_INIT(&nat);
    nue_result r = nue_window_get_native(window, &nat);
    if (NUE_FAILED(r)) return r;
    const nue_platform_ops* p = nuei::platform();
    auto fn = p ? NUE_PLATFORM_ENTRY(p, screen_keyboard) : nullptr;
    if (fn) return fn(&nat, show);
    const nue_backend* b = nuei::backend();
    if (b && b->input && b->input->screen_keyboard && b->input->screen_keyboard() && b->video && b->video->window_text_input)
        return b->video->window_text_input(nuei::window_impl_or_null(window), show, nullptr, 0);
    return show ? NUE_ERR_UNSUPPORTED : NUE_OK_NOOP;
}

NUE_API nue_bool NUE_CALL nue_text_input_has_screen_keyboard(void) { const nue_backend_input* i = inp(); return i && i->screen_keyboard ? i->screen_keyboard() : NUE_FALSE; }

NUE_API nue_bool NUE_CALL nue_haptics_is_available(void) {
    const nue_platform_ops* p = nuei::platform();
    if (p && p->haptics_available && p->haptics_available()) return NUE_TRUE;
    const nue_backend_input* i = inp();
    return i && i->haptics_available ? i->haptics_available() : NUE_FALSE;
}
NUE_API nue_result NUE_CALL nue_haptics_vibrate(uint32_t duration_ms, float intensity) {
    const nue_platform_ops* p = nuei::platform();
    if (p && p->haptics_vibrate && p->haptics_available && p->haptics_available()) return p->haptics_vibrate(duration_ms, intensity);
    NO_INPUT_OR_RETURN(i);
    return i->haptics_vibrate ? i->haptics_vibrate(duration_ms, intensity) : NUE_ERR_UNSUPPORTED;
}

// text input state per window: tracked in the core so nue_text_input_is_active works on every backend
}  // extern "C"

namespace {
struct TextState { std::mutex m; std::vector<nue_window_id> active; };
TextState& ts() { static TextState* t = new TextState(); return *t; }
}  // namespace

extern "C" {

NUE_API nue_result NUE_CALL nue_text_input_start(nue_window window) {
    nue_window_id id = nue_window_get_id(window);
    if (!id) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    const nue_backend* b = nuei::backend();
    if (b && b->video && b->video->window_text_input) {
        nue_result r = b->video->window_text_input(nuei::window_impl_or_null(window), NUE_TRUE, nullptr, 0);
        if (NUE_FAILED(r)) return r;
    }
    TextState& t = ts();
    std::lock_guard<std::mutex> lock(t.m);
    if (std::find(t.active.begin(), t.active.end(), id) == t.active.end()) t.active.push_back(id);
    return NUE_OK;
}

NUE_API nue_result NUE_CALL nue_text_input_stop(nue_window window) {
    nue_window_id id = nue_window_get_id(window);
    if (!id) return nuei::fail(NUE_ERR_INVALID_HANDLE, "invalid window handle");
    const nue_backend* b = nuei::backend();
    if (b && b->video && b->video->window_text_input) b->video->window_text_input(nuei::window_impl_or_null(window), NUE_FALSE, nullptr, 0);
    TextState& t = ts();
    std::lock_guard<std::mutex> lock(t.m);
    t.active.erase(std::remove(t.active.begin(), t.active.end(), id), t.active.end());
    return NUE_OK;
}

NUE_API nue_bool NUE_CALL nue_text_input_is_active(nue_window window) {
    nue_window_id id = nue_window_get_id(window);
    TextState& t = ts();
    std::lock_guard<std::mutex> lock(t.m);
    return id && std::find(t.active.begin(), t.active.end(), id) != t.active.end() ? NUE_TRUE : NUE_FALSE;
}

NUE_API nue_result NUE_CALL nue_text_input_set_area(nue_window window, const nue_rect_i* rect, int32_t cursor) {
    const nue_backend* b = nuei::backend();
    if (!b || !b->video || !b->video->window_text_input) return NUE_ERR_UNSUPPORTED;
    if (!nue_text_input_is_active(window)) return nuei::fail(NUE_ERR_BAD_STATE, "text input is not active on this window");
    return b->video->window_text_input(nuei::window_impl_or_null(window), NUE_TRUE, rect, cursor);
}

}  // extern "C"
