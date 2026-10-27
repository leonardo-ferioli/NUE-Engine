// DirectInput 8 (the `legacy_dinput` part): the device model behind IDirectInput8A / IDirectInputDevice8A. Plain C++: the facade turns the COM objects, guest
// structures (DIDATAFORMAT, DIPROPHEADER, DIACTIONFORMAT, DIDEVICEOBJECTDATA) and callbacks into the types below, and COM results are HRESULT numbers.
//
// Written against platform::Input (keyboard table, mouse, gamepads and an ordered event stream, shaped like NUE's input.h), so the same guest code runs on any host.
// What is modelled: the enumeration of devices, data formats (an object of the format is matched to an object of the device and gets an offset in the
// state), state reads (keyboard 0x80 bytes, mouse relative or absolute, joystick axes with range, dead zone and saturation, POV in hundredths of a
// degree), buffered data (a ring that keeps one slot free, overflow reported once), properties, cooperative levels with the foreground rule, acquire /
// unacquire / poll, capabilities and objects, and the action mapping of keyboard and mouse semantics. Not modelled: force feedback, effects, the default
// genre mappings of game controllers (their actions stay unmapped: see the hand-over document), configuration UI.
#pragma once
#include "nue_legacy/win32/common.hpp"
#include "nue_legacy/platform/clock.hpp"
#include "nue_legacy/platform/input.hpp"
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nue::legacy::dinput {

using win32::MutableBytes;
using Hresult = uint32_t;
namespace hr {
enum : Hresult {
    ok = 0, false_ = 1,                                    // S_OK, S_FALSE (DI_NOEFFECT, DI_NOTATTACHED, DI_BUFFEROVERFLOW, DI_PROPNOEFFECT share it)
    polled_device = 2,                                     // DI_POLLEDDEVICE
    invalid_param = 0x80070057u, not_initialized = 0x80070015u, input_lost = 0x8007001Eu, not_acquired = 0x8007000Cu, other_app_has_prio = 0x80070005u,
    device_not_reg = 0x80040154u, object_not_found = 0x80070002u, not_buffered = 0x80040207u, acquired = 0x800700AAu, unsupported = 0x80004001u,
    more_data = 0x800700EAu, no_interface = 0x80004002u, already_initialized = 0x800704DFu, handle = 0x80070006u, out_of_memory = 0x8007000Eu, no_aggregation = 0x80040110u,
    old_version = 0x8007047Eu, beta_version = 0x80070481u, bad_driver_version = 0x80070077u,
};
}

struct Guid {
    std::array<uint8_t, 16> bytes{};
    bool is_null() const { for (auto b : bytes) if (b) return false; return true; }
    friend bool operator==(const Guid&, const Guid&) = default;
    friend bool operator<(const Guid& a, const Guid& b) { return a.bytes < b.bytes; }
};
// The documented instance GUIDs and the object-type GUIDs (little-endian field layout of GUID).
Guid make_guid(uint32_t d1, uint16_t d2, uint16_t d3, std::array<uint8_t, 8> d4);
extern const Guid kGuidSysKeyboard, kGuidSysMouse, kGuidKey, kGuidXAxis, kGuidYAxis, kGuidZAxis, kGuidRxAxis, kGuidRyAxis, kGuidRzAxis, kGuidSlider, kGuidButton, kGuidPov;

namespace devclass { enum : uint32_t { all = 0, device = 1, pointer = 2, keyboard = 3, game_controller = 4 }; }
namespace devtype { enum : uint32_t { device = 0x11, mouse = 0x12, keyboard = 0x13, joystick = 0x14, gamepad = 0x15, driving = 0x16, flight = 0x17, first_person = 0x18 }; }
namespace enumflag { enum : uint32_t { all_devices = 0, attached_only = 1, force_feedback = 0x100, include_aliases = 8, include_phantoms = 0x10, include_hidden = 0x20 }; }
namespace didft { enum : uint32_t { all = 0, relative_axis = 1, absolute_axis = 2, axis = 3, push_button = 4, toggle_button = 8, button = 0xC, pov = 0x10, collection = 0x40, no_data = 0x80,
                                      any_instance = 0x00FFFF00, instance_mask = 0x00FFFF00, optional = 0x80000000u }; }
inline uint32_t make_instance(uint32_t n) { return n << 8; }
inline uint32_t instance_of(uint32_t type) { return (type >> 8) & 0xFFFF; }
namespace datafmt { enum : uint32_t { absolute_axes = 1, relative_axes = 2 }; }
namespace coop { enum : uint32_t { exclusive = 1, nonexclusive = 2, foreground = 4, background = 8, nowinkey = 0x10 }; }
namespace gdd { enum : uint32_t { peek = 1 }; }
namespace prop { enum : uint32_t { buffersize = 1, axismode = 2, granularity = 3, range = 4, deadzone = 5, saturation = 6, ffgain = 7, ffload = 8, autocenter = 9, calibrationmode = 10,
                                    calibration = 11, guidandpath = 12, instancename = 13, productname = 14, joystickid = 15, physicalrange = 18, logicalrange = 19, keyname = 20,
                                    cpoints = 21, appdata = 22, scancode = 23, vidpid = 24, username = 25, typename_ = 26 }; }
namespace how { enum : uint32_t { device = 0, byoffset = 1, byid = 2, byusage = 3 }; }
namespace axismode { enum : uint32_t { absolute = 0, relative = 1 }; }

struct DataFormatObject { Guid guid; uint32_t offset = 0, type = 0, flags = 0; bool has_guid = false; };
struct DataFormat { uint32_t flags = 0, data_size = 0; std::vector<DataFormatObject> objects; };

struct DeviceInstance {
    Guid instance, product;
    uint32_t device_type = 0;                   // DI8DEVTYPE_* | (subtype << 8)
    std::string instance_name, product_name;
    platform::GamepadId pad = 0;                // for a game controller
};

struct DeviceObject {                           // DIDEVICEOBJECTINSTANCE
    Guid guid;
    uint32_t offset = 0;                        // in the device's native state (c_dfDIKeyboard, c_dfDIMouse2, c_dfDIJoystick2)
    uint32_t type = 0;                          // DIDFT_* | instance
    uint32_t flags = 0;
    std::string name;
    uint32_t usage_page = 0, usage = 0;
};

struct DeviceCaps { uint32_t flags = 1, device_type = 0, axes = 0, buttons = 0, povs = 0, ff_sample_period = 0, ff_min_time_resolution = 0, firmware_revision = 0, hardware_revision = 0, ff_driver_version = 0; };

struct DeviceEvent { uint32_t offset = 0, data = 0, timestamp = 0, sequence = 0, app_data = 0; };

struct Action {                                 // DIACTION
    uint32_t app_data = 0, semantic = 0, flags = 0;
    Guid guid_instance;
    uint32_t object_id = 0, how = 0;            // filled by BuildActionMap
    std::string name;
};
namespace dia { enum : uint32_t { app_mapped = 0x2, unmapped = 0x0, user_config = 0x00000100, default_ = 0x00000080 }; }
struct ActionFormat {                           // DIACTIONFORMAT
    uint32_t flags = 0, data_size = 0, genre = 0, buffer_size = 0;
    int32_t axis_min = -100, axis_max = 100;
    Guid action_map_guid;
    std::vector<Action> actions;
};

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceInstance& instance() const = 0;
    virtual Hresult get_capabilities(DeviceCaps& out) = 0;
    virtual std::vector<DeviceObject> objects() const = 0;                       // EnumObjects (all kinds)
    virtual Hresult get_object_info(uint32_t object, uint32_t how, DeviceObject& out) = 0;

    virtual Hresult set_data_format(const DataFormat&) = 0;
    virtual Hresult set_cooperative_level(uint32_t hwnd, uint32_t flags) = 0;
    virtual Hresult acquire() = 0;
    virtual Hresult unacquire() = 0;
    virtual Hresult poll() = 0;
    virtual Hresult get_device_state(MutableBytes out) = 0;
    // `events` is filled up to `count`; on return `count` is the number delivered. `peek` leaves them in the buffer. hr::false_ = DI_BUFFEROVERFLOW.
    virtual Hresult get_device_data(std::vector<DeviceEvent>* events, uint32_t& count, uint32_t flags) = 0;

    // Properties. `how`/`object` select the target (DIPH_DEVICE ignores `object`).
    virtual Hresult set_property_dword(uint32_t property, uint32_t how, uint32_t object, uint32_t value) = 0;
    virtual Hresult get_property_dword(uint32_t property, uint32_t how, uint32_t object, uint32_t& value) = 0;
    virtual Hresult set_property_range(uint32_t property, uint32_t how, uint32_t object, int32_t min, int32_t max) = 0;
    virtual Hresult get_property_range(uint32_t property, uint32_t how, uint32_t object, int32_t& min, int32_t& max) = 0;
    virtual Hresult get_property_string(uint32_t property, std::string& value) = 0;

    virtual Hresult build_action_map(ActionFormat&, std::string_view user, uint32_t flags) = 0;
    virtual Hresult set_action_map(const ActionFormat&, std::string_view user, uint32_t flags) = 0;
};

class DirectInput {
public:
    // `foreground`: is this window the foreground window? (cooperative levels). `now_ms`: the clock of event time stamps.
    DirectInput(platform::Input& input, platform::Clock& clock, std::function<bool(uint32_t hwnd)> foreground);
    ~DirectInput();
    std::vector<DeviceInstance> enum_devices(uint32_t dev_type_or_class, uint32_t flags);
    // The devices that can satisfy an action format (its genre and semantics): keyboards and mice for their semantics, game controllers for the others.
    std::vector<DeviceInstance> enum_devices_by_semantics(const ActionFormat&, uint32_t flags);
    Hresult get_device_status(const Guid& instance);                           // hr::ok when attached, hr::false_ when not
    Hresult create_device(const Guid& instance, std::shared_ptr<Device>& out);
    Hresult run_control_panel(uint32_t) { return hr::unsupported; }
    void pump();                                                               // lets the platform input deliver its events (calls platform::Input::pump)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reference data formats (c_dfDIKeyboard, c_dfDIMouse, c_dfDIMouse2, c_dfDIJoystick, c_dfDIJoystick2): what a game passes to SetDataFormat.
DataFormat standard_keyboard_format();
DataFormat standard_mouse_format(bool with_8_buttons);
DataFormat standard_joystick_format(bool version2);

// HID usage (platform scancode) <-> DIK_* scan code.
uint32_t dik_of_hid(uint32_t hid);

}  // namespace nue::legacy::dinput
