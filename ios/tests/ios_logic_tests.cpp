// Host tests for the iOS backend's pure logic. No Apple headers, no game data. Run through ctest.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "audio_policy.hpp"
#include "caps.hpp"
#include "input_logic.hpp"
#include "lifecycle.hpp"
#include "power.hpp"
#include "storage.hpp"

using namespace nue::ios;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) CHECK(std::fabs((a) - (b)) <= (eps))

static void test_lifecycle() {
    using S = LifecycleState; using I = LifecycleInput;
    auto t = lifecycle_step(S::not_running, I::launched);
    CHECK(t.accepted && t.next == S::inactive);
    t = lifecycle_step(S::inactive, I::did_become_active);
    CHECK(t.next == S::active && (t.actions & act_resume_game) && (t.actions & act_resume_gpu_submission) && (t.actions & act_resume_audio));
    t = lifecycle_step(S::active, I::will_resign_active);
    CHECK(t.next == S::inactive && (t.actions & act_pause_game) && !(t.actions & act_stop_gpu_submission));
    t = lifecycle_step(S::inactive, I::did_enter_background);
    CHECK(t.next == S::background && (t.actions & act_stop_gpu_submission) && (t.actions & act_save_state) && (t.actions & act_release_caches));
    t = lifecycle_step(S::background, I::will_enter_foreground);
    CHECK(t.next == S::inactive && (t.actions & act_refresh_drawable_size));
    // Direct active -> background (scene notifications may skip resign) still stops the GPU and pauses.
    t = lifecycle_step(S::active, I::did_enter_background);
    CHECK(t.next == S::background && (t.actions & act_pause_game) && (t.actions & act_stop_gpu_submission));
    // Out-of-order foreground handling does not freeze the game.
    t = lifecycle_step(S::background, I::did_become_active);
    CHECK(t.accepted && t.next == S::active && (t.actions & act_refresh_drawable_size));
    // Nonsense is rejected without side effects.
    t = lifecycle_step(S::background, I::will_resign_active);
    CHECK(!t.accepted && t.next == S::background && t.actions == 0);
    t = lifecycle_step(S::not_running, I::memory_warning);
    CHECK(!t.accepted);
    // Memory warning keeps the state in every running state.
    for (S s : {S::inactive, S::active, S::background}) {
        t = lifecycle_step(s, I::memory_warning);
        CHECK(t.accepted && t.next == s && (t.actions & act_emit_low_memory) && (t.actions & act_release_caches));
    }
    t = lifecycle_step(S::active, I::will_terminate);
    CHECK(t.next == S::not_running && (t.actions & act_save_state) && (t.actions & act_flush_logs));
    // Exhaustive: a rejected transition never changes state and never asks for actions.
    for (int s = 0; s < 4; ++s)
        for (int i = 0; i < 7; ++i) {
            auto r = lifecycle_step(static_cast<S>(s), static_cast<I>(i));
            if (!r.accepted) CHECK(r.next == static_cast<S>(s) && r.actions == 0);
            // The GPU can never be told to resume without also resuming the game.
            if (r.actions & act_resume_gpu_submission) CHECK(r.actions & act_resume_game);
        }
    CHECK(std::strcmp(lifecycle_state_name(S::background), "background") == 0);
}

static void test_power() {
    CHECK(thermal_from_raw(0) == ThermalLevel::nominal);
    CHECK(thermal_from_raw(2) == ThermalLevel::serious);
    CHECK(thermal_from_raw(3) == ThermalLevel::critical);
    CHECK(thermal_from_raw(99) == ThermalLevel::critical);   // unknown -> safe side

    CHECK(classify_memory(1000ull << 20) == MemoryPressure::normal);
    CHECK(classify_memory(399ull << 20) == MemoryPressure::moderate);
    CHECK(classify_memory(249ull << 20) == MemoryPressure::high);
    CHECK(classify_memory(119ull << 20) == MemoryPressure::critical);
    CHECK(classify_memory(0) == MemoryPressure::critical);

    auto a = advise_quality(ThermalLevel::nominal, false, 60);
    CHECK(a.max_fps == 60 && a.resolution_scale_percent == 100 && !a.reduce_effects && !a.warn_player);
    a = advise_quality(ThermalLevel::fair, false, 60);
    CHECK(a.max_fps == 60 && a.resolution_scale_percent == 100);
    a = advise_quality(ThermalLevel::serious, false, 60);
    CHECK(a.max_fps == 30 && a.resolution_scale_percent < 100 && a.reduce_effects && !a.warn_player);
    a = advise_quality(ThermalLevel::critical, false, 60);
    CHECK(a.warn_player && a.resolution_scale_percent <= 70);
    a = advise_quality(ThermalLevel::nominal, true, 60);
    CHECK(a.max_fps == 30 && a.reduce_effects);
    // A game that wants 30 never gets raised.
    a = advise_quality(ThermalLevel::nominal, false, 30);
    CHECK(a.max_fps == 30);
    // Quality never rises as conditions worsen.
    ThermalLevel order[] = {ThermalLevel::nominal, ThermalLevel::fair, ThermalLevel::serious, ThermalLevel::critical};
    for (int i = 0; i + 1 < 4; ++i) {
        auto x = advise_quality(order[i], false, 60), y = advise_quality(order[i + 1], false, 60);
        CHECK(y.max_fps <= x.max_fps && y.resolution_scale_percent <= x.resolution_scale_percent);
    }

    auto r = choose_frame_rate(120, 60);
    CHECK(r.minimum == 60 && r.preferred == 60 && r.maximum == 60);
    r = choose_frame_rate(60, 120);   // cap above the display: clamp to the display
    CHECK(r.maximum == 60);
    r = choose_frame_rate(120, 0);
    CHECK(r.maximum == 120);
}

static void test_caps() {
    static_assert(kTexFormatCount == 23, "TexFormat must mirror nue::gpu::Format (23 entries); update both together");
    MetalProbe a15; a15.highest_apple_family = 8; a15.physical_memory = 6ull << 30;
    auto c = derive_caps(a15);
    CHECK(!c.texture_format[static_cast<size_t>(TexFormat::bc1)]);
    CHECK(!c.texture_format[static_cast<size_t>(TexFormat::bc3)]);
    CHECK(c.texture_format[static_cast<size_t>(TexFormat::astc4x4)]);
    CHECK(!c.texture_format[static_cast<size_t>(TexFormat::d24s8)] && c.texture_format[static_cast<size_t>(TexFormat::d32f)]);
    CHECK(!c.depth24_stencil8);
    CHECK(c.max_texture_size == 16384 && !c.origin_bottom_left && !c.homogeneous_depth);
    CHECK(c.texture_format[static_cast<size_t>(TexFormat::r5g6b5)]);

    MetalProbe a7; a7.highest_apple_family = 1;
    auto old = derive_caps(a7);
    CHECK(!old.texture_format[static_cast<size_t>(TexFormat::astc4x4)] && old.max_texture_size == 8192);

    MetalProbe mac; mac.highest_apple_family = 7; mac.supports_bc = true;
    auto m = derive_caps(mac);
    CHECK(m.texture_format[static_cast<size_t>(TexFormat::bc1)] && m.depth24_stencil8);

    // Policy
    auto pol = choose_texture_policy(c, 400ull << 20, 100ull << 20);
    CHECK(pol.preferred == TextureStorage::astc4x4);
    pol = choose_texture_policy(c, 150ull << 20, 100ull << 20);   // 2x100 > 150: shrink
    CHECK(pol.preferred == TextureStorage::astc6x6);
    pol = choose_texture_policy(c, 0, 0);
    CHECK(pol.preferred == TextureStorage::astc4x4);
    CHECK(choose_texture_policy(m, 0, 0).preferred == TextureStorage::native_bc);
    CHECK(choose_texture_policy(old, 0, 0).preferred == TextureStorage::rgba8);

    CHECK(storage_for_texture(c, pol, TexFormat::bc1, 512, 512) == TextureStorage::astc4x4);
    CHECK(storage_for_texture(c, pol, TexFormat::bc3, 16, 16) == TextureStorage::rgba8);   // tiny: exact
    CHECK(storage_for_texture(c, pol, TexFormat::rgba8, 512, 512) == TextureStorage::rgba8);
    CHECK(storage_for_texture(m, choose_texture_policy(m, 0, 0), TexFormat::bc1, 512, 512) == TextureStorage::native_bc);
    CHECK(storage_bytes(TextureStorage::astc4x4, 256, 256) == 64ull * 64 * 16);
    CHECK(storage_bytes(TextureStorage::astc4x4, 256, 256) == 2 * storage_bytes(TextureStorage::native_bc, 256, 256));   // 2x BC1
    CHECK(storage_bytes(TextureStorage::astc6x6, 256, 256) < storage_bytes(TextureStorage::astc4x4, 256, 256));
    CHECK(storage_bytes(TextureStorage::rgba8, 4, 4) == 64);

    auto b = plan_memory_budget(4ull << 30, 0);
    CHECK(b.hard_limit == 2048ull << 20 && b.soft_limit < b.hard_limit && b.texture_share < b.soft_limit);
    b = plan_memory_budget(3ull << 30, 0);   // 55 % of 3 GB = 1.65 GB < 2 GB goal
    CHECK(b.hard_limit == (3ull << 30) * 55 / 100);
    b = plan_memory_budget(6ull << 30, 900ull << 20);   // OS says only 900 MB left
    CHECK(b.hard_limit == 900ull << 20);
}

static void test_paths() {
    CHECK(normalize_relative_path("a/b/c").value() == "a/b/c");
    CHECK(normalize_relative_path("a\\b\\c").value() == "a/b/c");
    CHECK(normalize_relative_path("./a//b/./c/").value() == "a/b/c");
    CHECK(normalize_relative_path("a/b/../c").value() == "a/c");
    CHECK(normalize_relative_path("/abs/path").value() == "abs/path");
    CHECK(normalize_relative_path("C:\\Games\\x.bin").value() == "Games/x.bin");
    CHECK(!normalize_relative_path("../escape"));
    CHECK(!normalize_relative_path("a/../../escape"));
    CHECK(!normalize_relative_path("a\\..\\..\\escape"));
    CHECK(!normalize_relative_path(std::string("a\0b", 3)));
    CHECK(normalize_relative_path("").value() == "");

    CaseFoldIndex idx;
    idx.add("GLOBAL/GlobalB.lzc");
    idx.add("SPEED2.EXE");
    CHECK(idx.resolve("global\\globalb.LZC").value() == "GLOBAL/GlobalB.lzc");
    CHECK(idx.resolve("./speed2.exe").value() == "SPEED2.EXE");
    CHECK(!idx.resolve("nope"));
    CHECK(!idx.resolve("../SPEED2.EXE"));
    idx.add("speed2.exe");
    CHECK(idx.collisions() == 1);
    CHECK(idx.resolve("SPEED2.EXE").value() == "SPEED2.EXE");   // deterministic: lexicographically first ('S' < 's')

    std::vector<DirectoryEntry> list = {{"SPEED2.EXE", 5000000}, {"GLOBAL/GLOBALB.LZC", 100}, {"Movies/intro.bik", 0}};
    auto rep = validate_game_data(list, {{"speed2.exe", 1000}, {"global/globalb.lzc"}, {"movies/intro.bik"}, {"missing.dat"}});
    CHECK(!rep.ok());
    CHECK(rep.missing.size() == 1 && rep.missing[0] == "missing.dat");
    CHECK(rep.truncated.size() == 1 && rep.truncated[0] == "movies/intro.bik");
    CHECK(rep.total_bytes == 5000100);

    std::vector<DirectoryEntry> deep = {{"NFSU2/Speed2.exe", 10}, {"NFSU2/GLOBAL/a", 1}, {"readme.txt", 1}};
    CHECK(find_game_root(deep, "SPEED2.EXE").value() == "NFSU2/");
    CHECK(!find_game_root(deep, "SPEED2.EXE", 0));
    std::vector<DirectoryEntry> flat = {{"SPEED2.EXE", 1}, {"x/SPEED2.EXE", 1}};
    CHECK(find_game_root(flat, "SPEED2.EXE").value().empty());
    std::vector<DirectoryEntry> tricky = {{"NOTSPEED2.EXE", 1}};
    CHECK(!find_game_root(tricky, "SPEED2.EXE"));

    SandboxLayout lay;
    CHECK(lay.game_root("/D") == "/D/NFSU2");
    CHECK(lay.game_root("/D/") == "/D/NFSU2");
    CHECK(lay.saves_dir("/AS") == "/AS/NUE/saves");
    CHECK(lay.shader_cache_dir("/C") == "/C/NUE/shaders");
}

static void test_import() {
    std::vector<DirectoryEntry> src = {{"A/b.bin", 600ull << 20}, {"c.bin", 600ull << 20}, {"../evil", 5}, {"ok\\d.bin", 1}};
    auto plan = plan_import(src, 2ull << 30);
    CHECK(plan.items.size() == 3 && plan.rejected.size() == 1);
    CHECK(plan.items[2].dest_relative == "ok/d.bin");
    CHECK(plan.total_bytes == (1200ull << 20) + 1);
    CHECK(plan.enough_space);
    CHECK(plan.required_free_bytes > plan.total_bytes);
    plan = plan_import(src, 1200ull << 20);   // exactly the data size: the margin makes it insufficient
    CHECK(!plan.enough_space);
    // A resumed import: what is already copied does not count twice.
    plan = plan_import(src, 700ull << 20, 600ull << 20);
    CHECK(plan.enough_space);

    Progress p; p.total = 200; p.done = 50;
    CHECK(p.percent() == 25);
    p.done = 999;
    CHECK(p.percent() == 100);
    CHECK(Progress{}.percent() == 100);
}

static void test_crc() {
    // Standard check value of CRC-32/ISO-HDLC.
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    Crc32 c; c.update("1234", 4); c.update("56789", 5);
    CHECK(c.value() == 0xCBF43926u);
    CHECK(crc32("", 0) == 0);
}

static void test_audio() {
    auto c = session_config_for(AudioMixPolicy::exclusive);
    CHECK(c.category == SessionCategory::playback && !c.mix_with_others);
    c = session_config_for(AudioMixPolicy::respect_silent);
    CHECK(c.category == SessionCategory::solo_ambient);
    c = session_config_for(AudioMixPolicy::duck_others);
    CHECK(c.category == SessionCategory::playback && c.mix_with_others && c.duck_others);   // ducking requires mixing
    for (auto p : {AudioMixPolicy::exclusive, AudioMixPolicy::respect_silent, AudioMixPolicy::mix_with_others, AudioMixPolicy::duck_others}) {
        auto x = session_config_for(p);
        CHECK(!x.duck_others || x.mix_with_others);
    }
    CHECK(on_interruption(InterruptionPhase::began, false, true) == AudioAction::pause);
    CHECK(on_interruption(InterruptionPhase::ended, true, true) == AudioAction::resume);
    CHECK(on_interruption(InterruptionPhase::ended, false, true) == AudioAction::none);
    CHECK(on_interruption(InterruptionPhase::ended, true, false) == AudioAction::none);   // not active: wait for did_become_active
    CHECK(route_change_from_raw(2) == RouteChange::device_removed);
    CHECK(on_route_change(route_change_from_raw(2)) == AudioAction::pause);
    CHECK(on_route_change(route_change_from_raw(1)) == AudioAction::none);
    CHECK(route_change_from_raw(42) == RouteChange::other);
    CHECK(preferred_io_buffer_seconds(RouteKind::wired, false) < preferred_io_buffer_seconds(RouteKind::airplay, false));
    CHECK(preferred_io_buffer_seconds(RouteKind::wired, true) >= 0.010);
    CHECK(preferred_sample_rate(RouteKind::bluetooth_a2dp) == 0 && preferred_sample_rate(RouteKind::built_in_speaker) == 48000.0);
}

static void test_input() {
    CHECK(touch_phase_from_raw(0) == TouchPhase::down && touch_phase_from_raw(3) == TouchPhase::up && touch_phase_from_raw(4) == TouchPhase::cancel);
    CHECK(touch_phase_from_raw(7) == TouchPhase::hover_exit && touch_phase_from_raw(-1) == TouchPhase::ignored);
    float nx, ny;
    normalize_point(50, 200, 100, 100, nx, ny);
    CHECK_NEAR(nx, 0.5f, 1e-6f); CHECK_NEAR(ny, 1.0f, 1e-6f);
    normalize_point(-5, 5, 0, 0, nx, ny);
    CHECK(nx == 0 && ny == 0);

    PadTuning t;
    float ox, oy;
    condition_stick(0.05f, 0.05f, t, ox, oy);
    CHECK(ox == 0 && oy == 0);
    condition_stick(1.0f, 0.0f, t, ox, oy);
    CHECK_NEAR(ox, 1.0f, 1e-6f); CHECK_NEAR(oy, 0.0f, 1e-6f);
    // Continuity at the edge of the deadzone and direction preserved.
    condition_stick(t.stick_deadzone + 0.001f, 0, t, ox, oy);
    CHECK(ox > 0 && ox < 0.01f);
    condition_stick(-0.5f, 0.5f, t, ox, oy);
    CHECK(ox < 0 && oy > 0 && std::fabs(ox + oy) < 1e-6f);
    CHECK(std::sqrt(ox * ox + oy * oy) <= 1.0f);
    CHECK(condition_trigger(0.02f, t) == 0 && condition_trigger(1.0f, t) == 1.0f);

    PadState raw; raw.left_trigger = 0.6f; raw.right_trigger = 0.3f; raw.buttons = pad_a | pad_right_trigger_digital;
    auto s = condition_pad(raw);
    CHECK((s.buttons & pad_left_trigger_digital) && !(s.buttons & pad_right_trigger_digital) && (s.buttons & pad_a));

    CHECK(pad_family_from_product_category("Xbox One") == PadFamily::xbox);
    CHECK(pad_family_from_product_category("DualSense") == PadFamily::playstation);
    CHECK(pad_family_from_product_category("DualShock 4") == PadFamily::playstation);
    CHECK(pad_family_from_product_category("Switch Pro Controller") == PadFamily::nintendo);
    CHECK(pad_family_from_product_category(nullptr) == PadFamily::generic);

    // Steering: build gravity for a wheel rotated clockwise by `deg` in every orientation; result must be +, mirrored must be -.
    const float kRad = 3.14159265358979f / 180.0f;
    struct O { int id; float dx, dy, rx, ry; } os[] = {{1, 0, -1, 1, 0}, {2, 0, 1, -1, 0}, {3, -1, 0, 0, -1}, {4, 1, 0, 0, 1}};
    for (auto& o : os) {
        for (float deg : {0.0f, 10.0f, 20.0f, 45.0f, 80.0f}) {
            float a = deg * kRad;
            // gravity = d*cos(a) + r*sin(a)  (clockwise rotation of the device = gravity rotates towards UI-right in device frame)
            float gx = o.dx * std::cos(a) + o.rx * std::sin(a), gy = o.dy * std::cos(a) + o.ry * std::sin(a);
            float sp = steering_from_gravity(gx, gy, 0.0f, o.id);
            float sn = steering_from_gravity(o.dx * std::cos(a) - o.rx * std::sin(a), o.dy * std::cos(a) - o.ry * std::sin(a), 0.0f, o.id);
            if (deg <= 4.0f) CHECK(sp == 0 && sn == 0);
            else { CHECK(sp > 0 && sn < 0); CHECK_NEAR(sp, -sn, 1e-5f); }
            if (deg >= 45.0f) CHECK_NEAR(sp, 1.0f, 1e-5f);   // clamped at full lock
        }
        // Flat on the table: no steering.
        CHECK(steering_from_gravity(0.02f, -0.03f, -0.99f, o.id) == 0);
    }
    // Monotonic between deadzone and full lock.
    float prev = 0;
    for (int deg = 0; deg <= 40; deg += 2) {
        float a = deg * kRad;
        float v = steering_from_gravity(std::sin(a), -std::cos(a), 0, 1);
        CHECK(v >= prev - 1e-6f);
        prev = v;
    }

    LastUsedInput li;
    CHECK(li.current() == InputDevice::none);
    li.tick(1000);
    CHECK(li.note(InputDevice::touch));
    CHECK(!li.note(InputDevice::touch));
    CHECK(!li.note(InputDevice::gamepad, false));   // stick noise is ignored
    CHECK(li.current() == InputDevice::touch);
    CHECK(li.note(InputDevice::gamepad));
    CHECK(!li.note_motion(0.9f, 1500));           // too soon after another device
    CHECK(li.note_motion(0.9f, 4000));            // quiet for >= 2 s
    CHECK(li.current() == InputDevice::motion);
    CHECK(!li.note_motion(0.1f, 9000));
    li.tick(9500);
    CHECK(li.note(InputDevice::keyboard) && li.current() == InputDevice::keyboard);
}

int main() {
    test_lifecycle();
    test_power();
    test_caps();
    test_paths();
    test_import();
    test_crc();
    test_audio();
    test_input();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
