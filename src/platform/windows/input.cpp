#include "nue_win/input.hpp"

#include <unknwn.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace nue::win {

// ---- Raw Input ---------------------------------------------------------------------------------------------------------------------------

Status RawInput::register_devices(HWND hwnd, const Options& o) {
    unregister_all();
    std::vector<RAWINPUTDEVICE> list;
    DWORD common = (o.background ? RIDEV_INPUTSINK : 0);
    if (o.mouse) list.push_back({0x01, 0x02, common | (o.no_legacy ? RIDEV_NOLEGACY : 0) | (o.capture_mouse ? RIDEV_CAPTUREMOUSE : 0), hwnd});
    if (o.keyboard) list.push_back({0x01, 0x06, common | (o.no_legacy ? RIDEV_NOLEGACY : 0), hwnd});
    if (o.gamepads) {
        list.push_back({0x01, 0x05, common, hwnd});   // game pad
        list.push_back({0x01, 0x04, common, hwnd});   // joystick
    }
    if (list.empty()) return Status::success();
    if (!RegisterRawInputDevices(list.data(), UINT(list.size()), sizeof(RAWINPUTDEVICE))) return Status::last_error("RegisterRawInputDevices");
    registered_ = true;
    options_ = o;
    hwnd_ = hwnd;
    std::fill(std::begin(key_down_), std::end(key_down_), false);
    acc_dx_ = acc_dy_ = 0;
    have_last_abs_ = false;
    return Status::success();
}

void RawInput::unregister_all() {
    if (!registered_) return;
    std::vector<RAWINPUTDEVICE> list;
    auto add = [&](USHORT page, USHORT usage) { list.push_back({page, usage, RIDEV_REMOVE, nullptr}); };
    if (options_.mouse) add(1, 2);
    if (options_.keyboard) add(1, 6);
    if (options_.gamepads) { add(1, 5); add(1, 4); }
    RegisterRawInputDevices(list.data(), UINT(list.size()), sizeof(RAWINPUTDEVICE));
    registered_ = false;
}

bool RawInput::handle_wm_input(LPARAM lparam, RawInputEvent& out) {
    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || size == 0) return false;
    std::unique_ptr<uint64_t[]> storage(new uint64_t[(size + 7) / 8]);   // 8-byte aligned, as RAWINPUT requires
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, storage.get(), &size, sizeof(RAWINPUTHEADER)) != size) return false;
    return decode(*reinterpret_cast<const RAWINPUT*>(storage.get()), out);
}

bool RawInput::decode(const RAWINPUT& raw, RawInputEvent& out) {
    out = RawInputEvent{};
    out.device = raw.header.hDevice;
    if (raw.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = raw.data.mouse;
        out.kind = RawInputEvent::Mouse;
        RawMouseEvent& e = out.mouse;
        e.absolute = (m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0;
        e.virtual_desktop = (m.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        if (e.absolute) {   // remote desktop, pen, touch screens: report the delta so callers can treat every mouse alike
            if (have_last_abs_) { e.dx = m.lLastX - last_abs_x_; e.dy = m.lLastY - last_abs_y_; }
            last_abs_x_ = m.lLastX;
            last_abs_y_ = m.lLastY;
            have_last_abs_ = true;
        } else {
            e.dx = m.lLastX;
            e.dy = m.lLastY;
        }
        acc_dx_ += e.dx;
        acc_dy_ += e.dy;
        const USHORT f = m.usButtonFlags;
        if (f & RI_MOUSE_LEFT_BUTTON_DOWN) e.button_down |= 1u << 0;
        if (f & RI_MOUSE_LEFT_BUTTON_UP) e.button_up |= 1u << 0;
        if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) e.button_down |= 1u << 1;
        if (f & RI_MOUSE_RIGHT_BUTTON_UP) e.button_up |= 1u << 1;
        if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) e.button_down |= 1u << 2;
        if (f & RI_MOUSE_MIDDLE_BUTTON_UP) e.button_up |= 1u << 2;
        if (f & RI_MOUSE_BUTTON_4_DOWN) e.button_down |= 1u << 3;
        if (f & RI_MOUSE_BUTTON_4_UP) e.button_up |= 1u << 3;
        if (f & RI_MOUSE_BUTTON_5_DOWN) e.button_down |= 1u << 4;
        if (f & RI_MOUSE_BUTTON_5_UP) e.button_up |= 1u << 4;
        if (f & RI_MOUSE_WHEEL) e.wheel = int16_t(m.usButtonData);
        if (f & RI_MOUSE_HWHEEL) e.hwheel = int16_t(m.usButtonData);
        return true;
    }
    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& k = raw.data.keyboard;
        if (k.VKey == 0xFF) return false;   // "fake" key from escape sequences (e.g. Pause, Shift with NumLock): not a physical event
        out.kind = RawInputEvent::Key;
        RawKeyEvent& e = out.key;
        e.down = (k.Flags & RI_KEY_BREAK) == 0;
        e.extended = (k.Flags & RI_KEY_E0) != 0;
        e.scancode = uint16_t(k.MakeCode | (e.extended ? 0xE000 : 0) | ((k.Flags & RI_KEY_E1) ? 0xE100 : 0));
        uint16_t vk = k.VKey;
        if (vk == VK_SHIFT) vk = uint16_t(MapVirtualKeyW(k.MakeCode, MAPVK_VSC_TO_VK_EX));
        else if (vk == VK_CONTROL) vk = e.extended ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = e.extended ? VK_RMENU : VK_LMENU;
        e.vkey = vk;
        if (vk < 256) {
            e.repeat = e.down && key_down_[vk];
            key_down_[vk] = e.down;
        }
        return true;
    }
    out.kind = RawInputEvent::Hid;
    return true;
}

void RawInput::take_mouse_motion(int32_t& dx, int32_t& dy) {
    dx = acc_dx_;
    dy = acc_dy_;
    acc_dx_ = acc_dy_ = 0;
}

bool enumerate_raw_devices(std::vector<RawDeviceInfo>& out) {
    out.clear();
    UINT n = 0;
    if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0) return false;
    std::vector<RAWINPUTDEVICELIST> list(n);
    if (n == 0) return true;
    UINT got = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    if (got == UINT(-1)) return false;
    for (UINT i = 0; i < got; ++i) {
        RawDeviceInfo d;
        UINT len = 0;
        GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, nullptr, &len);
        if (len) {
            std::wstring name(len, L'\0');
            GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name.data(), &len);
            name.resize(wcslen(name.c_str()));
            d.name = to_utf8(name);
        }
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof info;
        UINT sz = sizeof info;
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) == UINT(-1)) continue;
        switch (info.dwType) {
            case RIM_TYPEMOUSE:
                d.type = RawDeviceInfo::Mouse;
                d.mouse_buttons = info.mouse.dwNumberOfButtons;
                d.mouse_sample_rate = info.mouse.dwSampleRate;
                break;
            case RIM_TYPEKEYBOARD: d.type = RawDeviceInfo::Keyboard; break;
            default:
                d.type = RawDeviceInfo::Hid;
                d.vendor_id = uint16_t(info.hid.dwVendorId);
                d.product_id = uint16_t(info.hid.dwProductId);
                d.usage_page = info.hid.usUsagePage;
                d.usage = info.hid.usUsage;
                break;
        }
        out.push_back(std::move(d));
    }
    return true;
}

// ---- XInput -----------------------------------------------------------------------------------------------------------------------------------

void apply_stick_deadzone(int rx, int ry, int deadzone, float& ox, float& oy) {
    const float x = float(rx), y = float(ry);
    const float mag = std::sqrt(x * x + y * y);
    if (mag <= float(deadzone) || mag == 0.0f) { ox = oy = 0; return; }
    const float clamped = std::min(mag, 32767.0f);
    const float scale = (clamped - float(deadzone)) / (32767.0f - float(deadzone));
    ox = x / mag * scale;
    oy = y / mag * scale;
}

float apply_trigger_threshold(int raw, int threshold) {
    if (raw <= threshold) return 0;
    return float(raw - threshold) / float(255 - threshold);
}

XInput::XInput() {
    static const struct { const wchar_t* wide; const char* narrow; bool ex; } candidates[] = {{L"xinput1_4.dll", "xinput1_4.dll", true}, {L"xinput1_3.dll", "xinput1_3.dll", true}, {L"xinput9_1_0.dll", "xinput9_1_0.dll", false}};
    for (const auto& c : candidates) {
        HMODULE m = LoadLibraryExW(c.wide, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) continue;
        if (c.ex) get_state_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, MAKEINTRESOURCEA(100))));   // XInputGetStateEx: adds the Guide button
        use_ex_ = get_state_ != nullptr;
        if (!get_state_) get_state_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
        set_state_ = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputSetState")));
        battery_ = reinterpret_cast<BatteryFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetBatteryInformation")));
        if (get_state_) { dll_ = c.narrow; return; }
    }
}

bool XInput::poll(int slot, GamepadState& out) {
    out = GamepadState{};
    if (!get_state_ || slot < 0 || slot >= MaxPads) return false;
    const uint64_t now = GetTickCount64();
    const bool was_connected = (connected_mask_ >> slot) & 1;
    if (!was_connected && now < next_absent_poll_ms_[slot]) return false;
    XINPUT_STATE st{};
    const DWORD r = get_state_(DWORD(slot), &st);
    if (r != ERROR_SUCCESS) {
        connected_mask_ &= ~(1u << slot);
        next_absent_poll_ms_[slot] = now + absent_poll_interval_ms;
        return false;
    }
    connected_mask_ |= 1u << slot;
    out.connected = true;
    out.packet = st.dwPacketNumber;
    out.buttons = st.Gamepad.wButtons;
    out.left_trigger = apply_trigger_threshold(st.Gamepad.bLeftTrigger, trigger_threshold);
    out.right_trigger = apply_trigger_threshold(st.Gamepad.bRightTrigger, trigger_threshold);
    apply_stick_deadzone(st.Gamepad.sThumbLX, st.Gamepad.sThumbLY, left_deadzone, out.left_x, out.left_y);
    apply_stick_deadzone(st.Gamepad.sThumbRX, st.Gamepad.sThumbRY, right_deadzone, out.right_x, out.right_y);
    out.any_input = out.buttons != 0 || out.left_trigger > 0 || out.right_trigger > 0 || out.left_x != 0 || out.left_y != 0 || out.right_x != 0 || out.right_y != 0;
    return true;
}

Status XInput::set_rumble(int slot, float low, float high) {
    if (!set_state_) return Status::fail("XInputSetState unavailable", ERROR_NOT_SUPPORTED);
    if (slot < 0 || slot >= MaxPads) return Status::fail("bad slot", ERROR_INVALID_PARAMETER);
    XINPUT_VIBRATION v{};
    v.wLeftMotorSpeed = WORD(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
    v.wRightMotorSpeed = WORD(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
    const DWORD r = set_state_(DWORD(slot), &v);
    return r == ERROR_SUCCESS ? Status::success() : Status::fail("XInputSetState", r);
}

void XInput::stop_all_rumble() {
    for (int i = 0; i < MaxPads; ++i) if ((connected_mask_ >> i) & 1) set_rumble(i, 0, 0);
}

bool XInput::battery(int slot, GamepadBattery& out) {
    out = GamepadBattery{};
    if (!battery_ || slot < 0 || slot >= MaxPads) return false;
    XINPUT_BATTERY_INFORMATION b{};
    if (battery_(DWORD(slot), BATTERY_DEVTYPE_GAMEPAD, &b) != ERROR_SUCCESS) return false;
    switch (b.BatteryType) {
        case BATTERY_TYPE_WIRED: out.kind = BatteryKind::Wired; break;
        case BATTERY_TYPE_ALKALINE: out.kind = BatteryKind::Alkaline; break;
        case BATTERY_TYPE_NIMH: out.kind = BatteryKind::Nimh; break;
        case BATTERY_TYPE_DISCONNECTED: out.kind = BatteryKind::None; break;
        default: out.kind = BatteryKind::Unknown; break;
    }
    switch (b.BatteryLevel) {
        case BATTERY_LEVEL_EMPTY: out.level = BatteryLevel::Empty; break;
        case BATTERY_LEVEL_LOW: out.level = BatteryLevel::Low; break;
        case BATTERY_LEVEL_MEDIUM: out.level = BatteryLevel::Medium; break;
        case BATTERY_LEVEL_FULL: out.level = BatteryLevel::Full; break;
        default: out.level = BatteryLevel::Unknown; break;
    }
    return true;
}

// ---- GameInput -----------------------------------------------------------------------------------------------------------------------------------

GameInputProbe probe_game_input() {
    GameInputProbe p;
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring redist = std::wstring(sys) + L"\\GameInputRedist.dll";
        p.redistributable_present = GetFileAttributesW(redist.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    HMODULE m = LoadLibraryExW(L"GameInput.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { p.note = "GameInput.dll is not installed (Windows 10 before 1809 or a stripped image)"; return p; }
    p.dll_present = true;
    using CreateFn = HRESULT(WINAPI*)(void** out);
    auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(m, "GameInputCreate")));
    if (!create) { p.note = "GameInput.dll has no GameInputCreate export"; return p; }
    void* object = nullptr;
    HRESULT hr = create(&object);
    if (SUCCEEDED(hr) && object) {
        p.create_ok = true;
        static_cast<IUnknown*>(object)->Release();
        p.note = "GameInputCreate succeeded";
    } else {
        p.note = "GameInputCreate failed: " + error_message(uint32_t(hr));
    }
    return p;
}

const char* input_device_name(InputDevice d) {
    switch (d) {
        case InputDevice::KeyboardMouse: return "keyboard+mouse";
        case InputDevice::Gamepad: return "gamepad";
        default: return "none";
    }
}

}  // namespace nue::win
