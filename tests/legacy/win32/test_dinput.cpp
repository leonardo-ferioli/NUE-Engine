// DirectInput: the device model on scripted platform input (state, buffering, properties, rules), then the call rules against the real DirectInput 8
// with the system keyboard and mouse (no input is injected: only results that do not depend on what the machine's devices do are compared).
#include "nue_legacy/dinput.hpp"
#include "nue_legacy/platform/null_services.hpp"
#include "test_support.hpp"
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#endif

using namespace nue::legacy::dinput;
using nue::legacy::win32::MutableBytes;
namespace null = nue::legacy::platform::null;

namespace {
struct Fixture {
    Fixture() : di(input, clock, [this](uint32_t hwnd) { return hwnd == foreground_window; }) {}
    null::NullInput input;
    null::NullClock clock;
    uint32_t foreground_window = 100;
    DirectInput di;
    std::shared_ptr<Device> make(const Guid& g) { std::shared_ptr<Device> d; CHECK_EQ(di.create_device(g, d), hr::ok); return d; }
    int64_t t = 0;
    int64_t next() { t += 1000000; return t; }
};
uint8_t byte_at(const std::vector<uint8_t>& v, size_t i) { return v[i]; }
}  // namespace

static void test_dinput() {
    Fixture f;
    // Enumeration: keyboard and mouse are always there, game controllers appear with the platform.
    CHECK_EQ(f.di.enum_devices(devclass::keyboard, 0).size(), 1u);
    CHECK_EQ(f.di.enum_devices(devclass::pointer, 0).size(), 1u);
    CHECK_EQ(f.di.enum_devices(devclass::game_controller, 0).size(), 0u);
    nue::legacy::platform::GamepadInfo info; info.name = "Test Pad"; info.axes = 6; info.buttons = 12;
    const auto pad = f.input.add_pad(info);
    f.input.pump();
    auto pads = f.di.enum_devices(devclass::game_controller, enumflag::attached_only);
    CHECK_EQ(pads.size(), 1u);
    CHECK(pads[0].instance_name == "Test Pad" && (pads[0].device_type & 0xFF) == devtype::gamepad);
    CHECK_EQ(f.di.enum_devices(devtype::gamepad, 0).size(), 1u);
    std::shared_ptr<Device> bad;
    CHECK_EQ(f.di.create_device(make_guid(1, 2, 3, {1, 2, 3, 4, 5, 6, 7, 8}), bad), hr::device_not_reg);
    CHECK_EQ(f.di.create_device(Guid{}, bad), hr::device_not_reg);

    // ---- keyboard
    auto kb = f.make(kGuidSysKeyboard);
    std::vector<uint8_t> state(256);
    auto read_state = [&](Device& d) { return d.get_device_state(MutableBytes(reinterpret_cast<std::byte*>(state.data()), state.size())); };
    CHECK_EQ(read_state(*kb), hr::not_acquired);                               // nothing is acquired yet (the real library checks this first)
    CHECK_EQ(kb->set_data_format(standard_keyboard_format()), hr::ok);
    CHECK_EQ(kb->acquire(), hr::invalid_param);                                // no cooperative level yet
    CHECK_EQ(kb->set_cooperative_level(100, coop::foreground | coop::nonexclusive), hr::ok);
    CHECK_EQ(read_state(*kb), hr::not_acquired);
    f.foreground_window = 7;
    CHECK_EQ(kb->acquire(), hr::other_app_has_prio);                           // a foreground device needs its window in front
    f.foreground_window = 100;
    CHECK_EQ(kb->acquire(), hr::ok);
    CHECK_EQ(kb->acquire(), hr::false_);                                       // already acquired: DI_NOEFFECT
    CHECK_EQ(kb->set_data_format(standard_keyboard_format()), hr::acquired);   // not while acquired
    CHECK_EQ(kb->set_property_dword(prop::buffersize, how::device, 0, 16), hr::acquired);
    f.input.key(4 /* A */, true, f.next());
    f.input.key(44 /* space */, true, f.next());
    f.input.pump();
    CHECK_EQ(read_state(*kb), hr::ok);
    CHECK_EQ(byte_at(state, 0x1E), 0x80);                                      // DIK_A
    CHECK_EQ(byte_at(state, 0x39), 0x80);                                      // DIK_SPACE
    CHECK_EQ(byte_at(state, 0x30), 0);
    state.resize(100);
    CHECK_EQ(kb->get_device_state(MutableBytes(reinterpret_cast<std::byte*>(state.data()), state.size())), hr::invalid_param);   // wrong size
    state.resize(256);
    CHECK_EQ(kb->poll(), hr::false_);                                          // a keyboard is not polled: DI_NOEFFECT
    f.foreground_window = 9;
    CHECK_EQ(read_state(*kb), hr::input_lost);                                 // the window lost the foreground
    CHECK_EQ(read_state(*kb), hr::not_acquired);
    f.foreground_window = 100;
    CHECK_EQ(kb->unacquire(), hr::false_);                                     // already lost
    // Buffered data.
    uint32_t count = 10;
    std::vector<DeviceEvent> events;
    CHECK_EQ(kb->get_device_data(&events, count, 0), hr::not_acquired);        // the device was lost
    CHECK_EQ(kb->acquire(), hr::ok);
    CHECK_EQ(kb->get_device_data(&events, count, 0), hr::not_buffered);        // buffer size is 0
    CHECK_EQ(kb->unacquire(), hr::ok);
    CHECK_EQ(kb->set_property_dword(prop::buffersize, how::device, 0, 4), hr::ok);   // room for 3: the ring keeps one slot free
    uint32_t size = 0;
    CHECK(kb->get_property_dword(prop::buffersize, how::device, 0, size) == hr::ok && size == 4);
    CHECK_EQ(kb->acquire(), hr::ok);
    f.input.key(30 /* 1 */, true, f.next());
    f.input.key(30, false, f.next());
    f.input.pump();
    count = 10;
    CHECK_EQ(kb->get_device_data(&events, count, gdd::peek), hr::ok);
    CHECK_EQ(count, 2u);
    count = 10;
    CHECK_EQ(kb->get_device_data(&events, count, 0), hr::ok);
    CHECK(count == 2 && events[0].offset == 0x02 && events[0].data == 0x80 && events[1].data == 0 && events[1].sequence == events[0].sequence + 1);
    count = 10;
    CHECK_EQ(kb->get_device_data(&events, count, 0), hr::ok);
    CHECK_EQ(count, 0u);
    for (uint32_t key : {5u, 6u, 7u, 8u}) f.input.key(key, true, f.next());   // four events into a ring of three
    f.input.pump();
    count = 10;
    CHECK_EQ(kb->get_device_data(&events, count, 0), hr::false_);              // DI_BUFFEROVERFLOW once
    CHECK_EQ(count, 3u);
    CHECK_EQ(events[0].offset, 0x2Eu);                                         // the oldest event (B) was dropped: the first one left is C (DIK_C)
    count = 10;
    CHECK_EQ(kb->get_device_data(&events, count, 0), hr::ok);                  // the flag is cleared by a read
    CHECK_EQ(kb->unacquire(), hr::ok);

    // ---- mouse: relative axes accumulate until read, buttons use the DirectInput order (left, right, middle)
    auto mouse = f.make(kGuidSysMouse);
    CHECK_EQ(mouse->set_data_format(standard_mouse_format(true)), hr::ok);
    CHECK_EQ(mouse->set_cooperative_level(100, coop::exclusive | coop::foreground), hr::ok);
    CHECK_EQ(mouse->acquire(), hr::ok);
    f.input.mouse_move(5, -3, f.next());
    f.input.mouse_move(2, 1, f.next());
    f.input.mouse_wheel(1, f.next());
    f.input.mouse_button(3 /* platform right */, true, f.next());
    f.input.mouse_button(2 /* platform middle */, true, f.next());
    f.input.pump();
    std::vector<uint8_t> mstate(20);
    auto read_mouse = [&] { return mouse->get_device_state(MutableBytes(reinterpret_cast<std::byte*>(mstate.data()), mstate.size())); };
    CHECK_EQ(read_mouse(), hr::ok);
    int32_t axes[3];
    std::memcpy(axes, mstate.data(), 12);
    CHECK(axes[0] == 7 && axes[1] == -2 && axes[2] == 120);
    CHECK(mstate[12] == 0 && mstate[13] == 0x80 && mstate[14] == 0x80);        // button 0 left up, button 1 right down, button 2 middle down
    CHECK_EQ(read_mouse(), hr::ok);
    std::memcpy(axes, mstate.data(), 12);
    CHECK(axes[0] == 0 && axes[1] == 0 && axes[2] == 0);                       // the motion was consumed
    CHECK(mstate[13] == 0x80);                                                 // the buttons are state
    CHECK_EQ(mouse->unacquire(), hr::ok);
    CHECK_EQ(mouse->set_property_dword(prop::axismode, how::device, 0, axismode::absolute), hr::ok);
    CHECK_EQ(mouse->acquire(), hr::ok);
    f.input.mouse_move(10, 20, f.next());
    f.input.pump();
    CHECK_EQ(read_mouse(), hr::ok);
    std::memcpy(axes, mstate.data(), 12);
    CHECK(axes[0] == 17 && axes[1] == 18);                                     // absolute: the running position (5+2+10, -3+1+20)
    CHECK_EQ(mouse->unacquire(), hr::ok);

    // ---- game controller: axes with range, dead zone, POV, buttons, buffered data
    std::shared_ptr<Device> joy;
    CHECK_EQ(f.di.create_device(pads[0].instance, joy), hr::ok);
    CHECK_EQ(joy->set_data_format(standard_joystick_format(true)), hr::ok);
    CHECK_EQ(joy->set_cooperative_level(100, coop::exclusive | coop::foreground), hr::ok);
    CHECK_EQ(joy->set_property_range(prop::range, how::device, 0, -1000, 1000), hr::ok);
    CHECK_EQ(joy->set_property_dword(prop::deadzone, how::device, 0, 1000), hr::ok);   // 10 %
    CHECK_EQ(joy->set_property_dword(prop::buffersize, how::device, 0, 32), hr::ok);
    int32_t lo = 0, hi = 0;
    CHECK(joy->get_property_range(prop::range, how::byoffset, 0, lo, hi) == hr::ok && lo == -1000 && hi == 1000);
    CHECK_EQ(joy->acquire(), hr::ok);
    CHECK_EQ(joy->poll(), hr::ok);                                             // a game controller is polled
    f.input.pad_axis(pad, 0, 32767, f.next());                                 // left stick full right
    f.input.pad_axis(pad, 1, 1000, f.next());                                  // a small push inside the dead zone
    f.input.pad_button(pad, 0, true, f.next());
    f.input.pad_hat(pad, 0, 2, f.next());                                      // right
    f.input.pump();
    std::vector<uint8_t> jstate(272);
    CHECK_EQ(joy->get_device_state(MutableBytes(reinterpret_cast<std::byte*>(jstate.data()), jstate.size())), hr::ok);
    int32_t jx, jy; uint32_t pov0, pov1;
    std::memcpy(&jx, &jstate[0], 4); std::memcpy(&jy, &jstate[4], 4); std::memcpy(&pov0, &jstate[32], 4); std::memcpy(&pov1, &jstate[36], 4);
    CHECK(jx >= 999 && jx <= 1000);
    CHECK_EQ(jy, 0);                                                           // inside the dead zone: centred
    CHECK_EQ(pov0, 9000u);
    CHECK_EQ(pov1, 0xFFFFFFFFu);
    CHECK_EQ(jstate[48], 0x80);
    count = 32;
    CHECK_EQ(joy->get_device_data(&events, count, 0), hr::ok);
    CHECK_EQ(count, 4u);                                                       // X, Y, button 0, POV 0
    CHECK(events[0].offset == 0 && int32_t(events[0].data) >= 999);
    CHECK(events[2].offset == 48 && events[2].data == 0x80);
    CHECK(events[3].offset == 32 && events[3].data == 9000);
    // Capabilities and objects.
    DeviceCaps caps;
    CHECK_EQ(joy->get_capabilities(caps), hr::ok);
    CHECK(caps.axes == 8 && caps.buttons == 32 && caps.povs == 4);
    DeviceObject object;
    CHECK(joy->get_object_info(0, how::byoffset, object) == hr::ok && object.guid == kGuidXAxis);
    CHECK_EQ(joy->get_object_info(9999, how::byoffset, object), hr::object_not_found);
    // A format object that cannot be matched and is not optional is refused.
    CHECK_EQ(joy->unacquire(), hr::ok);
    DataFormat strict;
    strict.data_size = 8;
    strict.objects.push_back({kGuidKey, 0, didft::button | didft::any_instance, 0, true});     // a joystick has no keys
    CHECK_EQ(joy->set_data_format(strict), hr::invalid_param);
    strict.objects[0].type |= didft::optional;
    CHECK_EQ(joy->set_data_format(strict), hr::ok);
    // Cooperative level rules.
    CHECK_EQ(joy->set_cooperative_level(100, coop::exclusive | coop::nonexclusive | coop::foreground), hr::invalid_param);
    CHECK_EQ(joy->set_cooperative_level(100, coop::exclusive), hr::invalid_param);

    // ---- action mapping: keyboard and mouse semantics map, game controller semantics stay unmapped
    ActionFormat actions;
    Action jump; jump.semantic = 0x81000039u; jump.app_data = 11;                  // DIKEYBOARD_SPACE
    Action steer; steer.semantic = 0x01008A01u; steer.app_data = 12;               // a driving-genre axis
    actions.actions = {jump, steer};
    auto kb2 = f.make(kGuidSysKeyboard);
    CHECK_EQ(kb2->build_action_map(actions, "player", 0), hr::ok);
    CHECK(actions.actions[0].how == dia::app_mapped && actions.actions[0].guid_instance == kGuidSysKeyboard && instance_of(actions.actions[0].object_id) == 0x39);
    CHECK_EQ(actions.actions[1].how, uint32_t(dia::unmapped));
    CHECK_EQ(kb2->set_action_map(actions, "player", 0), hr::ok);
    CHECK_EQ(f.di.enum_devices_by_semantics(actions, 0).size(), 2u);               // the keyboard (keyboard semantics) and the game controller (the genre)
    CHECK_EQ(f.di.get_device_status(kGuidSysKeyboard), hr::ok);
}
REGISTER_TEST("dinput", test_dinput);

#if defined(_WIN32)
namespace {
// The standard formats as the real library has them, rebuilt as the library's DataFormat so both sides get the same bytes.
template <class F> void expect_equal(const char* what, F&& real_call, F&& mine_call, int& differences) {
    const auto a = real_call(), b = mine_call();
    if (a != b) { ++differences; std::fprintf(stderr, "%s: windows 0x%08X, mine 0x%08X\n", what, uint32_t(a), uint32_t(b)); }
}
LRESULT CALLBACK nothing_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
}  // namespace

static void test_dinput_against_windows() {
    WNDCLASSEXA wc{sizeof wc};
    wc.lpfnWndProc = nothing_proc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "nue_legacy_dinput_test";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "t", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);   // never shown
    IDirectInput8A* real = nullptr;
    if (FAILED(DirectInput8Create(wc.hInstance, DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&real), nullptr)) || !real) { std::printf("  (no DirectInput 8 on this machine: skipped)\n"); return; }
    null::NullInput input; null::NullClock clock;
    DirectInput mine(input, clock, [](uint32_t) { return false; });   // the test window is not in front

    struct Pair { IDirectInputDevice8A* real = nullptr; std::shared_ptr<Device> mine; };
    auto make = [&](const GUID& g, const Guid& mg) { Pair p; CHECK(SUCCEEDED(real->CreateDevice(g, &p.real, nullptr))); CHECK_EQ(mine.create_device(mg, p.mine), hr::ok); return p; };
    int differences = 0;
    auto compare = [&](const char* what, HRESULT a, Hresult b) { if (uint32_t(a) != b) { ++differences; std::fprintf(stderr, "%s: windows 0x%08X, mine 0x%08X\n", what, uint32_t(a), b); } };
    // Unknown and null instance GUIDs.
    {
        IDirectInputDevice8A* d = nullptr;
        GUID unknown = {1, 2, 3, {1, 2, 3, 4, 5, 6, 7, 8}};
        std::shared_ptr<Device> m;
        compare("create unknown", real->CreateDevice(unknown, &d, nullptr), mine.create_device(make_guid(1, 2, 3, {1, 2, 3, 4, 5, 6, 7, 8}), m));
        compare("create null", real->CreateDevice(GUID_NULL, &d, nullptr), mine.create_device(Guid{}, m));
    }
    for (int which = 0; which < 2; ++which) {
        const bool keyboard = which == 0;
        auto d = keyboard ? make(GUID_SysKeyboard, kGuidSysKeyboard) : make(GUID_SysMouse, kGuidSysMouse);
        const char* name = keyboard ? "keyboard " : "mouse ";
        auto label = [&](const char* s) { static std::string text; text = std::string(name) + s; return text.c_str(); };
        BYTE buffer[256];
        DataFormat format = keyboard ? standard_keyboard_format() : standard_mouse_format(true);
        const DIDATAFORMAT* real_format = keyboard ? &c_dfDIKeyboard : &c_dfDIMouse2;
        MutableBytes bytes(reinterpret_cast<std::byte*>(buffer), keyboard ? 256 : 20);
        // Before anything is set.
        compare(label("acquire with nothing set"), d.real->Acquire(), d.mine->acquire());
        compare(label("get state with nothing set"), d.real->GetDeviceState(keyboard ? 256 : 20, buffer), d.mine->get_device_state(bytes));
        compare(label("poll with nothing set"), d.real->Poll(), d.mine->poll());
        compare(label("unacquire when not acquired"), d.real->Unacquire(), d.mine->unacquire());
        // Cooperative level combinations.
        const struct { DWORD real; uint32_t mine; const char* what; } levels[] = {
            {DISCL_FOREGROUND | DISCL_NONEXCLUSIVE, coop::foreground | coop::nonexclusive, "fg+nonexclusive"},
            {DISCL_BACKGROUND | DISCL_NONEXCLUSIVE, coop::background | coop::nonexclusive, "bg+nonexclusive"},
            {DISCL_FOREGROUND | DISCL_EXCLUSIVE, coop::foreground | coop::exclusive, "fg+exclusive"},
            {DISCL_BACKGROUND | DISCL_EXCLUSIVE, coop::background | coop::exclusive, "bg+exclusive"},
            {DISCL_FOREGROUND, coop::foreground, "foreground only"},
            {DISCL_EXCLUSIVE, coop::exclusive, "exclusive only"},
            {DISCL_FOREGROUND | DISCL_BACKGROUND | DISCL_NONEXCLUSIVE, coop::foreground | coop::background | coop::nonexclusive, "both fg and bg"},
            {0, 0, "none"},
        };
        for (const auto& l : levels) compare(label(l.what), d.real->SetCooperativeLevel(hwnd, l.real), d.mine->set_cooperative_level(1, l.mine));
        // Data format.
        compare(label("set data format"), d.real->SetDataFormat(real_format), d.mine->set_data_format(format));
        compare(label("coop for acquire"), d.real->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE), d.mine->set_cooperative_level(1, coop::background | coop::nonexclusive));
        // Properties.
        DIPROPDWORD pd{{sizeof pd, sizeof(DIPROPHEADER), 0, DIPH_DEVICE}, 0};
        uint32_t value = 99;
        compare(label("get buffer size"), d.real->GetProperty(DIPROP_BUFFERSIZE, &pd.diph), d.mine->get_property_dword(prop::buffersize, how::device, 0, value));
        if (pd.dwData != value) { ++differences; std::fprintf(stderr, "%sdefault buffer size: windows %lu, mine %u\n", name, pd.dwData, value); }
        pd.dwData = 16;
        compare(label("set buffer size"), d.real->SetProperty(DIPROP_BUFFERSIZE, &pd.diph), d.mine->set_property_dword(prop::buffersize, how::device, 0, 16));
        pd.dwData = 0;
        compare(label("get axis mode"), d.real->GetProperty(DIPROP_AXISMODE, &pd.diph), d.mine->get_property_dword(prop::axismode, how::device, 0, value));
        if (!keyboard && pd.dwData != value) { ++differences; std::fprintf(stderr, "%sdefault axis mode: windows %lu, mine %u\n", name, pd.dwData, value); }
        pd.dwData = 7;
        compare(label("set axis mode with a bad value"), d.real->SetProperty(DIPROP_AXISMODE, &pd.diph), d.mine->set_property_dword(prop::axismode, how::device, 0, 7));
        pd.diph.dwHow = DIPH_BYOFFSET; pd.diph.dwObj = 0;
        pd.dwData = 5;
        compare(label("set buffer size on an object"), d.real->SetProperty(DIPROP_BUFFERSIZE, &pd.diph), d.mine->set_property_dword(prop::buffersize, how::byoffset, 0, 5));
        // Acquire, state, data.
        compare(label("acquire"), d.real->Acquire(), d.mine->acquire());
        compare(label("acquire again"), d.real->Acquire(), d.mine->acquire());
        compare(label("poll"), d.real->Poll(), d.mine->poll());
        compare(label("get state"), d.real->GetDeviceState(keyboard ? 256 : 20, buffer), d.mine->get_device_state(bytes));
        compare(label("get state with a wrong size"), d.real->GetDeviceState(7, buffer), d.mine->get_device_state(MutableBytes(reinterpret_cast<std::byte*>(buffer), 7)));
        pd.diph.dwHow = DIPH_DEVICE; pd.diph.dwObj = 0; pd.dwData = 8;
        compare(label("set buffer size while acquired"), d.real->SetProperty(DIPROP_BUFFERSIZE, &pd.diph), d.mine->set_property_dword(prop::buffersize, how::device, 0, 8));
        DWORD n = 0;
        uint32_t mine_count = 0;
        compare(label("get data with no pending events (peek)"), d.real->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &n, DIGDD_PEEK), d.mine->get_device_data(nullptr, mine_count, gdd::peek));
        compare(label("set data format while acquired"), d.real->SetDataFormat(real_format), d.mine->set_data_format(format));
        compare(label("unacquire"), d.real->Unacquire(), d.mine->unacquire());
        compare(label("unacquire again"), d.real->Unacquire(), d.mine->unacquire());
        compare(label("get data after unacquire"), d.real->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), nullptr, &n, DIGDD_PEEK), d.mine->get_device_data(nullptr, mine_count, gdd::peek));
        // Capabilities.
        DIDEVCAPS rc{sizeof rc};
        DeviceCaps mc;
        compare(label("capabilities"), d.real->GetCapabilities(&rc), d.mine->get_capabilities(mc));
        if ((rc.dwDevType & 0xFF) != (mc.device_type & 0xFF)) { ++differences; std::fprintf(stderr, "%sdevice type: windows 0x%lX, mine 0x%X\n", name, rc.dwDevType, mc.device_type); }
        DIDEVCAPS bad{7};
        compare(label("capabilities with a bad size"), d.real->GetCapabilities(&bad), mc.device_type == 0 ? hr::invalid_param : hr::invalid_param);
        d.real->Release();
    }
    // A format whose objects cannot be matched.
    {
        auto d = make(GUID_SysMouse, kGuidSysMouse);
        DIOBJECTDATAFORMAT odf[1] = {{&GUID_Key, 0, DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0}};
        DIDATAFORMAT df{sizeof df, sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, 4, 1, odf};
        DataFormat mf; mf.flags = datafmt::absolute_axes; mf.data_size = 4; mf.objects.push_back({kGuidKey, 0, didft::button | didft::any_instance, 0, true});
        compare("unmatched required object", d.real->SetDataFormat(&df), d.mine->set_data_format(mf));
        odf[0].dwType |= 0x80000000u;   // DIDFT_OPTIONAL is not in the SDK header
        mf.objects[0].type |= didft::optional;
        compare("unmatched optional object", d.real->SetDataFormat(&df), d.mine->set_data_format(mf));
        DIDATAFORMAT odd{sizeof odd, sizeof(DIOBJECTDATAFORMAT), 0, 6, 0, nullptr};
        DataFormat mo; mo.data_size = 6;
        compare("data size not a multiple of 4", d.real->SetDataFormat(&odd), d.mine->set_data_format(mo));
        d.real->Release();
    }
    real->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    std::printf("  %d differences\n", differences);
    CHECK_EQ(differences, 0);
}
REGISTER_TEST("dinput_windows", test_dinput_against_windows);
#endif
