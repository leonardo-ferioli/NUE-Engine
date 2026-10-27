// The portable input of a host that has an event pump: a backend pushes InputEvents, the snapshots and the listeners follow at the next pump.
#include "nue_legacy/platform/null_services.hpp"
#include "test_support.hpp"
#include <vector>

using namespace nue::legacy::platform;

static void test_event_input() {
    null::NullInput input;
    std::vector<InputEvent> seen;
    const uint32_t listener = input.add_listener([&](const InputEvent& e) { seen.push_back(e); });
    auto event = [](InputEventKind kind, uint32_t code, int32_t value, int32_t value2 = 0, GamepadId pad = 0) { InputEvent e; e.kind = kind; e.code = code; e.value = value; e.value2 = value2; e.pad = pad; e.time_ns = 1000; return e; };
    input.push(event(InputEventKind::key, 4, 1));                       // HID usage 4 = the A key
    input.push(event(InputEventKind::mouse_motion, 0, 5, -3));
    input.push(event(InputEventKind::mouse_button, kMouseRight, 1));
    CHECK(input.key_state()[4] == 0);                                   // nothing changes before the pump
    input.pump();
    CHECK(input.key_state()[4] == 1 && input.key_state()[5] == 0);
    float x = 0, y = 0, dx = 0, dy = 0;
    CHECK_EQ(input.mouse_state(&x, &y), 1u << (kMouseRight - 1));
    CHECK(x == 5 && y == -3);
    input.mouse_relative(&dx, &dy);
    CHECK(dx == 5 && dy == -3);
    input.mouse_relative(&dx, &dy);
    CHECK(dx == 0 && dy == 0);                                          // relative motion is consumed
    const GamepadId pad = input.add_pad(GamepadInfo{0, "Test pad", 1, 2, 6, 12, 1, true, true});
    input.push(event(InputEventKind::pad_axis, 0, -20000, 0, pad));
    input.push(event(InputEventKind::pad_button, 3, 1, 0, pad));
    input.push(event(InputEventKind::pad_hat, 0, 2, 0, pad));
    input.pump();
    GamepadState state;
    CHECK(input.gamepad_state(pad, state));
    CHECK(state.axes[0] == -20000 && (state.buttons & 8) && state.hats[0] == 2);
    CHECK_EQ(input.gamepads().size(), 1u);
    input.push(event(InputEventKind::key, 4, 0));
    input.pump();
    CHECK(input.key_state()[4] == 0);
    CHECK_EQ(seen.size(), 8u);                                          // every listener sees every event, in order (add_pad announces the pad as an event)
    CHECK(seen[0].kind == InputEventKind::key && seen[1].kind == InputEventKind::mouse_motion && seen[3].kind == InputEventKind::pad_added && seen[7].kind == InputEventKind::key);
    input.remove_listener(listener);
    input.push(event(InputEventKind::key, 5, 1));
    input.pump();
    CHECK_EQ(seen.size(), 8u);
    CHECK(input.key_state()[5] == 1);
    input.push(event(InputEventKind::key, 9999, 1));                    // an out-of-range scancode is ignored, not a crash
    input.pump();
}
REGISTER_TEST("event_input", test_event_input);
