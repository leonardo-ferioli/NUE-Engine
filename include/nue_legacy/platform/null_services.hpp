// Deterministic null implementations of the clock, storage, system and input interfaces (the threads and the config store of the standard
// backend already involve no OS state, so tests use those). No OS call, no real time, no real disk: a Win32-layer test built on these runs
// the same way everywhere.
#pragma once
#include "nue_legacy/platform/clock.hpp"
#include "nue_legacy/platform/input.hpp"
#include "nue_legacy/platform/storage.hpp"
#include "nue_legacy/platform/system.hpp"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace nue::legacy::platform::null {

// A virtual clock: it moves only when told to (`advance_ns`) or when somebody sleeps (a sleep advances it by the slept time at once, so a
// 10-second Sleep costs no wall time). `counter` runs at 10 MHz like the Windows performance counter.
class NullClock final : public Clock {
public:
    explicit NullClock(int64_t start_unix_ms = 946684800000ll /* 2000-01-01 */, int32_t utc_offset_minutes = 0) : unix_start_(start_unix_ms), offset_(utc_offset_minutes) {}
    int64_t time_ns() override { return now_.load(); }
    uint64_t counter() override { return uint64_t(now_.load() / 100); }
    uint64_t counter_frequency() override { return 10000000ull; }
    int64_t unix_ms() override { return unix_start_ + now_.load() / 1000000; }
    int32_t local_utc_offset_minutes(int64_t) override { return offset_; }
    void sleep_ns(int64_t ns) override { if (ns > 0) now_ += ns; slept_ += ns > 0 ? ns : 0; }
    void advance_ns(int64_t ns) { now_ += ns; }
    int64_t total_slept_ns() const { return slept_.load(); }
private:
    std::atomic<int64_t> now_{0}, slept_{0};
    int64_t unix_start_;
    int32_t offset_;
};

// A file system held in memory with the same rules as the standard one (roots, case-insensitive lookup, merged listings). Timestamps come
// from a counter that advances at each change, so listings and stats are reproducible.
class MemoryStorage final : public Storage {
public:
    MemoryStorage();
    ~MemoryStorage() override;
    Status mount(const MountDesc&) override;
    bool has_root(std::string_view root) override;
    bool normalize(std::string_view path, std::string& out) override;
    Result<std::string> resolve(std::string_view path, bool for_write) override;
    Result<FileHandle> open(std::string_view path, uint32_t flags) override;
    Status close(FileHandle) override;
    Status read(FileHandle, MutableBytes out, size_t& read) override;
    Status write(FileHandle, Bytes data, size_t& written) override;
    Status read_at(FileHandle, uint64_t offset, MutableBytes out, size_t& read) override;
    Status write_at(FileHandle, uint64_t offset, Bytes data, size_t& written) override;
    Status seek(FileHandle, int64_t offset, SeekOrigin, uint64_t& position) override;
    Status size(FileHandle, uint64_t& out) override;
    Status set_size(FileHandle, uint64_t) override;
    Status flush(FileHandle) override;
    Status stat(std::string_view path, StatInfo& out) override;
    Status create_directory(std::string_view path) override;
    Status remove(std::string_view path) override;
    Status rename(std::string_view from, std::string_view to) override;
    Status list(std::string_view path, std::vector<DirEntry>& out) override;

    // Test helpers: put a file with content directly.
    void put_file(std::string_view path, std::string_view content);
    std::string get_file(std::string_view path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class NullSystem final : public System {
public:
    SystemInfo info() override;
    SystemInfo value;                                  // what info() returns; tests may change it
    NullSystem();
};

// A scripted input source: tests push events and set snapshots.
class NullInput final : public Input {
public:
    NullInput();
    void pump() override;
    const uint8_t* key_state() override { return keys_; }
    uint32_t mouse_state(float* x, float* y) override { if (x) *x = mouse_x_; if (y) *y = mouse_y_; return buttons_; }
    uint32_t mouse_relative(float* dx, float* dy) override { if (dx) *dx = rel_x_; if (dy) *dy = rel_y_; const uint32_t b = buttons_; rel_x_ = rel_y_ = 0; return b; }
    bool mouse_present() override { return mouse_present_; }
    std::vector<GamepadInfo> gamepads() override { return pads_; }
    bool gamepad_state(GamepadId id, GamepadState& out) override;
    uint32_t add_listener(Listener) override;
    void remove_listener(uint32_t id) override;

    // The one entry a real backend needs: it turns each event its own OS or engine reports (SDL3, NUE's nue_event, an Android or iOS input queue) into an
    // InputEvent (scancodes are USB HID usages, as in SDL and NUE) and pushes it; the snapshots and the listeners are updated at the next `pump`. A pad is
    // announced with add_pad() / remove_pad() so that it has an id. The class is therefore the portable input of every host that has an event pump.
    void push(const InputEvent&);

    // Scripting. Events are queued and delivered (and the snapshots updated) at the next `pump`.
    void key(uint32_t scancode, bool down, int64_t time_ns);
    void mouse_button(uint32_t button, bool down, int64_t time_ns);
    void mouse_move(int32_t dx, int32_t dy, int64_t time_ns);
    void mouse_wheel(int32_t notches, int64_t time_ns);
    GamepadId add_pad(const GamepadInfo&);
    void pad_axis(GamepadId, uint32_t axis, int16_t value, int64_t time_ns);
    void pad_button(GamepadId, uint32_t button, bool down, int64_t time_ns);
    void pad_hat(GamepadId, uint32_t hat, uint8_t mask, int64_t time_ns);
    void remove_pad(GamepadId, int64_t time_ns);

private:
    void deliver(const InputEvent&);
    uint8_t keys_[kScancodeCount] = {};
    float mouse_x_ = 0, mouse_y_ = 0, rel_x_ = 0, rel_y_ = 0;
    uint32_t buttons_ = 0;
    bool mouse_present_ = true;
    std::vector<GamepadInfo> pads_;
    std::map<GamepadId, GamepadState> pad_states_;
    std::vector<InputEvent> queue_;
    std::map<uint32_t, Listener> listeners_;
    uint32_t next_listener_ = 1;
    GamepadId next_pad_ = 1;
    std::mutex m_;
};

}  // namespace nue::legacy::platform::null
