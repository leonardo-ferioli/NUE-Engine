#include "nue_legacy/dinput.hpp"
#include <algorithm>
#include <cstring>
#include <set>

namespace nue::legacy::dinput {

Guid make_guid(uint32_t d1, uint16_t d2, uint16_t d3, std::array<uint8_t, 8> d4) {
    Guid g;
    std::memcpy(&g.bytes[0], &d1, 4);       // the in-memory layout of a Windows GUID (little endian fields)
    std::memcpy(&g.bytes[4], &d2, 2);
    std::memcpy(&g.bytes[6], &d3, 2);
    std::memcpy(&g.bytes[8], d4.data(), 8);
    return g;
}

const Guid kGuidSysKeyboard = make_guid(0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidSysMouse = make_guid(0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidXAxis = make_guid(0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidYAxis = make_guid(0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidZAxis = make_guid(0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidRxAxis = make_guid(0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidRyAxis = make_guid(0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidRzAxis = make_guid(0xA36D02E3, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidSlider = make_guid(0xA36D02E4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidButton = make_guid(0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidKey = make_guid(0x55728220, 0xD33C, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});
const Guid kGuidPov = make_guid(0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00});

namespace {

constexpr uint32_t kPovCentered = 0xFFFFFFFFu;
constexpr uint32_t kAxisCount = 8;                    // X Y Z Rx Ry Rz Slider0 Slider1
const Guid* const kAxisGuids[kAxisCount] = {&kGuidXAxis, &kGuidYAxis, &kGuidZAxis, &kGuidRxAxis, &kGuidRyAxis, &kGuidRzAxis, &kGuidSlider, &kGuidSlider};
const char* const kAxisNames[kAxisCount] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider"};

// DIK_* of a HID usage (0 = none). The DIK codes are the set-1 scan codes, with 0x80 added for the extended keys.
uint32_t dik_table(uint32_t hid) {
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    if (hid >= 4 && hid <= 29) return letters[hid - 4];
    if (hid >= 30 && hid <= 38) return 0x02 + (hid - 30);
    if (hid == 39) return 0x0B;
    if (hid >= 58 && hid <= 67) return 0x3B + (hid - 58);          // F1..F10
    if (hid >= 104 && hid <= 106) return 0x64 + (hid - 104);       // F13..F15
    switch (hid) {
    case 40: return 0x1C; case 41: return 0x01; case 42: return 0x0E; case 43: return 0x0F; case 44: return 0x39; case 45: return 0x0C; case 46: return 0x0D; case 47: return 0x1A; case 48: return 0x1B;
    case 49: return 0x2B; case 51: return 0x27; case 52: return 0x28; case 53: return 0x29; case 54: return 0x33; case 55: return 0x34; case 56: return 0x35; case 57: return 0x3A;
    case 68: return 0x57; case 69: return 0x58; case 70: return 0xB7; case 71: return 0x46; case 72: return 0xC5; case 73: return 0xD2; case 74: return 0xC7; case 75: return 0xC9; case 76: return 0xD3; case 77: return 0xCF;
    case 78: return 0xD1; case 79: return 0xCD; case 80: return 0xCB; case 81: return 0xD0; case 82: return 0xC8;
    case 83: return 0x45; case 84: return 0xB5; case 85: return 0x37; case 86: return 0x4A; case 87: return 0x4E; case 88: return 0x9C;
    case 89: return 0x4F; case 90: return 0x50; case 91: return 0x51; case 92: return 0x4B; case 93: return 0x4C; case 94: return 0x4D; case 95: return 0x47; case 96: return 0x48; case 97: return 0x49;
    case 98: return 0x52; case 99: return 0x53; case 101: return 0xDD;
    case 224: return 0x1D; case 225: return 0x2A; case 226: return 0x38; case 227: return 0xDB; case 228: return 0x9D; case 229: return 0x36; case 230: return 0xB8; case 231: return 0xDC;
    default: return 0;
    }
}

const char* dik_name(uint32_t dik) {
    switch (dik) {
    case 0x01: return "Esc"; case 0x0E: return "Backspace"; case 0x0F: return "Tab"; case 0x1C: return "Enter"; case 0x1D: return "Ctrl"; case 0x2A: return "Shift"; case 0x36: return "Right Shift";
    case 0x38: return "Alt"; case 0x39: return "Space"; case 0x3A: return "Caps Lock"; case 0x45: return "Num Lock"; case 0x46: return "Scroll Lock"; case 0x9D: return "Right Ctrl"; case 0xB8: return "Right Alt";
    case 0xC8: return "Up Arrow"; case 0xD0: return "Down Arrow"; case 0xCB: return "Left Arrow"; case 0xCD: return "Right Arrow"; case 0xD2: return "Insert"; case 0xD3: return "Delete";
    case 0xC7: return "Home"; case 0xCF: return "End"; case 0xC9: return "Page Up"; case 0xD1: return "Page Down";
    default: return nullptr;
    }
}

// The device-independent description of an object a device exposes.
struct NativeObject {
    Guid guid;
    uint32_t type = 0;              // DIDFT_* | instance
    uint32_t native_offset = 0;
    std::string name;
    uint32_t kind = 0;              // 0 axis, 1 button, 2 pov
    uint32_t index = 0;             // axis, button or pov number
};

enum class Kind { keyboard, mouse, joystick };

}  // namespace

uint32_t dik_of_hid(uint32_t hid) { return dik_table(hid); }

DataFormat standard_keyboard_format() {
    DataFormat f;
    f.data_size = 256;
    for (uint32_t i = 0; i < 256; ++i) f.objects.push_back({kGuidKey, i, didft::optional | didft::button | make_instance(i), 0, true});
    return f;
}

DataFormat standard_mouse_format(bool eight_buttons) {
    DataFormat f;
    f.flags = datafmt::relative_axes;
    f.data_size = eight_buttons ? 20 : 16;
    f.objects.push_back({kGuidXAxis, 0, didft::optional | didft::axis | didft::any_instance, 0, true});
    f.objects.push_back({kGuidYAxis, 4, didft::optional | didft::axis | didft::any_instance, 0, true});
    f.objects.push_back({kGuidZAxis, 8, didft::optional | didft::axis | didft::any_instance, 0, true});
    for (uint32_t i = 0; i < (eight_buttons ? 8u : 4u); ++i) f.objects.push_back({Guid{}, 12 + i, didft::optional | didft::button | didft::any_instance, 0, false});
    return f;
}

DataFormat standard_joystick_format(bool version2) {
    DataFormat f;
    f.flags = datafmt::absolute_axes;
    f.data_size = version2 ? 272 : 80;
    uint32_t offset = 0;
    for (uint32_t a = 0; a < 6; ++a, offset += 4) f.objects.push_back({*kAxisGuids[a], offset, didft::optional | didft::axis | didft::any_instance, 0, true});
    for (uint32_t s = 0; s < 2; ++s, offset += 4) f.objects.push_back({kGuidSlider, offset, didft::optional | didft::axis | didft::any_instance, 0, true});
    for (uint32_t p = 0; p < 4; ++p, offset += 4) f.objects.push_back({kGuidPov, offset, didft::optional | didft::pov | didft::any_instance, 0, true});
    const uint32_t buttons = version2 ? 128 : 32;
    for (uint32_t b = 0; b < buttons; ++b) f.objects.push_back({Guid{}, offset + b, didft::optional | didft::button | didft::any_instance, 0, false});
    return f;
}

// ------------------------------------------------------------------------------------------------------------------------------ devices
namespace {

struct Shared;   // the DirectInput object's shared state (clock, input, foreground callback)

class DeviceBase : public Device, public std::enable_shared_from_this<DeviceBase> {
public:
    DeviceBase(Kind k, DeviceInstance inst, platform::Input& input, platform::Clock& clock, std::function<bool(uint32_t)> fg) : kind_(k), instance_(std::move(inst)), input_(input), clock_(clock), foreground_(std::move(fg)) {
        build_objects();
        axis_min_.fill(k == Kind::joystick ? 0 : INT32_MIN);
        axis_max_.fill(k == Kind::joystick ? 65535 : INT32_MAX);
        deadzone_.fill(0);
        saturation_.fill(10000);
        relative_ = k == Kind::mouse;
    }

    const DeviceInstance& instance() const override { return instance_; }

    Hresult get_capabilities(DeviceCaps& out) override {
        out = DeviceCaps{};
        out.device_type = instance_.device_type;
        out.flags = 1 | (kind_ == Kind::joystick ? 0x2u : 0u);           // DIDC_ATTACHED, DIDC_POLLEDDEVICE (joystick polls)
        for (const auto& o : objects_) { if (o.kind == 0) ++out.axes; else if (o.kind == 1) ++out.buttons; else ++out.povs; }
        return hr::ok;
    }

    std::vector<DeviceObject> objects() const override {
        std::vector<DeviceObject> out;
        for (const auto& o : objects_) { DeviceObject d; d.guid = o.guid; d.offset = o.native_offset; d.type = o.type; d.name = o.name; out.push_back(d); }
        return out;
    }

    Hresult get_object_info(uint32_t object, uint32_t how_, DeviceObject& out) override {
        const NativeObject* o = find_by(how_, object);
        if (!o) return hr::object_not_found;
        out = DeviceObject{};
        out.guid = o->guid; out.offset = o->native_offset; out.type = o->type; out.name = o->name;
        return hr::ok;
    }

    Hresult set_data_format(const DataFormat& f) override {
        std::lock_guard lock(m_);
        if (acquired_) return hr::acquired;
        if (f.data_size == 0 || (f.data_size & 3) != 0) return hr::invalid_param;
        // Each format object is matched to a distinct device object; one that cannot be matched must be optional.
        std::vector<int> object_of_format(f.objects.size(), -1);
        std::vector<bool> used(objects_.size(), false);
        for (size_t i = 0; i < f.objects.size(); ++i) {
            const auto& fo = f.objects[i];
            if (fo.offset >= f.data_size) return hr::invalid_param;
            for (size_t d = 0; d < objects_.size(); ++d) {
                if (used[d]) continue;
                const auto& o = objects_[d];
                if ((fo.type & 0xFF) && !((fo.type & 0xFF) & (o.type & 0xFF))) continue;                              // the type bits must overlap (DIDFT_ALL, 0, matches anything)
                if ((fo.type & didft::instance_mask) != didft::any_instance && instance_of(fo.type) != instance_of(o.type)) continue;   // a given instance must be that instance
                if (fo.has_guid && !fo.guid.is_null() && fo.guid != o.guid) continue;
                object_of_format[i] = int(d);
                used[d] = true;
                break;
            }
            if (object_of_format[i] < 0 && !(fo.type & didft::optional)) return hr::invalid_param;
        }
        format_ = f;
        format_object_.assign(objects_.size(), -1);
        for (size_t i = 0; i < f.objects.size(); ++i) if (object_of_format[i] >= 0) format_object_[size_t(object_of_format[i])] = int(f.objects[i].offset);
        has_format_ = true;
        if (f.flags & datafmt::absolute_axes) relative_ = false;
        if (f.flags & datafmt::relative_axes) relative_ = true;
        action_app_data_.clear();
        return hr::ok;
    }

    Hresult set_cooperative_level(uint32_t hwnd, uint32_t flags) override {
        std::lock_guard lock(m_);
        const bool a = flags & coop::exclusive, b = flags & coop::nonexclusive, c = flags & coop::foreground, d = flags & coop::background;
        if ((a == b) || (c == d)) return hr::invalid_param;               // exactly one of each pair
        if (flags & ~(coop::exclusive | coop::nonexclusive | coop::foreground | coop::background | coop::nowinkey)) return hr::invalid_param;
        if (acquired_) return hr::acquired;
        if (kind_ == Kind::keyboard && a && d) return hr::unsupported;     // a keyboard cannot be exclusive in the background
        if (kind_ == Kind::mouse && a && d) return hr::unsupported;
        hwnd_ = hwnd;
        coop_ = flags;
        has_coop_ = true;
        return hr::ok;
    }

    Hresult acquire() override {
        std::lock_guard lock(m_);
        if (!has_format_ || !has_coop_) return hr::invalid_param;
        if (acquired_) return hr::false_;
        if ((coop_ & coop::foreground) && hwnd_ && !foreground_(hwnd_)) return hr::other_app_has_prio;
        acquired_ = true;
        sync_from_platform();
        return hr::ok;
    }

    Hresult unacquire() override {
        std::lock_guard lock(m_);
        if (!acquired_) return hr::false_;
        acquired_ = false;
        events_.clear();
        overflowed_ = false;
        return hr::ok;
    }

    Hresult poll() override {
        std::lock_guard lock(m_);
        if (!acquired_) return hr::not_acquired;
        if (!lost_check()) return hr::input_lost;
        return kind_ == Kind::joystick ? hr::ok : hr::false_;               // DI_NOEFFECT for a device that is not polled
    }

    Hresult get_device_state(MutableBytes out) override {
        std::lock_guard lock(m_);
        if (!acquired_) return hr::not_acquired;
        if (out.size() != format_.data_size) return hr::invalid_param;
        if (!lost_check()) return hr::input_lost;
        std::memset(out.data(), 0, out.size());
        for (size_t i = 0; i < objects_.size(); ++i) {
            if (format_object_[i] < 0) continue;
            write_object(out, i);
        }
        if (kind_ == Kind::mouse && relative_) { rel_x_ = rel_y_ = rel_z_ = 0; }   // relative axes report the motion since the last read
        return hr::ok;
    }

    Hresult get_device_data(std::vector<DeviceEvent>* out, uint32_t& count, uint32_t flags) override {
        std::lock_guard lock(m_);
        if (flags & ~gdd::peek) return hr::invalid_param;
        if (!acquired_) return hr::not_acquired;
        if (capacity_ == 0) return hr::not_buffered;
        if (!lost_check()) return hr::input_lost;
        const uint32_t available = uint32_t(events_.size());
        const uint32_t take = std::min(count, available);
        if (out) { out->assign(events_.begin(), events_.begin() + take); }
        const bool overflow = overflowed_;
        if (!(flags & gdd::peek)) {
            events_.erase(events_.begin(), events_.begin() + take);
            overflowed_ = false;
        }
        count = take;
        return overflow ? hr::false_ : hr::ok;                               // DI_BUFFEROVERFLOW
    }

    // ---- properties
    Hresult set_property_dword(uint32_t property, uint32_t how_, uint32_t object, uint32_t value) override {
        std::lock_guard lock(m_);
        switch (property) {
        case prop::buffersize:
            if (how_ != how::device) { return find_by(how_, object) ? hr::unsupported : hr::object_not_found; }
            if (acquired_) return hr::acquired;
            capacity_ = std::min<uint32_t>(value, 1u << 16);
            while (capacity_ && events_.size() > capacity_ - 1) events_.pop_front();
            if (!capacity_) events_.clear();
            return hr::ok;
        case prop::axismode:
            if (how_ != how::device) return hr::invalid_param;
            if (value != axismode::absolute && value != axismode::relative) return hr::invalid_param;
            if (kind_ == Kind::keyboard) return hr::unsupported;
            if (acquired_) return hr::acquired;
            relative_ = value == axismode::relative;
            return hr::ok;
        case prop::deadzone: case prop::saturation: {
            if (value > 10000) return hr::invalid_param;
            if (kind_ != Kind::joystick) return hr::unsupported;
            auto& table = property == prop::deadzone ? deadzone_ : saturation_;
            if (how_ == how::device) { table.fill(value); return hr::ok; }
            const NativeObject* o = find_by(how_, object);
            if (!o || o->kind != 0) return hr::object_not_found;
            table[o->index] = value;
            return hr::ok;
        }
        case prop::appdata: {
            if (how_ == how::device) return hr::invalid_param;
            const NativeObject* o = find_by(how_, object);
            if (!o) return hr::object_not_found;
            app_data_[o->type] = value;
            return hr::ok;
        }
        default:
            return hr::unsupported;
        }
    }

    Hresult get_property_dword(uint32_t property, uint32_t how_, uint32_t object, uint32_t& value) override {
        std::lock_guard lock(m_);
        switch (property) {
        case prop::buffersize: if (how_ != how::device) return hr::invalid_param; value = capacity_; return hr::ok;
        case prop::axismode: if (how_ != how::device) return hr::invalid_param; value = relative_ ? axismode::relative : axismode::absolute; return hr::ok;
        case prop::granularity: { if (how_ == how::device) return hr::invalid_param; const NativeObject* o = find_by(how_, object); if (!o) return hr::object_not_found; value = 1; return hr::ok; }
        case prop::deadzone: case prop::saturation: {
            if (kind_ != Kind::joystick) return hr::unsupported;
            auto& table = property == prop::deadzone ? deadzone_ : saturation_;
            if (how_ == how::device) { value = table[0]; return hr::ok; }
            const NativeObject* o = find_by(how_, object);
            if (!o || o->kind != 0) return hr::object_not_found;
            value = table[o->index];
            return hr::ok;
        }
        case prop::joystickid: value = instance_.pad ? instance_.pad - 1 : 0; return hr::ok;
        case prop::appdata: { if (how_ == how::device) return hr::invalid_param; const NativeObject* o = find_by(how_, object); if (!o) return hr::object_not_found; auto it = app_data_.find(o->type); value = it == app_data_.end() ? 0 : it->second; return hr::ok; }
        default: return hr::unsupported;
        }
    }

    Hresult set_property_range(uint32_t property, uint32_t how_, uint32_t object, int32_t min, int32_t max) override {
        std::lock_guard lock(m_);
        if (property != prop::range) return hr::unsupported;
        if (min > max) return hr::invalid_param;
        if (kind_ == Kind::keyboard) return hr::unsupported;
        if (acquired_) return hr::acquired;
        if (how_ == how::device) { axis_min_.fill(min); axis_max_.fill(max); return hr::ok; }
        const NativeObject* o = find_by(how_, object);
        if (!o || o->kind != 0) return hr::object_not_found;
        axis_min_[o->index] = min; axis_max_[o->index] = max;
        return hr::ok;
    }

    Hresult get_property_range(uint32_t property, uint32_t how_, uint32_t object, int32_t& min, int32_t& max) override {
        std::lock_guard lock(m_);
        if (property != prop::range && property != prop::physicalrange && property != prop::logicalrange) return hr::unsupported;
        if (kind_ == Kind::keyboard) return hr::unsupported;
        if (how_ == how::device) return hr::invalid_param;
        const NativeObject* o = find_by(how_, object);
        if (!o || o->kind != 0) return hr::object_not_found;
        min = axis_min_[o->index]; max = axis_max_[o->index];
        return hr::ok;
    }

    Hresult get_property_string(uint32_t property, std::string& value) override {
        if (property == prop::instancename) { value = instance_.instance_name; return hr::ok; }
        if (property == prop::productname) { value = instance_.product_name; return hr::ok; }
        return hr::unsupported;
    }

    // ---- action mapping: keyboard and mouse semantics map to their objects; the others stay unmapped (see the hand-over document)
    Hresult build_action_map(ActionFormat& f, std::string_view, uint32_t flags) override {
        std::lock_guard lock(m_);
        if (flags & ~0x3u) return hr::invalid_param;   // DIDBAM_DEFAULT 0, PRESERVE 1, INITIALIZE 3, HWDEFAULTS 4 (accepted loosely)
        for (auto& a : f.actions) {
            if (flags == 3) { a.object_id = 0; a.how = dia::unmapped; a.guid_instance = Guid{}; }
            else if (flags == 1 && a.how != dia::unmapped) continue;
            const uint32_t type_bits = a.semantic & 0xFF000000u;
            const NativeObject* match = nullptr;
            if (kind_ == Kind::keyboard && type_bits == 0x81000000u) {                  // DIKEYBOARD_*: the low byte is the DIK code
                const uint32_t dik = a.semantic & 0xFF;
                for (const auto& o : objects_) if (o.native_offset == dik) { match = &o; break; }
            } else if (kind_ == Kind::mouse && type_bits == 0x82000000u) {              // DIMOUSE_*: axes and buttons
                const uint32_t code = a.semantic & 0x1FF;
                const bool axis = (code & 0x300) == 0x200;
                const uint32_t n = code & 0xFF;
                for (const auto& o : objects_) if ((axis && o.kind == 0 && o.index == n) || (!axis && o.kind == 1 && o.index == n)) { match = &o; break; }
            }
            if (match) {
                a.object_id = match->type;
                a.guid_instance = instance_.instance;
                a.how = dia::app_mapped;
            } else if (flags != 1) {
                a.how = dia::unmapped;
            }
        }
        return hr::ok;
    }

    Hresult set_action_map(const ActionFormat& f, std::string_view, uint32_t) override {
        // The state of an action-mapped device is a DWORD per action, in the order of the format.
        DataFormat data;
        data.flags = f.axis_min < f.axis_max ? datafmt::absolute_axes : 0;
        data.data_size = uint32_t(f.actions.size() * 4);
        std::vector<std::pair<uint32_t, uint32_t>> app;
        for (size_t i = 0; i < f.actions.size(); ++i) {
            const auto& a = f.actions[i];
            if (a.how == dia::unmapped) continue;
            DataFormatObject o;
            o.offset = uint32_t(i * 4);
            o.type = a.object_id;
            data.objects.push_back(o);
            app.emplace_back(uint32_t(i * 4), a.app_data);
        }
        if (data.data_size == 0) return hr::invalid_param;
        const Hresult r = set_data_format(data);
        if (r != hr::ok) return r;
        std::lock_guard lock(m_);
        for (auto& p : app) action_app_data_[p.first] = p.second;
        if (f.buffer_size) { capacity_ = f.buffer_size; }
        return hr::ok;
    }

    // ---- input from the platform (called with the DirectInput object's lock released)
    void on_event(const platform::InputEvent& e) {
        std::lock_guard lock(m_);
        track(e);
        if (!acquired_ || capacity_ == 0 || !has_format_) return;
        generate(e);
    }

protected:
    void build_objects() {
        if (kind_ == Kind::keyboard) {
            for (uint32_t hid = 0; hid < 256; ++hid) {
                const uint32_t dik = dik_table(hid);
                if (!dik) continue;
                NativeObject o;
                o.guid = kGuidKey; o.type = didft::push_button | make_instance(dik); o.native_offset = dik; o.kind = 1; o.index = dik;
                const char* n = dik_name(dik);
                o.name = n ? n : "Key";
                objects_.push_back(o);
            }
            std::sort(objects_.begin(), objects_.end(), [](const NativeObject& a, const NativeObject& b) { return a.native_offset < b.native_offset; });
        } else if (kind_ == Kind::mouse) {
            for (uint32_t a = 0; a < 3; ++a) { NativeObject o; o.guid = *kAxisGuids[a]; o.type = didft::relative_axis | make_instance(a); o.native_offset = a * 4; o.name = kAxisNames[a]; o.kind = 0; o.index = a; objects_.push_back(o); }
            for (uint32_t b = 0; b < 5; ++b) { NativeObject o; o.guid = kGuidButton; o.type = didft::push_button | make_instance(3 + b); o.native_offset = 12 + b; o.name = "Button " + std::to_string(b); o.kind = 1; o.index = b; objects_.push_back(o); }
        } else {
            for (uint32_t a = 0; a < kAxisCount; ++a) { NativeObject o; o.guid = *kAxisGuids[a]; o.type = didft::absolute_axis | make_instance(a); o.native_offset = a * 4; o.name = kAxisNames[a]; o.kind = 0; o.index = a; objects_.push_back(o); }
            for (uint32_t p = 0; p < 4; ++p) { NativeObject o; o.guid = kGuidPov; o.type = didft::pov | make_instance(p); o.native_offset = 32 + p * 4; o.name = "Hat Switch"; o.kind = 2; o.index = p; objects_.push_back(o); }
            for (uint32_t b = 0; b < 32; ++b) { NativeObject o; o.guid = kGuidButton; o.type = didft::push_button | make_instance(b); o.native_offset = 48 + b; o.name = "Button " + std::to_string(b); o.kind = 1; o.index = b; objects_.push_back(o); }
        }
    }

    const NativeObject* find_by(uint32_t how_, uint32_t object) const {
        for (const auto& o : objects_) {
            if (how_ == how::byoffset && o.native_offset == object) return &o;
            if (how_ == how::byid && (o.type & (0xFF | didft::instance_mask)) == (object & (0xFF | didft::instance_mask))) return &o;
            if (how_ == how::byid && (object & didft::instance_mask) == didft::any_instance) { /* any instance of the type */ if ((o.type & 0xFF & object) != 0) return &o; }
        }
        return nullptr;
    }

    bool lost_check() {
        if ((coop_ & coop::foreground) && hwnd_ && !foreground_(hwnd_)) { acquired_ = false; events_.clear(); return false; }
        return true;
    }

    uint32_t now_ms() { return uint32_t(clock_.time_ns() / 1000000); }

    // State kept from the events the device has seen (the platform snapshot is the starting point when the device is acquired).
    void sync_from_platform() {
        rel_x_ = rel_y_ = rel_z_ = 0;
        if (kind_ == Kind::keyboard) { const uint8_t* keys = input_.key_state(); for (uint32_t hid = 0; hid < platform::kScancodeCount; ++hid) { const uint32_t dik = dik_table(hid); if (dik) key_down_[dik] = keys[hid] != 0; } }
        if (kind_ == Kind::mouse) { const uint32_t mask = input_.mouse_state(nullptr, nullptr); for (uint32_t b = 0; b < 5; ++b) buttons_[b] = (mask >> (b == 1 ? 2 : b == 2 ? 1 : b)) & 1; }
        if (kind_ == Kind::joystick) { platform::GamepadState s; if (input_.gamepad_state(instance_.pad, s)) pad_ = s; }
    }

    static uint32_t mouse_button_of(uint32_t platform_button) {   // platform: 1 left, 2 middle, 3 right, 4 X1, 5 X2 -> DirectInput: 0 left, 1 right, 2 middle, 3 X1, 4 X2
        switch (platform_button) { case 1: return 0; case 3: return 1; case 2: return 2; case 4: return 3; case 5: return 4; default: return 0xFF; }
    }

    void track(const platform::InputEvent& e) {
        switch (kind_) {
        case Kind::keyboard: if (e.kind == platform::InputEventKind::key) { const uint32_t dik = dik_table(e.code); if (dik) key_down_[dik] = e.value != 0; } break;
        case Kind::mouse:
            if (e.kind == platform::InputEventKind::mouse_motion) { rel_x_ += e.value; rel_y_ += e.value2; abs_x_ += e.value; abs_y_ += e.value2; }
            else if (e.kind == platform::InputEventKind::mouse_wheel) { rel_z_ += e.value; abs_z_ += e.value; }
            else if (e.kind == platform::InputEventKind::mouse_button) { const uint32_t b = mouse_button_of(e.code); if (b < 5) buttons_[b] = e.value != 0; }
            break;
        case Kind::joystick:
            if (e.pad != instance_.pad) break;
            if (e.kind == platform::InputEventKind::pad_axis && e.code < 8) pad_.axes[e.code] = int16_t(e.value);
            else if (e.kind == platform::InputEventKind::pad_button && e.code < 32) pad_.buttons = e.value ? (pad_.buttons | (1u << e.code)) : (pad_.buttons & ~(1u << e.code));
            else if (e.kind == platform::InputEventKind::pad_hat && e.code < 4) pad_.hats[e.code] = uint8_t(e.value);
            break;
        }
    }

    int32_t joystick_axis_value(uint32_t index) const {
        // The platform axes: 0 left X, 1 left Y, 2 right X, 3 right Y, 4 left trigger, 5 right trigger. DirectInput: X Y Z Rx Ry Rz.
        int32_t raw = 0;
        switch (index) {
        case 0: raw = pad_.axes[0]; break;
        case 1: raw = pad_.axes[1]; break;
        case 2: raw = int32_t(pad_.axes[5]) - int32_t(pad_.axes[4]); break;           // Z: right trigger minus left trigger (XInput through DirectInput)
        case 3: raw = pad_.axes[2]; break;
        case 4: raw = pad_.axes[3]; break;
        default: raw = 0; break;
        }
        raw = std::clamp(raw, -32768, 32767);
        int32_t normalized = raw * 10000 / 32768;                                     // -10000 .. 10000
        const int32_t dz = int32_t(deadzone_[index]), sat = int32_t(saturation_[index]);
        if (std::abs(normalized) < dz) normalized = 0;
        if (std::abs(normalized) > sat) normalized = normalized < 0 ? -10000 : 10000;
        const int64_t lo = axis_min_[index], hi = axis_max_[index];
        if (index >= 6) return int32_t((lo + hi) / 2);
        return int32_t((lo + hi) / 2 + normalized * (hi - lo) / 20000);
    }

    uint32_t pov_value(uint32_t index) const {
        const uint8_t mask = index < 4 ? pad_.hats[index] : 0;
        if (!mask) return kPovCentered;
        const bool up = mask & 1, right = mask & 2, down = mask & 4, left = mask & 8;
        if (up && right) return 4500; if (right && down) return 13500; if (down && left) return 22500; if (left && up) return 31500;
        if (up) return 0; if (right) return 9000; if (down) return 18000; if (left) return 27000;
        return kPovCentered;
    }

    void put32(MutableBytes out, uint32_t offset, uint32_t v) { if (offset + 4 <= out.size()) std::memcpy(out.data() + offset, &v, 4); }
    void write_object(MutableBytes out, size_t i) {
        const NativeObject& o = objects_[i];
        const uint32_t offset = uint32_t(format_object_[i]);
        if (o.kind == 1) {
            bool down = false;
            if (kind_ == Kind::keyboard) down = key_down_[o.native_offset];
            else if (kind_ == Kind::mouse) down = buttons_[o.index];
            else down = (pad_.buttons >> o.index) & 1;
            if (offset < out.size()) out[offset] = down ? std::byte{0x80} : std::byte{0};
        } else if (o.kind == 2) {
            put32(out, offset, pov_value(o.index));
        } else if (kind_ == Kind::mouse) {
            const int32_t v = relative_ ? (o.index == 0 ? rel_x_ : o.index == 1 ? rel_y_ : rel_z_) : (o.index == 0 ? abs_x_ : o.index == 1 ? abs_y_ : abs_z_);
            put32(out, offset, uint32_t(v));
        } else {
            put32(out, offset, uint32_t(joystick_axis_value(o.index)));
        }
    }

    // Appends a buffered event for the object a platform event concerns.
    void push(uint32_t native_offset_key, uint32_t data, uint32_t timestamp_ns_hint) {
        (void)timestamp_ns_hint;
        // `native_offset_key` is the index of the device object; its format offset says where it sits in the state.
        const int offset = format_object_[native_offset_key];
        if (offset < 0) return;
        DeviceEvent ev;
        ev.offset = uint32_t(offset);
        ev.data = data;
        ev.timestamp = now_ms();
        ev.sequence = ++sequence_;
        auto it = action_app_data_.find(ev.offset);
        if (it != action_app_data_.end()) ev.app_data = it->second;
        else { auto a = app_data_.find(objects_[native_offset_key].type); if (a != app_data_.end()) ev.app_data = a->second; }
        if (events_.size() + 1 >= capacity_) { events_.pop_front(); overflowed_ = true; }   // the ring keeps one slot free; the oldest event is lost
        events_.push_back(ev);
    }

    size_t index_of(uint32_t kind, uint32_t index) const {
        for (size_t i = 0; i < objects_.size(); ++i) if (objects_[i].kind == kind && objects_[i].index == index) return i;
        return SIZE_MAX;
    }

    void generate(const platform::InputEvent& e) {
        switch (kind_) {
        case Kind::keyboard:
            if (e.kind == platform::InputEventKind::key) { const uint32_t dik = dik_table(e.code); const size_t i = dik ? index_of(1, dik) : SIZE_MAX; if (i != SIZE_MAX) push(uint32_t(i), e.value ? 0x80u : 0u, 0); }
            break;
        case Kind::mouse:
            if (e.kind == platform::InputEventKind::mouse_motion) {
                if (relative_) {
                    if (e.value) if (size_t i = index_of(0, 0); i != SIZE_MAX) push(uint32_t(i), uint32_t(e.value), 0);
                    if (e.value2) if (size_t i = index_of(0, 1); i != SIZE_MAX) push(uint32_t(i), uint32_t(e.value2), 0);
                } else {
                    if (e.value) if (size_t i = index_of(0, 0); i != SIZE_MAX) push(uint32_t(i), uint32_t(abs_x_), 0);
                    if (e.value2) if (size_t i = index_of(0, 1); i != SIZE_MAX) push(uint32_t(i), uint32_t(abs_y_), 0);
                }
            } else if (e.kind == platform::InputEventKind::mouse_wheel) {
                if (size_t i = index_of(0, 2); i != SIZE_MAX) push(uint32_t(i), uint32_t(relative_ ? e.value : abs_z_), 0);
            } else if (e.kind == platform::InputEventKind::mouse_button) {
                const uint32_t b = mouse_button_of(e.code);
                if (b < 5) if (size_t i = index_of(1, b); i != SIZE_MAX) push(uint32_t(i), e.value ? 0x80u : 0u, 0);
            }
            break;
        case Kind::joystick:
            if (e.pad != instance_.pad) break;
            if (e.kind == platform::InputEventKind::pad_axis) {
                static const uint32_t di_axis_of_platform[8] = {0, 1, 3, 4, 2, 2, 0xFF, 0xFF};   // left X, left Y, right X, right Y, triggers -> Z
                const uint32_t a = e.code < 8 ? di_axis_of_platform[e.code] : 0xFF;
                if (a != 0xFF) if (size_t i = index_of(0, a); i != SIZE_MAX) push(uint32_t(i), uint32_t(joystick_axis_value(a)), 0);
            } else if (e.kind == platform::InputEventKind::pad_button && e.code < 32) {
                if (size_t i = index_of(1, e.code); i != SIZE_MAX) push(uint32_t(i), e.value ? 0x80u : 0u, 0);
            } else if (e.kind == platform::InputEventKind::pad_hat && e.code < 4) {
                if (size_t i = index_of(2, e.code); i != SIZE_MAX) push(uint32_t(i), pov_value(e.code), 0);
            }
            break;
        }
    }

    Kind kind_;
    DeviceInstance instance_;
    platform::Input& input_;
    platform::Clock& clock_;
    std::function<bool(uint32_t)> foreground_;
    std::mutex m_;
    std::vector<NativeObject> objects_;
    DataFormat format_;
    std::vector<int> format_object_;               // per device object: its offset in the state, or -1
    bool has_format_ = false, has_coop_ = false, acquired_ = false, relative_ = false, overflowed_ = false;
    uint32_t hwnd_ = 0, coop_ = 0, capacity_ = 0, sequence_ = 0;
    std::deque<DeviceEvent> events_;
    std::map<uint32_t, uint32_t> action_app_data_, app_data_;
    std::array<int32_t, kAxisCount> axis_min_{}, axis_max_{};
    std::array<uint32_t, kAxisCount> deadzone_{}, saturation_{};
    bool key_down_[256] = {};
    bool buttons_[8] = {};
    int32_t rel_x_ = 0, rel_y_ = 0, rel_z_ = 0, abs_x_ = 0, abs_y_ = 0, abs_z_ = 0;
    platform::GamepadState pad_;
};

}  // namespace

struct DirectInput::Impl {
    Impl(platform::Input& i, platform::Clock& c, std::function<bool(uint32_t)> fg) : input(i), clock(c), foreground(std::move(fg)) {}
    platform::Input& input;
    platform::Clock& clock;
    std::function<bool(uint32_t)> foreground;
    uint32_t listener = 0;
    std::mutex m;
    std::vector<std::weak_ptr<DeviceBase>> devices;
};

DirectInput::DirectInput(platform::Input& input, platform::Clock& clock, std::function<bool(uint32_t)> foreground) : impl_(std::make_unique<Impl>(input, clock, std::move(foreground))) {
    Impl* impl = impl_.get();
    impl_->listener = input.add_listener([impl](const platform::InputEvent& e) {
        std::vector<std::shared_ptr<DeviceBase>> live;
        {
            std::lock_guard lock(impl->m);
            for (auto it = impl->devices.begin(); it != impl->devices.end();) { if (auto d = it->lock()) { live.push_back(d); ++it; } else it = impl->devices.erase(it); }
        }
        for (auto& d : live) d->on_event(e);
    });
}

DirectInput::~DirectInput() { impl_->input.remove_listener(impl_->listener); }

void DirectInput::pump() { impl_->input.pump(); }

namespace {
Guid pad_guid(platform::GamepadId id) {
    return make_guid(0x4E465532 /* 'NFU2' */, 0x0001, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, uint8_t(id & 0xFF), uint8_t((id >> 8) & 0xFF)});
}
}  // namespace

std::vector<DeviceInstance> DirectInput::enum_devices(uint32_t filter, uint32_t flags) {
    std::vector<DeviceInstance> all;
    DeviceInstance k;
    k.instance = kGuidSysKeyboard; k.product = kGuidSysKeyboard; k.device_type = devtype::keyboard | (4u << 8); k.instance_name = "Keyboard"; k.product_name = "Keyboard";
    all.push_back(k);
    if (impl_->input.mouse_present()) {
        DeviceInstance m;
        m.instance = kGuidSysMouse; m.product = kGuidSysMouse; m.device_type = devtype::mouse | (1u << 8); m.instance_name = "Mouse"; m.product_name = "Mouse";
        all.push_back(m);
    }
    for (const auto& pad : impl_->input.gamepads()) {
        if (!pad.connected && (flags & enumflag::attached_only)) continue;
        DeviceInstance j;
        j.instance = pad_guid(pad.id); j.product = pad_guid(pad.id);
        j.device_type = devtype::gamepad | (2u << 8);
        j.instance_name = pad.name.empty() ? "Controller" : pad.name; j.product_name = j.instance_name; j.pad = pad.id;
        all.push_back(j);
    }
    std::vector<DeviceInstance> out;
    for (auto& d : all) {
        const uint32_t type = d.device_type & 0xFF;
        bool match = false;
        if (filter == devclass::all) match = true;
        else if (filter <= 4) {                                           // a DI8DEVCLASS_*
            if (filter == devclass::pointer) match = type == devtype::mouse;
            else if (filter == devclass::keyboard) match = type == devtype::keyboard;
            else if (filter == devclass::game_controller) match = type >= devtype::joystick;
            else if (filter == devclass::device) match = false;
        } else match = type == (filter & 0xFF);                           // a DI8DEVTYPE_*
        if (match) out.push_back(std::move(d));
    }
    return out;
}

std::vector<DeviceInstance> DirectInput::enum_devices_by_semantics(const ActionFormat& f, uint32_t flags) {
    bool keyboard_semantics = false, mouse_semantics = false, other = false;
    for (const auto& a : f.actions) {
        const uint32_t t = a.semantic & 0xFF000000u;
        if (t == 0x81000000u) keyboard_semantics = true; else if (t == 0x82000000u) mouse_semantics = true; else other = true;
    }
    std::vector<DeviceInstance> out;
    for (auto& d : enum_devices(devclass::all, flags)) {
        const uint32_t type = d.device_type & 0xFF;
        if ((type == devtype::keyboard && keyboard_semantics) || (type == devtype::mouse && mouse_semantics) || (type >= devtype::joystick && other)) out.push_back(std::move(d));
    }
    return out;
}

Hresult DirectInput::get_device_status(const Guid& instance) {
    for (const auto& d : enum_devices(devclass::all, 0)) if (d.instance == instance) return hr::ok;
    return hr::false_;
}

Hresult DirectInput::create_device(const Guid& instance, std::shared_ptr<Device>& out) {
    if (instance.is_null()) return hr::device_not_reg;                       // the real library answers REGDB_E_CLASSNOTREG for a null instance
    for (auto& d : enum_devices(devclass::all, 0)) {
        if (!(d.instance == instance)) continue;
        const uint32_t type = d.device_type & 0xFF;
        auto device = std::make_shared<DeviceBase>(type == devtype::keyboard ? Kind::keyboard : type == devtype::mouse ? Kind::mouse : Kind::joystick, d, impl_->input, impl_->clock, impl_->foreground);
        {
            std::lock_guard lock(impl_->m);
            impl_->devices.push_back(device);
        }
        out = device;
        return hr::ok;
    }
    return hr::device_not_reg;
}

}  // namespace nue::legacy::dinput
