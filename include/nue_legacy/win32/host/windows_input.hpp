// platform::Input on a Windows host, by polling: the keyboard through GetAsyncKeyState, the mouse by the movement of the cursor between two pumps and the
// state of its buttons, game controllers through XInput (loaded when present). It is the input of the DirectInput part of the Win32 facade on Windows.
// Limitations (the reason a native backend such as NUE's is better): the cursor cannot move past the screen edge, the wheel is not seen, and keys are sampled at
// the pump rate. It has not been checked against a real device by this project's tests: they use the scripted null input.
#pragma once
#include "nue_legacy/platform/input.hpp"
#include <memory>

namespace nue::legacy::win32::host {
std::unique_ptr<platform::Input> make_windows_input();
}
