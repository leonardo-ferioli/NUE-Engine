#include "nue_legacy/win32/host/windows_input.hpp"
#include "nue_legacy/win32/window.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <chrono>
#include <map>
#include <mutex>

namespace nue::legacy::win32::host {
namespace {

// The XInput state, declared here so that no SDK library is needed: xinput1_4.dll is loaded at run time when it exists.
struct XInputGamepad { WORD buttons; BYTE left_trigger, right_trigger; SHORT lx, ly, rx, ry; };
struct XInputState { DWORD packet; XInputGamepad pad; };
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputState*);

class WindowsInput final : public platform::Input {
public:
    WindowsInput() {
        std::memset(keys_, 0, sizeof keys_);
        for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
            if (HMODULE m = LoadLibraryExA(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) { get_state_ = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState")); if (get_state_) break; }
        }
        POINT p; GetCursorPos(&p); last_x_ = p.x; last_y_ = p.y;
        start_ = std::chrono::steady_clock::now();
    }
    void pump() override {
        std::vector<platform::InputEvent> events;
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();
        for (uint32_t hid = 4; hid < 232; ++hid) {
            const uint32_t vk = virtual_key_from_hid(hid);
            if (!vk) continue;
            const bool down = (GetAsyncKeyState(int(vk)) & 0x8000) != 0;
            if (down != (keys_[hid] != 0)) {
                keys_[hid] = down ? 1 : 0;
                platform::InputEvent e; e.kind = platform::InputEventKind::key; e.time_ns = now; e.code = hid; e.value = down;
                events.push_back(e);
            }
        }
        POINT p; GetCursorPos(&p);
        if (p.x != last_x_ || p.y != last_y_) {
            platform::InputEvent e; e.kind = platform::InputEventKind::mouse_motion; e.time_ns = now; e.value = p.x - last_x_; e.value2 = p.y - last_y_;
            events.push_back(e);
            rel_x_ += float(e.value); rel_y_ += float(e.value2);
            last_x_ = p.x; last_y_ = p.y;
        }
        static const struct { int vk; uint32_t button; } buttons[] = {{VK_LBUTTON, platform::kMouseLeft}, {VK_MBUTTON, platform::kMouseMiddle}, {VK_RBUTTON, platform::kMouseRight}, {VK_XBUTTON1, platform::kMouseX1}, {VK_XBUTTON2, platform::kMouseX2}};
        for (const auto& b : buttons) {
            const bool down = (GetAsyncKeyState(b.vk) & 0x8000) != 0;
            const uint32_t bit = 1u << (b.button - 1);
            if (down != ((buttons_ & bit) != 0)) {
                buttons_ = down ? (buttons_ | bit) : (buttons_ & ~bit);
                platform::InputEvent e; e.kind = platform::InputEventKind::mouse_button; e.time_ns = now; e.code = b.button; e.value = down;
                events.push_back(e);
            }
        }
        if (get_state_) pump_pads(now, events);
        mouse_x_ = float(p.x); mouse_y_ = float(p.y);
        std::map<uint32_t, Listener> listeners;
        { std::lock_guard lock(m_); listeners = listeners_; }
        for (const auto& e : events) for (const auto& [id, fn] : listeners) fn(e);
    }
    const uint8_t* key_state() override { return keys_; }
    uint32_t mouse_state(float* x, float* y) override { if (x) *x = mouse_x_; if (y) *y = mouse_y_; return buttons_; }
    uint32_t mouse_relative(float* dx, float* dy) override { if (dx) *dx = rel_x_; if (dy) *dy = rel_y_; rel_x_ = rel_y_ = 0; return buttons_; }
    bool mouse_present() override { return GetSystemMetrics(SM_MOUSEPRESENT) != 0; }
    std::vector<platform::GamepadInfo> gamepads() override { std::lock_guard lock(m_); std::vector<platform::GamepadInfo> out; for (auto& [user, info] : pads_) if (info.connected) out.push_back(info); return out; }
    bool gamepad_state(platform::GamepadId id, platform::GamepadState& out) override { std::lock_guard lock(m_); auto it = pad_states_.find(id); if (it == pad_states_.end()) return false; out = it->second; return true; }
    uint32_t add_listener(Listener l) override { std::lock_guard lock(m_); listeners_[next_++] = std::move(l); return next_ - 1; }
    void remove_listener(uint32_t id) override { std::lock_guard lock(m_); listeners_.erase(id); }

private:
    void pump_pads(int64_t now, std::vector<platform::InputEvent>& events) {
        for (DWORD user = 0; user < 4; ++user) {
            XInputState s{};
            const bool present = get_state_(user, &s) == ERROR_SUCCESS;
            std::lock_guard lock(m_);
            auto info_it = pads_.find(user);
            if (present && info_it == pads_.end()) {
                platform::GamepadInfo info; info.id = user + 1; info.name = "XInput Controller " + std::to_string(user + 1); info.axes = 6; info.buttons = 14; info.hats = 1; info.xbox_style = true;
                pads_[user] = info;
                platform::InputEvent e; e.kind = platform::InputEventKind::pad_added; e.pad = info.id; e.time_ns = now; events.push_back(e);
            } else if (!present && info_it != pads_.end()) {
                platform::InputEvent e; e.kind = platform::InputEventKind::pad_removed; e.pad = info_it->second.id; e.time_ns = now; events.push_back(e);
                pad_states_.erase(info_it->second.id);
                pads_.erase(info_it);
                continue;
            }
            if (!present) continue;
            const platform::GamepadId id = user + 1;
            platform::GamepadState next{};
            next.axes[0] = s.pad.lx; next.axes[1] = int16_t(-s.pad.ly); next.axes[2] = s.pad.rx; next.axes[3] = int16_t(-s.pad.ry);
            next.axes[4] = int16_t(s.pad.left_trigger * 32767 / 255); next.axes[5] = int16_t(s.pad.right_trigger * 32767 / 255);
            // XInput button bits: 1 up, 2 down, 4 left, 8 right, 0x10 start, 0x20 back, 0x40 L3, 0x80 R3, 0x100 LB, 0x200 RB, 0x1000 A, 0x2000 B, 0x4000 X, 0x8000 Y.
            static const WORD order[] = {0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0x20, 0x10, 0x40, 0x80};
            for (unsigned i = 0; i < sizeof order / sizeof order[0]; ++i) if (s.pad.buttons & order[i]) next.buttons |= 1u << i;
            uint8_t hat = 0;
            if (s.pad.buttons & 1) hat |= 1; if (s.pad.buttons & 8) hat |= 2; if (s.pad.buttons & 2) hat |= 4; if (s.pad.buttons & 4) hat |= 8;
            next.hats[0] = hat;
            platform::GamepadState& prev = pad_states_[id];
            for (uint32_t a = 0; a < 6; ++a) if (prev.axes[a] != next.axes[a]) { platform::InputEvent e; e.kind = platform::InputEventKind::pad_axis; e.pad = id; e.code = a; e.value = next.axes[a]; e.time_ns = now; events.push_back(e); }
            for (uint32_t b = 0; b < 32; ++b) if (((prev.buttons ^ next.buttons) >> b) & 1) { platform::InputEvent e; e.kind = platform::InputEventKind::pad_button; e.pad = id; e.code = b; e.value = (next.buttons >> b) & 1; e.time_ns = now; events.push_back(e); }
            if (prev.hats[0] != next.hats[0]) { platform::InputEvent e; e.kind = platform::InputEventKind::pad_hat; e.pad = id; e.code = 0; e.value = next.hats[0]; e.time_ns = now; events.push_back(e); }
            prev = next;
        }
    }

    std::mutex m_;
    uint8_t keys_[platform::kScancodeCount];
    float mouse_x_ = 0, mouse_y_ = 0, rel_x_ = 0, rel_y_ = 0;
    LONG last_x_ = 0, last_y_ = 0;
    uint32_t buttons_ = 0;
    std::map<uint32_t, Listener> listeners_;
    uint32_t next_ = 1;
    XInputGetStateFn get_state_ = nullptr;
    std::map<DWORD, platform::GamepadInfo> pads_;
    std::map<platform::GamepadId, platform::GamepadState> pad_states_;
    std::chrono::steady_clock::time_point start_;
};
}  // namespace

std::unique_ptr<platform::Input> make_windows_input() { return std::make_unique<WindowsInput>(); }
}  // namespace nue::legacy::win32::host
