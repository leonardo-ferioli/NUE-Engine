#include <cmath>
#include <limits>
#include <thread>

#include "audio_policy.hpp"
#include "crash_report.hpp"
#include "display_policy.hpp"
#include "event_queue.hpp"
#include "gpu_caps.hpp"
#include "input_classify.hpp"
#include "lifecycle_model.hpp"
#include "memory_budget.hpp"
#include "testing.hpp"
#include "thermal_policy.hpp"

using namespace nue::android;

// ------------------------------------------------------------------------------------------ lifecycle

namespace {
bool has_action(const std::vector<LifeAction>& v, LifeAction a) {
    for (LifeAction x : v)
        if (x == a) return true;
    return false;
}
int index_of(const std::vector<LifeAction>& v, LifeAction a) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == a) return int(i);
    return -1;
}
} // namespace

TEST(lifecycle, normal_start_renders_only_with_surface_and_resume) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    m.handle(LifeEvent::Create, &a);
    m.handle(LifeEvent::Start, &a);
    CHECK(a.empty());
    m.handle(LifeEvent::Resume, &a);
    CHECK(has_action(a, LifeAction::ResumeAudio));
    CHECK(!has_action(a, LifeAction::StartRendering)); // no surface yet
    a.clear();
    m.handle(LifeEvent::SurfaceCreated, 1920, 1080, &a);
    CHECK(has_action(a, LifeAction::StartRendering));
    CHECK(m.can_render());
    CHECK_EQ(m.state().width, 1920);
}

TEST(lifecycle, surface_destroyed_releases_before_anything_else) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    m.handle(LifeEvent::Create, &a);
    m.handle(LifeEvent::Start, &a);
    m.handle(LifeEvent::Resume, &a);
    m.handle(LifeEvent::SurfaceCreated, 1280, 720, &a);
    a.clear();
    m.handle(LifeEvent::SurfaceDestroyed, &a);
    CHECK(a.size() >= 2);
    CHECK_EQ(index_of(a, LifeAction::StopRendering), 0);
    CHECK_EQ(index_of(a, LifeAction::ReleaseSurface), 1);
    CHECK(!m.state().rendering);
}

TEST(lifecycle, pause_saves_and_stops_audio_resume_restores) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    for (LifeEvent e : {LifeEvent::Create, LifeEvent::Start, LifeEvent::Resume})
        m.handle(e, &a);
    m.handle(LifeEvent::SurfaceCreated, 800, 480, &a);
    a.clear();
    m.handle(LifeEvent::Pause, &a);
    CHECK(has_action(a, LifeAction::SaveState));
    CHECK(has_action(a, LifeAction::StopRendering));
    CHECK(has_action(a, LifeAction::SuspendAudio));
    CHECK(index_of(a, LifeAction::SaveState) < index_of(a, LifeAction::StopRendering));
    a.clear();
    m.handle(LifeEvent::Resume, &a);
    CHECK(has_action(a, LifeAction::StartRendering)); // surface survived the pause (multi-window case)
    CHECK(has_action(a, LifeAction::ResumeAudio));
}

TEST(lifecycle, rotation_resizes_without_restarting) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    for (LifeEvent e : {LifeEvent::Create, LifeEvent::Start, LifeEvent::Resume})
        m.handle(e, &a);
    m.handle(LifeEvent::SurfaceCreated, 1080, 1920, &a);
    a.clear();
    m.handle(LifeEvent::SurfaceChanged, 1920, 1080, &a);
    CHECK_EQ(a.size(), size_t(1));
    CHECK(a[0] == LifeAction::ResizeSurface);
    a.clear();
    m.handle(LifeEvent::SurfaceChanged, 1920, 1080, &a); // identical: nothing to do
    CHECK(a.empty());
    m.handle(LifeEvent::ConfigurationChanged, &a);      // fold posture etc: re-query size
    CHECK(has_action(a, LifeAction::ResizeSurface));
}

TEST(lifecycle, focus_loss_pauses_simulation_but_keeps_rendering) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    for (LifeEvent e : {LifeEvent::Create, LifeEvent::Start, LifeEvent::Resume})
        m.handle(e, &a);
    m.handle(LifeEvent::SurfaceCreated, 1280, 720, &a);
    a.clear();
    m.handle(LifeEvent::FocusLost, &a);
    CHECK(has_action(a, LifeAction::PauseSimulation));
    CHECK(m.can_render());
    CHECK(m.state().audio_running);
    a.clear();
    m.handle(LifeEvent::FocusGained, &a);
    CHECK(has_action(a, LifeAction::ResumeSimulation));
}

TEST(lifecycle, audio_focus_loss_silences_and_pauses) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    for (LifeEvent e : {LifeEvent::Create, LifeEvent::Start, LifeEvent::Resume})
        m.handle(e, &a);
    a.clear();
    m.handle(LifeEvent::AudioFocusLost, &a);
    CHECK(has_action(a, LifeAction::SuspendAudio));
    CHECK(has_action(a, LifeAction::PauseSimulation));
    a.clear();
    m.handle(LifeEvent::AudioFocusGained, &a);
    CHECK(has_action(a, LifeAction::ResumeAudio));
    CHECK(has_action(a, LifeAction::ResumeSimulation));
}

TEST(lifecycle, focus_lost_while_paused_does_not_blip_on_resume) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    for (LifeEvent e : {LifeEvent::Create, LifeEvent::Start, LifeEvent::Resume})
        m.handle(e, &a);
    m.handle(LifeEvent::FocusLost, &a);
    m.handle(LifeEvent::Pause, &a);
    a.clear();
    m.handle(LifeEvent::Resume, &a);
    CHECK(!has_action(a, LifeAction::PauseSimulation)); // already paused: no second action
    CHECK(!has_action(a, LifeAction::ResumeSimulation));
    a.clear();
    m.handle(LifeEvent::FocusGained, &a);
    CHECK(has_action(a, LifeAction::ResumeSimulation));
}

TEST(lifecycle, destroy_quits) {
    LifecycleModel m;
    std::vector<LifeAction> a;
    for (LifeEvent e : {LifeEvent::Create, LifeEvent::Start, LifeEvent::Resume})
        m.handle(e, &a);
    m.handle(LifeEvent::SurfaceCreated, 640, 480, &a);
    a.clear();
    m.handle(LifeEvent::Pause, &a);
    m.handle(LifeEvent::Stop, &a);
    m.handle(LifeEvent::Destroy, &a);
    CHECK(has_action(a, LifeAction::Quit));
    CHECK(!m.can_render());
}

// ------------------------------------------------------------------------------------------ memory

TEST(memory, trim_levels_map_to_pressure) {
    CHECK(pressure_from_trim_level(-1) == MemPressure::Critical);
    CHECK(pressure_from_trim_level(5) == MemPressure::Moderate);
    CHECK(pressure_from_trim_level(10) == MemPressure::Low);
    CHECK(pressure_from_trim_level(15) == MemPressure::Critical);
    CHECK(pressure_from_trim_level(20) == MemPressure::Background);
    CHECK(pressure_from_trim_level(40) == MemPressure::Background);
    CHECK(pressure_from_trim_level(80) == MemPressure::Background);
    CHECK(pressure_from_trim_level(0) == MemPressure::None);
}

TEST(memory, budget_follows_configured_cap) {
    SystemMemory s;
    s.total_ram = 12ull << 30;
    s.available_ram = 6ull << 30;
    s.low_threshold = 400ull << 20;
    s.rss = 800ull << 20;
    BudgetConfig cfg;
    cfg.process_cap = 2ull << 30; // an application policy, not an engine constant
    Budget b = compute_budget(s, MemPressure::None, cfg);
    Budget uncapped = compute_budget(s, MemPressure::None);
    CHECK(uncapped.hard_limit > b.hard_limit); // no built-in 2 GiB cap
    CHECK_EQ(b.hard_limit, uint64_t(2ull << 30));
    CHECK(b.soft_limit < b.hard_limit && b.soft_limit > (1ull << 30));
    CHECK(b.texture_budget < b.soft_limit);
    CHECK_EQ(b.bytes_to_release, uint64_t(0));
}

TEST(memory, small_device_gets_a_smaller_budget) {
    SystemMemory s;
    s.total_ram = 3ull << 30;
    s.available_ram = 1200ull << 20;
    s.low_threshold = 300ull << 20;
    s.rss = 500ull << 20;
    Budget b = compute_budget(s, MemPressure::None);
    CHECK(b.hard_limit <= uint64_t(3ull << 30) * 55 / 100);
    CHECK(b.hard_limit <= s.rss + (s.available_ram - s.low_threshold));
}

TEST(memory, pressure_shrinks_and_requests_release) {
    SystemMemory s;
    s.total_ram = 8ull << 30;
    s.available_ram = 4ull << 30;
    s.low_threshold = 400ull << 20;
    s.rss = 1600ull << 20;
    BudgetConfig cfg;
    cfg.process_cap = 2ull << 30;
    Budget none = compute_budget(s, MemPressure::None, cfg);
    Budget low = compute_budget(s, MemPressure::Low, cfg);
    Budget crit = compute_budget(s, MemPressure::Critical, cfg);
    CHECK(low.soft_limit < none.soft_limit && crit.soft_limit < low.soft_limit);
    CHECK(crit.bytes_to_release > 0);
    CHECK_EQ(crit.bytes_to_release, s.rss - crit.soft_limit);
    s.low_memory = true;
    Budget lowmem = compute_budget(s, MemPressure::None);
    CHECK(lowmem.hard_limit <= s.rss); // no growth while the system is in the low state
}

// ------------------------------------------------------------------------------------------ display

TEST(display, exact_refresh_beats_multiples) {
    std::vector<DisplayMode> modes = {{1, 2400, 1080, 60}, {2, 2400, 1080, 90}, {3, 2400, 1080, 120}, {4, 1200, 540, 60}};
    DisplayMode cur = modes[2];
    ModeChoice c = choose_display_mode(modes, cur, 60.0f);
    CHECK_EQ(c.mode_id, 1);
    CHECK(c.exact);
    CHECK_EQ(c.present_interval, 1);
}

TEST(display, multiple_when_no_exact) {
    std::vector<DisplayMode> modes = {{2, 2400, 1080, 90}, {3, 2400, 1080, 120}};
    ModeChoice c = choose_display_mode(modes, modes[1], 60.0f);
    CHECK_EQ(c.mode_id, 3);
    CHECK(!c.exact && c.seamless_multiple);
    CHECK_EQ(c.present_interval, 2);
    ModeChoice c30 = choose_display_mode(modes, modes[0], 30.0f);
    CHECK_EQ(c30.mode_id, 2); // 90 / 3 = 30
    CHECK_EQ(c30.present_interval, 3);
}

TEST(display, never_changes_resolution_and_handles_odd_rates) {
    std::vector<DisplayMode> modes = {{1, 1080, 2400, 144}, {2, 720, 1600, 60}};
    ModeChoice c = choose_display_mode(modes, modes[0], 60.0f);
    CHECK_EQ(c.mode_id, 1);          // only mode at this resolution: closest, not a clean multiple
    CHECK(!c.seamless_multiple && !c.exact);
    CHECK_EQ(c.present_interval, 2); // round(144/60) = 2 -> 72 fps effective: the pacer must drop frames
    ModeChoice none = choose_display_mode({}, modes[0], 60.0f);
    CHECK_EQ(none.mode_id, -1);
    ModeChoice z = choose_display_mode(modes, modes[0], 0.0f);
    CHECK_EQ(z.mode_id, -1);
}

TEST(display, ntsc_rates_count_as_exact) {
    std::vector<DisplayMode> modes = {{1, 1920, 1080, 59.94f}, {2, 1920, 1080, 120.0f}};
    ModeChoice c = choose_display_mode(modes, modes[1], 60.0f);
    CHECK_EQ(c.mode_id, 1);
    CHECK(c.exact);
}

TEST(display, safe_area_and_letterbox) {
    Insets bars{0, 63, 0, 0};
    Insets cutout{96, 0, 0, 0};
    Insets safe = compute_safe_area(bars, cutout);
    CHECK(safe.left == 96 && safe.top == 63 && safe.right == 0 && safe.bottom == 0);
    Insets neg = compute_safe_area({-5, 0, 0, 0}, {0, 0, 0, 0});
    CHECK_EQ(neg.left, 0);

    // 21:9 phone, 4:3 game, no insets: pillarbox
    Rect r = fit_aspect(2400, 1080, {}, 4.0f / 3.0f);
    CHECK_EQ(r.h, 1080);
    CHECK_EQ(r.w, 1440);
    CHECK_EQ(r.x, 480);
    // 16:9 game on a taller window, insets applied first
    Rect r2 = fit_aspect(1080, 1920, {0, 100, 0, 100}, 16.0f / 9.0f);
    CHECK_EQ(r2.w, 1080);
    CHECK(r2.h == 607 || r2.h == 608); // 1080 * 9 / 16 = 607.5
    CHECK(r2.y >= 100 && r2.y + r2.h <= 1820);
    Rect degenerate = fit_aspect(100, 100, {60, 0, 60, 0}, 1.0f);
    CHECK(degenerate.w == 100 && degenerate.h == 100);
}

// ------------------------------------------------------------------------------------------ thermal

TEST(thermal, status_mapping) {
    CHECK(thermal_level_from_status(0) == ThermalLevel::Nominal);
    CHECK(thermal_level_from_status(1) == ThermalLevel::Fair);
    CHECK(thermal_level_from_status(2) == ThermalLevel::Serious);
    CHECK(thermal_level_from_status(3) == ThermalLevel::Serious);
    CHECK(thermal_level_from_status(4) == ThermalLevel::Critical);
    CHECK(thermal_level_from_status(6) == ThermalLevel::Critical);
    CHECK(thermal_level_from_status(-1) == ThermalLevel::Nominal);
}

TEST(thermal, escalates_immediately_and_relaxes_slowly) {
    ThermalGovernor g;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(g.update(0, nan, 0).level == ThermalLevel::Nominal);
    ThermalAdvice a = g.update(3, nan, 1000);
    CHECK(a.level == ThermalLevel::Serious);
    CHECK_EQ(a.fps_cap, 30);
    CHECK(a.quality_scale < 1.0f && a.drop_performance_mode);
    // cooled down, but not long enough
    CHECK(g.update(0, nan, 5000).level == ThermalLevel::Serious);
    CHECK(g.update(0, nan, 20000).level == ThermalLevel::Serious);
    // 20 s after the first cool reading: one step down only
    CHECK(g.update(0, nan, 26000).level == ThermalLevel::Fair);
    CHECK(g.update(0, nan, 30000).level == ThermalLevel::Fair);
    CHECK(g.update(0, nan, 46000).level == ThermalLevel::Nominal);
    CHECK_EQ(g.advice().fps_cap, 0);
}

TEST(thermal, headroom_acts_before_status) {
    ThermalGovernor g;
    CHECK(g.update(0, 0.9f, 0).level == ThermalLevel::Fair);
    CHECK(g.update(0, 1.05f, 100).level == ThermalLevel::Serious);
    // headroom still high: do not relax even after a long time
    CHECK(g.update(0, 0.8f, 100000).level == ThermalLevel::Serious);
    CHECK(g.update(0, 0.5f, 100001).level == ThermalLevel::Serious); // timer starts now
    CHECK(g.update(0, 0.5f, 121001).level == ThermalLevel::Fair);
}

TEST(thermal, adpf_targets) {
    CHECK_NEAR(double(adpf_target_ns(60.0f)), 14166667.0, 1.5);
    CHECK_EQ(adpf_target_ns(30.0f, 1.0f), int64_t(33333333));
    CHECK_EQ(adpf_target_ns(0.0f), int64_t(0));
    AdpfTargetTracker t;
    CHECK(t.needs_update(14166666));
    CHECK(!t.needs_update(14166700));  // under 1 %
    CHECK(t.needs_update(28333333));
    CHECK(!t.needs_update(0));
}

// ------------------------------------------------------------------------------------------ gpu

TEST(gpu, vendor_identification) {
    CHECK(vendor_from_pci_id(0x5143) == GpuVendor::Adreno);
    CHECK(vendor_from_pci_id(0x13B5) == GpuVendor::Mali);
    CHECK(vendor_from_pci_id(0x1010) == GpuVendor::PowerVR);
    CHECK(vendor_from_pci_id(0x144D) == GpuVendor::Xclipse);
    CHECK(vendor_from_pci_id(0x1AE0) == GpuVendor::Software);
    CHECK(vendor_from_pci_id(0x1234) == GpuVendor::Unknown);
    CHECK(vendor_from_name("Adreno (TM) 740") == GpuVendor::Adreno);
    CHECK(vendor_from_name("Mali-G715-Immortalis MC11") == GpuVendor::Mali);
    CHECK(vendor_from_name("Immortalis-G925") == GpuVendor::Mali);
    CHECK(vendor_from_name("PowerVR D-Series DXT-48-1536") == GpuVendor::PowerVR);
    CHECK(vendor_from_name("Samsung Xclipse 940") == GpuVendor::Xclipse);
    CHECK(vendor_from_name("Android Emulator OpenGL ES Translator (NVIDIA GeForce)") == GpuVendor::Software);
    CHECK(vendor_from_name("Google SwiftShader") == GpuVendor::Software);
    CHECK_EQ(adreno_generation("Adreno (TM) 740"), 7);
    CHECK_EQ(adreno_generation("Adreno (TM) 506"), 5);
    CHECK_EQ(adreno_generation("Mali-G78"), 0);
}

TEST(gpu, extension_matching_is_by_token) {
    const char* list = "GL_OES_texture_float GL_EXT_texture_compression_s3tc_srgb GL_KHR_debug";
    CHECK(has_extension(list, "GL_OES_texture_float"));
    CHECK(has_extension(list, "GL_KHR_debug"));
    CHECK(!has_extension(list, "GL_EXT_texture_compression_s3tc")); // only the _srgb variant is present
    CHECK(!has_extension("", "x"));
}

TEST(gpu, gles_caps_and_dxt_plan) {
    // Mali: ETC2 mandatory in ES 3, ASTC yes, S3TC no
    uint32_t mali = tex_caps_from_gles(3, 2, "GL_KHR_texture_compression_astc_ldr GL_EXT_texture_filter_anisotropic");
    CHECK(mali & kTexETC2);
    CHECK(mali & kTexASTC_LDR);
    CHECK(!(mali & kTexBC1_3));
    CHECK(plan_dxt(mali) == DxtStrategy::TranscodeETC2);
    // Adreno: S3TC exposed as extension
    uint32_t adreno = tex_caps_from_gles(3, 2, "GL_EXT_texture_compression_s3tc GL_KHR_texture_compression_astc_ldr");
    CHECK(adreno & kTexBC1_3);
    CHECK(plan_dxt(adreno) == DxtStrategy::NativeBC);
    // ES 2.0 without anything: decode
    CHECK(plan_dxt(tex_caps_from_gles(2, 0, "")) == DxtStrategy::DecodeRGBA8);
    // ASTC only (hypothetical): transcode to ASTC
    CHECK(plan_dxt(kTexASTC_LDR) == DxtStrategy::TranscodeASTC);
}

TEST(gpu, vulkan_caps_need_all_formats_of_the_family) {
    VkFormatFacts f;
    f.bc1 = f.bc2 = true; // bc3 missing
    CHECK(!(tex_caps_from_vulkan(f) & kTexBC1_3));
    f.bc3 = true;
    CHECK(tex_caps_from_vulkan(f) & kTexBC1_3);
    f.etc2_rgb = f.etc2_rgba8 = true;
    CHECK(!(tex_caps_from_vulkan(f) & kTexETC2));
    f.etc2_rgb_a1 = true;
    CHECK(tex_caps_from_vulkan(f) & kTexETC2);
    f.d24s8 = true;
    f.sampler_anisotropy = true;
    uint32_t c = tex_caps_from_vulkan(f);
    CHECK((c & kTexD24S8) && (c & kTexAniso) && !(c & kTexD32S8));
}

TEST(gpu, texture_memory_estimates) {
    CHECK_NEAR(bytes_per_texel(DxtStrategy::NativeBC, true), 0.5, 1e-6);
    CHECK_NEAR(bytes_per_texel(DxtStrategy::NativeBC, false), 1.0, 1e-6);
    CHECK_NEAR(bytes_per_texel(DxtStrategy::DecodeRGBA8, true), 4.0, 1e-6);
}

TEST(gpu, api_choice) {
    ApiFacts f;
    f.vulkan_present = true;
    f.vulkan_api_version = vk_version(1, 3, 128);
    f.vendor = GpuVendor::Adreno;
    f.adreno_gen = 7;
    f.sdk_int = 34;
    CHECK(pick_graphics_api(f).api == ApiChoice::Vulkan);
    f.adreno_gen = 5;
    CHECK(pick_graphics_api(f).api == ApiChoice::OpenGLES);
    f.adreno_gen = 6;
    f.vulkan_api_version = vk_version(1, 0, 3);
    CHECK(pick_graphics_api(f).api == ApiChoice::OpenGLES);
    f.vulkan_api_version = vk_version(1, 1, 0);
    f.vendor = GpuVendor::Software;
    CHECK(pick_graphics_api(f).api == ApiChoice::OpenGLES);
    f.vendor = GpuVendor::Mali;
    CHECK(pick_graphics_api(f).api == ApiChoice::Vulkan);
    f.vulkan_present = false;
    CHECK(pick_graphics_api(f).api == ApiChoice::OpenGLES);
    CHECK_EQ(vk_major(vk_version(1, 3, 5)), 1u);
    CHECK_EQ(vk_minor(vk_version(1, 3, 5)), 3u);
}

// ------------------------------------------------------------------------------------------ input

TEST(input, classify_sources) {
    CHECK(classify_sources(0x00001002, false) == InputKind::Touch);        // touchscreen
    CHECK(classify_sources(0x00000401 | 0x01000010, false) == InputKind::Gamepad);
    CHECK(classify_sources(0x00000401, false) == InputKind::Gamepad);
    CHECK(classify_sources(0x01000010, false) == InputKind::Gamepad);      // joystick only
    CHECK(classify_sources(0x00000101 | 0x00000201, false, 2) == InputKind::Keyboard);
    CHECK(classify_sources(0x00000101, false, 1) == InputKind::Unknown);   // phone volume keys
    CHECK(classify_sources(0x00000101, true, 2) == InputKind::Unknown);    // virtual keyboard
    CHECK(classify_sources(0x00002002, false) == InputKind::Mouse);
    CHECK(classify_sources(0x00020004, false) == InputKind::Mouse);        // pointer capture
    CHECK(classify_sources(0x00004002 | 0x00001002, false) == InputKind::Stylus);
    CHECK(classify_sources(0x00100008, false) == InputKind::Touchpad);
    CHECK(classify_sources(0x00000401, true) == InputKind::Unknown);       // virtual gamepad ghost
    CHECK(classify_sources(0, false) == InputKind::Unknown);
}

TEST(input, button_and_axis_tables) {
    CHECK(pad_button_from_keycode(96) == PadButton::A);
    CHECK(pad_button_from_keycode(97) == PadButton::B);
    CHECK(pad_button_from_keycode(99) == PadButton::X);
    CHECK(pad_button_from_keycode(100) == PadButton::Y);
    CHECK(pad_button_from_keycode(108) == PadButton::Start);
    CHECK(pad_button_from_keycode(109) == PadButton::Back);
    CHECK(pad_button_from_keycode(19) == PadButton::DpadUp);
    CHECK(pad_button_from_keycode(4) == PadButton::None);    // BACK is the system key
    CHECK(pad_button_from_keycode(98) == PadButton::None);   // BUTTON_C unused
    CHECK(pad_axis_from_android_axis(0, true) == PadAxis::LeftX);
    CHECK(pad_axis_from_android_axis(11, true) == PadAxis::RightX);
    CHECK(pad_axis_from_android_axis(14, true) == PadAxis::RightY);
    CHECK(pad_axis_from_android_axis(11, false) == PadAxis::None);
    CHECK(pad_axis_from_android_axis(12, false) == PadAxis::RightX);
    CHECK(pad_axis_from_android_axis(23, true) == PadAxis::LeftTrigger);  // AXIS_BRAKE
    CHECK(pad_axis_from_android_axis(22, true) == PadAxis::RightTrigger); // AXIS_GAS
    CHECK(pad_axis_from_android_axis(99, true) == PadAxis::None);
    CHECK(uses_xbox_right_stick(true, true, false, false));
    CHECK(!uses_xbox_right_stick(true, true, true, true));
    CHECK(!uses_xbox_right_stick(false, false, true, true));
}

TEST(input, radial_deadzone) {
    StickValue z = apply_radial_deadzone(0.1f, 0.1f, 0.2f);
    CHECK(z.x == 0 && z.y == 0);
    StickValue full = apply_radial_deadzone(1.0f, 0.0f, 0.2f);
    CHECK_NEAR(full.x, 1.0, 1e-5);
    StickValue half = apply_radial_deadzone(0.6f, 0.0f, 0.2f);
    CHECK_NEAR(half.x, 0.5, 1e-5);
    StickValue diag = apply_radial_deadzone(1.0f, 1.0f, 0.2f); // corner beyond unit circle clamps to 1
    CHECK_NEAR(std::sqrt(diag.x * diag.x + diag.y * diag.y), 1.0, 1e-5);
}

TEST(input, last_input_tracker) {
    LastInputTracker t;
    CHECK(t.note({InputKind::Touch, 1.0f}, 1000));
    CHECK(t.last() == InputKind::Touch);
    CHECK(!t.note({InputKind::Touch, 1.0f}, 1010));        // same kind
    CHECK(!t.note({InputKind::Gamepad, 0.1f}, 2000));      // drifting stick
    CHECK(!t.note({InputKind::Gamepad, 1.0f}, 1050));      // within the hold time
    CHECK(t.note({InputKind::Gamepad, 1.0f}, 1200));
    CHECK(t.last() == InputKind::Gamepad);
    CHECK(!t.note({InputKind::Unknown, 1.0f}, 5000));
    t.device_removed(InputKind::Gamepad);
    CHECK(t.last() == InputKind::Touch);
    t.device_removed(InputKind::Keyboard); // unrelated removal
    CHECK(t.last() == InputKind::Touch);
}

// ------------------------------------------------------------------------------------------ audio

TEST(audio, route_mapping) {
    CHECK(route_from_device_type(2) == AudioRoute::Speaker);
    CHECK(route_from_device_type(3) == AudioRoute::Wired);
    CHECK(route_from_device_type(8) == AudioRoute::Bluetooth);
    CHECK(route_from_device_type(26) == AudioRoute::Bluetooth);
    CHECK(route_from_device_type(22) == AudioRoute::Usb);
    CHECK(route_from_device_type(9) == AudioRoute::Hdmi);
    CHECK(route_from_device_type(0) == AudioRoute::Unknown);
    CHECK(route_has_high_latency(AudioRoute::Bluetooth, 8));
    CHECK(!route_has_high_latency(AudioRoute::Wired, 3));
}

TEST(audio, plan_uses_native_rate_and_whole_bursts) {
    AudioProps p;
    p.native_sample_rate = 48000;
    p.frames_per_buffer = 192;
    p.low_latency = true;
    AudioPlan a = plan_audio(p, 44100, 40);
    CHECK_EQ(a.sample_rate, 48000);
    CHECK(a.needs_resample && a.low_latency_mode);
    CHECK_EQ(a.buffer_frames % 192, 0);
    CHECK(a.buffer_frames >= 1920 && a.buffer_frames <= 1920 + 192); // 40 ms = 1920 frames
    AudioPlan same = plan_audio(p, 48000, 1);
    CHECK(!same.needs_resample);
    CHECK_EQ(same.buffer_frames, 384);                                 // minimum two bursts
    AudioPlan huge = plan_audio(p, 48000, 100000);
    CHECK(huge.buffer_frames <= 48000);                               // one second at most
    AudioProps weird;
    weird.native_sample_rate = 0;
    weird.frames_per_buffer = 0;
    AudioPlan w = plan_audio(weird, 22050, 20);
    CHECK(w.sample_rate == 48000 && w.buffer_frames > 0);
}

TEST(audio, focus_state_machine) {
    AudioFocusModel m;
    CHECK(m.on_change(-3) == FocusAction::Duck);
    CHECK(m.on_change(-3) == FocusAction::None);
    CHECK(m.on_change(1) == FocusAction::Unduck);
    CHECK(m.on_change(-2) == FocusAction::Pause);
    CHECK(m.on_change(-3) == FocusAction::None); // already paused
    CHECK(m.on_change(1) == FocusAction::Resume);
    CHECK(m.on_change(-1) == FocusAction::Pause);
    CHECK(m.permanently_lost() && m.paused());
    CHECK(m.on_change(1) == FocusAction::Resume);
    CHECK(!m.permanently_lost());
    CHECK(m.on_change(1) == FocusAction::None);
    CHECK(m.on_change(42) == FocusAction::None);
}

TEST(audio, tone_is_phase_continuous) {
    ToneGenerator t(440.0f, 48000, 0.5f);
    float a[100], b[100], all[200];
    t.fill(a, 100);
    t.fill(b, 100);
    ToneGenerator u(440.0f, 48000, 0.5f);
    u.fill(all, 200);
    for (int i = 0; i < 100; ++i) {
        CHECK_NEAR(a[i], all[i], 1e-6);
        CHECK_NEAR(b[i], all[100 + i], 1e-6);
    }
    float peak = 0;
    for (float v : all) peak = std::max(peak, std::fabs(v));
    CHECK(peak > 0.45f && peak <= 0.5f);
}

// ------------------------------------------------------------------------------------------ crash

TEST(crash, report_text_and_names) {
    CHECK(std::string(signal_name(11)) == "SIGSEGV");
    CHECK(std::string(signal_name(6)) == "SIGABRT");
    CHECK(std::string(signal_code_name(11, 1)) == "SEGV_MAPERR");
    CHECK(std::string(signal_code_name(11, 2)) == "SEGV_ACCERR");
    CrashInfo c;
    c.signal = 11;
    c.si_code = 1;
    c.fault_address = 0xdeadbeef;
    c.pid = 1234;
    c.tid = 1250;
    c.thread_name = "SDLThread";
    c.app_version = "0.1.0";
    c.abi = "arm64-v8a";
    c.device = "Google Pixel 8";
    c.sdk_int = 35;
    c.unix_time = 1790000000;
    c.rss_bytes = 123456 * 1024;
    c.frames.push_back({0x7f00001000, 0x1000, "/data/app/lib/arm64/libmain.so", "nue_frame"});
    c.frames.push_back({0x7f00002000, 0x2000, "", ""});
    std::string r = format_crash_report(c);
    CHECK(r.find("SIGSEGV") != std::string::npos);
    CHECK(r.find("SEGV_MAPERR") != std::string::npos);
    CHECK(r.find("0xdeadbeef") != std::string::npos);
    CHECK(r.find("SDLThread") != std::string::npos);
    CHECK(r.find("libmain.so (nue_frame)") != std::string::npos);
    CHECK(r.find("#01 pc 0000000000002000  <unknown>") != std::string::npos);
    CHECK(r.find("123456 KiB") != std::string::npos);
}

TEST(crash, file_name_is_utc_timestamp) {
    // 2026-10-22 12:34:56 UTC
    CHECK_EQ(crash_file_name(1792672496, 77), std::string("crash-20261022-123456-77.txt"));
    CHECK_EQ(crash_file_name(0, 1), std::string("crash-19700101-000000-1.txt"));
    CHECK_EQ(crash_file_name(951782400, 5), std::string("crash-20000229-000000-5.txt")); // leap day
}

// ------------------------------------------------------------------------------------------ events

TEST(events, fifo_and_coalescing) {
    EventQueue q(8);
    q.push(make_event(NUE_ANDROID_EVENT_LIFECYCLE, NUE_ANDROID_LIFE_PAUSE));
    q.push(make_event(NUE_ANDROID_EVENT_DISPLAY, 1920, 1080));
    q.push(make_event(NUE_ANDROID_EVENT_DISPLAY, 2400, 1080));    // replaces the previous display event
    q.push(make_event(NUE_ANDROID_EVENT_LIFECYCLE, NUE_ANDROID_LIFE_RESUME));
    CHECK_EQ(q.size(), size_t(3));
    NueAndroidEvent e;
    CHECK(q.poll(&e) && e.type == NUE_ANDROID_EVENT_LIFECYCLE && e.i[0] == NUE_ANDROID_LIFE_PAUSE);
    CHECK(q.poll(&e) && e.type == NUE_ANDROID_EVENT_DISPLAY && e.i[0] == 2400);
    CHECK(q.poll(&e) && e.i[0] == NUE_ANDROID_LIFE_RESUME);
    CHECK(!q.poll(&e));
}

TEST(events, import_progress_coalesces_but_state_changes_do_not) {
    EventQueue q;
    q.push(make_event(NUE_ANDROID_EVENT_IMPORT, NUE_ANDROID_IMPORT_COPYING, 1, 10));
    q.push(make_event(NUE_ANDROID_EVENT_IMPORT, NUE_ANDROID_IMPORT_COPYING, 2, 10));
    q.push(make_event(NUE_ANDROID_EVENT_IMPORT, NUE_ANDROID_IMPORT_DONE, 10, 10));
    CHECK_EQ(q.size(), size_t(2));
    NueAndroidEvent e;
    q.poll(&e);
    CHECK_EQ(e.i[1], 2);
    q.poll(&e);
    CHECK_EQ(e.i[0], int32_t(NUE_ANDROID_IMPORT_DONE));
}

TEST(events, overflow_drops_state_events_first_and_counts) {
    EventQueue q(4);
    q.push(make_event(NUE_ANDROID_EVENT_THERMAL, 1));
    for (int i = 0; i < 3; ++i) q.push(make_event(NUE_ANDROID_EVENT_FOCUS, i));
    CHECK_EQ(q.size(), size_t(4));
    q.push(make_event(NUE_ANDROID_EVENT_LIFECYCLE, NUE_ANDROID_LIFE_PAUSE)); // full: the thermal event is sacrificed
    CHECK_EQ(q.size(), size_t(4));
    CHECK_EQ(q.dropped(), 1u);
    NueAndroidEvent e;
    q.poll(&e);
    CHECK(e.type == NUE_ANDROID_EVENT_FOCUS);
    q.push(make_event(NUE_ANDROID_EVENT_FOCUS, 9));
    q.push(make_event(NUE_ANDROID_EVENT_FOCUS, 9)); // nothing coalescable left: dropped
    CHECK_EQ(q.dropped(), 2u);
}

TEST(events, concurrent_producers) {
    EventQueue q(100000);
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t)
        th.emplace_back([&q, t] {
            for (int i = 0; i < 1000; ++i) q.push(make_event(NUE_ANDROID_EVENT_FOCUS, t, i));
        });
    for (auto& x : th) x.join();
    CHECK_EQ(q.size(), size_t(4000));
    int last[4] = {-1, -1, -1, -1};
    NueAndroidEvent e;
    bool ordered = true;
    while (q.poll(&e)) {
        if (e.i[1] <= last[e.i[0]]) ordered = false; // per-producer order preserved
        last[e.i[0]] = e.i[1];
    }
    CHECK(ordered);
}

// ------------------------------------------------------------------------------------------------------- cpu

#include "cpu_topology.hpp"

TEST(cpu, snapdragon_8_gen_layout) {
    // 1 prime 3.3 GHz, 4 gold 3.0 / 2.8, 3 silver 2.0
    std::vector<uint32_t> khz = {2000000, 2000000, 2000000, 2800000, 2800000, 3000000, 3000000, 3300000};
    CoreMasks m = classify_cores(khz);
    CHECK(m.hybrid);
    CHECK_EQ(m.total, 8u);
    CHECK_EQ(m.efficiency_count, 3u);
    CHECK_EQ(m.performance_count, 4u + 1u); // 2.8 / 3.3 = 0.85 counts as performance
    CHECK_EQ(m.efficiency, uint64_t(0x07));
    CHECK_EQ(m.performance, uint64_t(0xF8));
    CHECK_EQ(m.all, uint64_t(0xFF));
    CHECK_EQ(m.max_khz, 3300000u);
}

TEST(cpu, symmetric_and_unknown) {
    CoreMasks sym = classify_cores({2400000, 2400000, 2400000, 2400000});
    CHECK(!sym.hybrid);
    CHECK_EQ(sym.performance_count, 4u);
    CoreMasks none = classify_cores({0, 0});
    CHECK(!none.hybrid && none.performance == 0 && none.all == 3u);
    CoreMasks empty = classify_cores({});
    CHECK_EQ(empty.total, 0u);
}

TEST(cpu, priority_mapping) {
    CHECK_EQ(nice_from_priority(0), 0);
    CHECK_EQ(nice_from_priority(1), 10);
    CHECK_EQ(nice_from_priority(2), 0);
    CHECK_EQ(nice_from_priority(5), -16);
    for (int p = 1; p <= 5; ++p) CHECK_EQ(priority_from_nice(nice_from_priority(p)), p == 0 ? 2 : p);
}
