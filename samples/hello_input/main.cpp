// hello_input: prints keyboard / mouse / gamepad / touch events and which device the player used last. It needs a window for keyboard focus,
// so it opens a small one: never run by the tests. Quit with Escape or by closing the window.
#include "nuepp/nue.hpp"

#include <cstdio>

int main() {
    nue_init_desc d = nuepp::init<nue_init_desc>();
    d.app_name = "NUE hello_input"; d.app_id = "hello-input";
    nuepp::Context ctx(d);
    if (!ctx.ok()) { std::fprintf(stderr, "init: %s\n", ctx.result().describe().c_str()); return 1; }
    nue_window_desc wd = nuepp::init<nue_window_desc>();
    wd.title = "NUE hello_input"; wd.width = 640; wd.height = 360;
    nuepp::Window window(wd);
    if (!window.ok()) { std::fprintf(stderr, "window: %s\n", window.result().describe().c_str()); return 1; }
    static const char* const kinds[] = {"none", "keyboard+mouse", "gamepad", "touch"};
    static const char* const glyphs[] = {"keyboard", "xbox", "playstation", "nintendo", "generic pad", "touch", "steam"};
    bool run = true;
    while (run) {
        nuepp::events::pump();
        nuepp::events::drain([&](const nue_event& e) {
            switch (e.type) {
                case NUE_EVENT_QUIT: case NUE_EVENT_WINDOW_CLOSE_REQUESTED: run = false; break;
                case NUE_EVENT_KEY_DOWN:
                    std::printf("key down: %s%s\n", nue_scancode_name(e.payload.key.scancode), e.payload.key.repeat ? " (repeat)" : "");
                    if (e.payload.key.scancode == NUE_SC_ESCAPE) run = false;
                    break;
                case NUE_EVENT_MOUSE_BUTTON_DOWN: std::printf("mouse button %d at %.0f,%.0f\n", e.payload.button.button, double(e.payload.button.x), double(e.payload.button.y)); break;
                case NUE_EVENT_GAMEPAD_ADDED: std::printf("gamepad %u connected\n", e.payload.gamepad.gamepad_id); break;
                case NUE_EVENT_GAMEPAD_REMOVED: std::printf("gamepad %u removed\n", e.payload.gamepad.gamepad_id); break;
                case NUE_EVENT_GAMEPAD_BUTTON_DOWN:
                    std::printf("pad %u button %s\n", e.payload.gamepad.gamepad_id,
                                nue_gamepad_button_name(nue_gamepad_button(e.payload.gamepad.control), nuepp::input::last_device().glyphs));
                    break;
                case NUE_EVENT_FINGER_DOWN: std::printf("finger %llu at %.2f,%.2f\n", (unsigned long long)e.payload.finger.finger_id, double(e.payload.finger.x), double(e.payload.finger.y)); break;
                case NUE_EVENT_INPUT_DEVICE_CHANGED:
                    std::printf("last used device: %s (show %s glyphs)\n", kinds[e.payload.input_device.device_kind % 4], glyphs[e.payload.input_device.glyph_family % 7]);
                    break;
                default: break;
            }
            return true;
        });
        nuepp::sleep_ns(2000000, false);
    }
    return 0;
}
