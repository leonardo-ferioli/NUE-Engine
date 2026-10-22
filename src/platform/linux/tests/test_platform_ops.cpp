// Tests of the nue_platform_ops table against in-memory machines. The platform layer reads /proc,
// /sys and the environment through the injected IFileSystem / IEnv, so these run on any host and
// touch nothing real. (Entries that need Linux system calls are only present in the Linux build:
// see test_runtime_linux.cpp.)
#include <cstring>

#include "fixtures.hpp"
#include "lx_fs.hpp"
#include "lx_sysinfo.hpp"
#include "lx_test.hpp"
#include "nue_ext_linux.h"
#include "ops_stubs.hpp"
#include "platform_linux_test_hooks.h"

using namespace nue::lx;

namespace {

struct Machine {
    MemFs fs;
    MapEnv env;
    const nue_platform_ops* ops = nullptr;
    nue_backend_init bi;

    explicit Machine(void (*build)(MemFs&)) {
        build(fs);
        env.vars["HOME"] = "/home/deck";
        env.vars["XDG_RUNTIME_DIR"] = "/run/user/1000";
        env.vars["LANG"] = "es_ES.UTF-8";
        {
            std::lock_guard<std::mutex> l(stubs::mtx);
            stubs::events.clear();
            stubs::extensions.clear();
        }
        nue_linux_test_set_environment(&fs, &env);
        ops = nue_platform_linux_get_ops();
        std::memset(&bi, 0, sizeof bi);
        bi.struct_size = sizeof bi;
        bi.deterministic = NUE_TRUE;  // keeps the Linux build from touching the desktop's services
    }
    bool init() { return ops->init(&bi) >= 0; }
    ~Machine() {
        ops->shutdown();
        nue_linux_test_set_environment(nullptr, nullptr);
    }
};

bool has_cap(const nue_platform_ops* ops, nue_capability c) {
    return (ops->capabilities[static_cast<unsigned>(c) / 64] >> (static_cast<unsigned>(c) % 64)) & 1ull;
}

std::string path_of(const nue_platform_ops* ops, nue_root_kind k, const nue_init_desc* d) {
    char buf[512];
    size_t need = 0;
    nue_result r = ops->storage_root(k, d, buf, sizeof buf, &need);
    return r >= 0 ? std::string(buf) : std::string("<") + std::to_string(r) + ">";
}

}  // namespace

TEST(ops_table_shape) {
    const nue_platform_ops* ops = nue_platform_linux_get_ops();
    CHECK(ops != nullptr);
    CHECK(std::string(ops->name) == "linux");
    CHECK_EQ(ops->struct_size, uint32_t(sizeof(nue_platform_ops)));
    CHECK(ops->init && ops->shutdown && ops->pump);
    CHECK(ops->system_info && ops->memory_info && ops->system_locale && ops->storage_root && ops->power_info);
    CHECK(ops->import == nullptr);  // desktop: a plain verified copy in the core is enough
    CHECK(ops->perf_hint_create == nullptr);
    CHECK(ops->haptics_available == nullptr);
    CHECK(ops->crash_release_input == nullptr);
}

TEST(ops_system_info_steam_deck) {
    Machine m(fixtures::steam_deck);
    CHECK(m.init());
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    CHECK(m.ops->system_info(&si) >= 0);
    CHECK(si.os == NUE_OS_LINUX);
    CHECK_EQ(std::string(si.os_name), std::string("SteamOS Holo"));
    CHECK_EQ(std::string(si.os_version), std::string("6.5.0-valve22-1-neptune-65"));
    CHECK(si.device_class == NUE_DEVICE_HANDHELD);
    CHECK_EQ(std::string(si.device_model), std::string("Jupiter"));
    CHECK(std::string(si.cpu_name).find("Custom APU 0405") != std::string::npos);
    CHECK_EQ(si.cpu_logical_cores, 8u);
    CHECK_EQ(si.cpu_physical_cores, 4u);
    CHECK_EQ(si.cpu_performance_cores, 0u);  // uniform CPU: reported as "not hybrid"
    CHECK_EQ(si.cpu_max_mhz, 3500u);
    CHECK((si.cpu_features & NUE_CPU_AVX2) != 0);
    CHECK((si.cpu_features & NUE_CPU_AVX512) == 0);
    CHECK_EQ(si.memory_total_bytes, uint64_t(16384000) * 1024);
    CHECK_EQ(si.gpu_count, 1u);
    CHECK_EQ(si.gpus[0].vendor_id, 0x1002u);
    CHECK(si.gpus[0].is_default);
    CHECK_EQ(std::string(si.locale), std::string("es-ES"));
    CHECK(has_cap(m.ops, NUE_CAP_BATTERY));
    CHECK(has_cap(m.ops, NUE_CAP_THERMAL_STATE));
    CHECK(m.ops->timer_resolution_get != nullptr);
    CHECK(!has_cap(m.ops, NUE_CAP_CORE_CLASS));
    CHECK(!has_cap(m.ops, NUE_CAP_MINIDUMP));  // text stack reports only: no minidump on Linux
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        CHECK(stubs::extensions.count("nue.linux.v1") == 1);
    }
}

TEST(ops_system_info_hybrid_and_laptop) {
    Machine m(fixtures::intel_hybrid);
    m.fs.add_file("/sys/class/power_supply/BAT0/type", "Battery\n");
    m.fs.add_file("/sys/class/power_supply/BAT0/status", "Charging\n");
    m.fs.add_file("/sys/class/power_supply/BAT0/capacity", "90\n");
    m.fs.add_file("/sys/class/dmi/id/chassis_type", "10\n");
    m.fs.add_file("/sys/class/dmi/id/product_name", "Laptop 15\n");
    CHECK(m.init());
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    CHECK(m.ops->system_info(&si) >= 0);
    CHECK(si.device_class == NUE_DEVICE_LAPTOP);
    CHECK_EQ(si.cpu_performance_cores, 4u);
    CHECK_EQ(si.cpu_efficiency_cores, 4u);
    nue_power_info pi;
    NUE_STRUCT_INIT(&pi);
    CHECK(m.ops->power_info(&pi) >= 0);
    CHECK(pi.state == NUE_POWER_CHARGING);
    CHECK_EQ(pi.battery_percent, 90);
}

TEST(ops_arm_board_model_from_device_tree) {
    Machine m(fixtures::arm_big_little);
    m.fs.add_file("/proc/device-tree/model", std::string("Rockchip RK3588 Board\0", 22));
    m.fs.add_file("/proc/cpuinfo", fixtures::kCpuInfoArm);
    CHECK(m.init());
    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    CHECK(m.ops->system_info(&si) >= 0);
    CHECK_EQ(std::string(si.device_model), std::string("Rockchip RK3588 Board"));
    CHECK((si.cpu_features & NUE_CPU_NEON) != 0);
    CHECK_EQ(si.cpu_performance_cores, 2u);
    CHECK_EQ(si.cpu_efficiency_cores, 4u);
}

TEST(ops_storage_roots_follow_xdg) {
    Machine m(fixtures::steam_deck);
    CHECK(m.init());
    nue_init_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.org_name = "NUE";
    d.app_id = "nfsu2";
    CHECK_EQ(path_of(m.ops, NUE_ROOT_USER, &d), std::string("/home/deck/.local/share/NUE/nfsu2"));
    CHECK_EQ(path_of(m.ops, NUE_ROOT_CACHE, &d), std::string("/home/deck/.cache/NUE/nfsu2"));
    CHECK_EQ(path_of(m.ops, NUE_ROOT_SETTINGS, &d), std::string("/home/deck/.config/NUE/nfsu2"));
    CHECK_EQ(path_of(m.ops, NUE_ROOT_TEMP, &d), std::string("/run/user/1000/NUE/nfsu2"));
    d.game_data_path = "/games/nfsu2";
    CHECK_EQ(path_of(m.ops, NUE_ROOT_DATA, &d), std::string("/games/nfsu2"));
    d.user_data_path = "/mnt/saves";
    CHECK_EQ(path_of(m.ops, NUE_ROOT_USER, &d), std::string("/mnt/saves"));
    // names that could escape the XDG directory are neutralised
    d.user_data_path = nullptr;
    d.org_name = "../evil";
    d.app_id = "a/b";
    std::string p = path_of(m.ops, NUE_ROOT_USER, &d);
    CHECK(p.find("..") == std::string::npos);
    CHECK(p.compare(0, 24, "/home/deck/.local/share/") == 0);
    // size query and truncation follow NUE_STRING_OUT
    d.org_name = "NUE";
    d.app_id = "nfsu2";
    size_t need = 0;
    char small[8];
    CHECK(m.ops->storage_root(NUE_ROOT_USER, &d, small, sizeof small, &need) >= 0);
    CHECK_EQ(need, std::string("/home/deck/.local/share/NUE/nfsu2").size() + 1);
    CHECK_EQ(std::string(small), std::string("/home/d"));
    CHECK(m.ops->storage_root(static_cast<nue_root_kind>(77), &d, small, sizeof small, &need) < 0);
}

TEST(ops_storage_root_without_home_fails_cleanly) {
    MemFs fs;
    MapEnv env;  // no HOME, no XDG
    nue_linux_test_set_environment(&fs, &env);
    const nue_platform_ops* ops = nue_platform_linux_get_ops();
    nue_backend_init bi;
    std::memset(&bi, 0, sizeof bi);
    bi.deterministic = NUE_TRUE;
    CHECK(ops->init(&bi) >= 0);
    char buf[64];
    size_t need;
    CHECK_EQ(ops->storage_root(NUE_ROOT_USER, nullptr, buf, sizeof buf, &need), NUE_ERR_NOT_FOUND);
    ops->shutdown();
    nue_linux_test_set_environment(nullptr, nullptr);
}

TEST(ops_locale_forms) {
    Machine m(fixtures::steam_deck);
    CHECK(m.init());
    char buf[32];
    CHECK(m.ops->system_locale(buf, sizeof buf) >= 0);
    CHECK_EQ(std::string(buf), std::string("es-ES"));
    m.env.vars["LC_ALL"] = "C";
    CHECK(m.ops->system_locale(buf, sizeof buf) >= 0);
    CHECK_EQ(std::string(buf), std::string("en-US"));
    m.env.vars["LC_ALL"] = "";
    m.env.vars["LANGUAGE"] = "de_DE:en_US";
    m.env.vars["LC_MESSAGES"] = "";
    CHECK(m.ops->system_locale(buf, sizeof buf) >= 0);
    CHECK_EQ(std::string(buf), std::string("de-DE"));
}

TEST(ops_memory_respects_cgroup_limit) {
    Machine m(fixtures::steam_deck);
    m.fs.add_file("/sys/fs/cgroup/memory.max", "4294967296\n");
    CHECK(m.init());
    uint64_t total = 0, avail = 0;
    CHECK(m.ops->memory_info(&total, &avail) >= 0);
    CHECK_EQ(total, uint64_t(4294967296ull));
    CHECK(avail <= total);
}

TEST(ops_power_info_and_events) {
    Machine m(fixtures::steam_deck);
    CHECK(m.init());
    nue_power_info pi;
    NUE_STRUCT_INIT(&pi);
    CHECK(m.ops->power_info(&pi) >= 0);
    CHECK(pi.state == NUE_POWER_ON_BATTERY);
    CHECK_EQ(pi.battery_percent, 64);
    CHECK_EQ(pi.seconds_left, 7200);
    CHECK(!pi.on_ac_power);
    CHECK(pi.thermal == NUE_THERMAL_NOMINAL);  // 70.5 C

    nue_linux_test_poll_now();
    m.ops->pump();  // first pump only records the baseline: no event for the initial state
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        CHECK(stubs::events.empty());
    }
    m.fs.add_file("/sys/class/hwmon/hwmon3/temp1_input", "91000\n");
    m.fs.add_file("/sys/class/power_supply/BAT1/capacity", "63\n");
    nue_linux_test_poll_now();
    m.ops->pump();
    bool thermal = false, power = false;
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        for (const auto& e : stubs::events) {
            if (e.type == NUE_EVENT_THERMAL_CHANGED) { thermal = true; CHECK_EQ(e.payload.power.thermal, uint32_t(NUE_THERMAL_SERIOUS)); }
            if (e.type == NUE_EVENT_POWER_CHANGED) { power = true; CHECK_EQ(e.payload.power.percent, 63); }
        }
        stubs::events.clear();
    }
    CHECK(thermal && power);
    m.ops->pump();  // nothing changed since: no duplicates
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        CHECK(stubs::events.empty());
    }
    m.fs.add_file("/sys/class/power_supply/ACAD/type", "Mains\n");
    m.fs.add_file("/sys/class/power_supply/ACAD/online", "1\n");
    nue_linux_test_poll_now();
    nue_power_info after;
    NUE_STRUCT_INIT(&after);
    m.ops->power_info(&after);
    CHECK(after.on_ac_power);
    CHECK(after.state != NUE_POWER_ON_BATTERY);
}

TEST(ops_extension_resolve_session_renderer_describe) {
    Machine m(fixtures::hybrid_gpu_laptop);
    m.fs.add_file("/games/nfsu2/GLOBAL/GlobalA.BUN", "x");
    m.fs.add_file("/etc/os-release", "NAME=\"Ubuntu\"\nPRETTY_NAME=\"Ubuntu 24.04 LTS\"\nID=ubuntu\n");
    m.fs.advance_ns(10'000'000'000);
    CHECK(m.init());
    const nue_ext_linux_v1* x;
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        x = static_cast<const nue_ext_linux_v1*>(stubs::extensions["nue.linux.v1"]);
    }
    CHECK(x != nullptr && x->struct_size == sizeof(nue_ext_linux_v1));

    char buf[512];
    size_t need = 0;
    uint32_t flags = 0;
    CHECK_EQ(x->resolve_path("/games/nfsu2", "C:\\global\\globala.bun", buf, sizeof buf, &need, &flags), NUE_OK);
    CHECK_EQ(std::string(buf), std::string("/games/nfsu2/GLOBAL/GlobalA.BUN"));
    CHECK((flags & NUE_LINUX_PATH_FOUND) && (flags & NUE_LINUX_PATH_CASE_CHANGED));
    CHECK_EQ(x->resolve_path("/games/nfsu2", "global\\new.sav", buf, sizeof buf, &need, &flags), NUE_ERR_NOT_FOUND);
    CHECK((flags & NUE_LINUX_PATH_PARENT_EXISTS) && !(flags & NUE_LINUX_PATH_FOUND));
    CHECK_EQ(std::string(buf), std::string("/games/nfsu2/GLOBAL/new.sav"));

    nue_linux_session_info si;
    NUE_STRUCT_INIT(&si);
    CHECK_EQ(x->get_session(&si), NUE_OK);
    CHECK(!si.steam_deck && !si.flatpak);
    CHECK_EQ(std::string(si.distro_id), std::string("ubuntu"));

    nue_linux_renderer_plan rp;
    NUE_STRUCT_INIT(&rp);
    CHECK_EQ(x->plan_renderer(NUE_FALSE, &rp), NUE_OK);
    CHECK_EQ(std::string(rp.api), std::string("vulkan"));
    CHECK(rp.hybrid_gpu);
    CHECK_EQ(rp.vendor_id, 0x10deu);  // discrete NVIDIA preferred
    CHECK(std::string(rp.environment).find("MESA_VK_DEVICE_SELECT=10de:2560") != std::string::npos);

    std::string text(8192, 0);
    CHECK_EQ(x->describe(NUE_FALSE, &text[0], text.size(), &need), NUE_OK);
    CHECK(std::string(text.c_str()).find("Ubuntu 24.04 LTS") != std::string::npos);
}

TEST(ops_extension_steamos) {
    Machine m(fixtures::steamos_deck);
    m.env.vars["SteamDeck"] = "1";
    m.env.vars["SteamGamepadUI"] = "1";
    m.env.vars["SteamGameId"] = "1234567";
    m.env.vars["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = "0x28de/0x1205";
    m.env.vars["MESA_SHADER_CACHE_DIR"] = "/home/deck/.local/share/Steam/steamapps/shadercache/1234567";
    m.env.vars["PATH"] = "/usr/bin";
    m.fs.advance_ns(10'000'000'000);
    nue_init_desc d;
    std::memset(&d, 0, sizeof d);
    d.struct_size = sizeof d;
    d.org_name = "NUE";
    d.app_id = "nfsu2";
    m.bi.desc = &d;
    CHECK(m.init());
    const nue_ext_linux_v1* x;
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        x = static_cast<const nue_ext_linux_v1*>(stubs::extensions["nue.linux.v1"]);
    }
    CHECK(x && x->get_steamos_info && x->plan_frame_pacing && x->suggest_fps_cap && x->controller_glyph && x->shader_cache_dir && x->find_steam_app);

    nue_linux_steamos_info si;
    NUE_STRUCT_INIT(&si);
    CHECK_EQ(x->get_steamos_info(&si), NUE_OK);
    CHECK(si.hardware == NUE_LINUX_STEAM_HW_DECK_LCD && !si.hardware_is_heuristic);
    CHECK(si.steamos_like && std::string(si.flavor) == "SteamOS");
    CHECK(si.immutable_root && si.session_mode == NUE_LINUX_SESSION_GAMING);
    CHECK(si.launched_by_steam && si.steam_game_id == 1234567 && !si.non_steam_shortcut);
    CHECK(si.panel_width == 1280 && si.panel_height == 800 && si.panel_max_hz == 60 && !si.panel_oled);
    CHECK(si.steam_input_active && si.physical_pads_hidden && si.visible_gamepads == 1);
    CHECK(si.default_glyphs == NUE_LINUX_GLYPHS_STEAM);
    CHECK_EQ(si.sd_card_count, 1u);
    CHECK_EQ(std::string(si.first_sd_card), std::string("/run/media/deck/GAMES"));

    nue_linux_frame_pacing fp;
    NUE_STRUCT_INIT(&fp);
    CHECK_EQ(x->plan_frame_pacing(60, 60.0, &fp), NUE_OK);
    CHECK(fp.present_vsync && !fp.own_limiter && !fp.judder_expected);
    CHECK_EQ(x->plan_frame_pacing(0, 60.0, &fp), NUE_ERR_INVALID_ARGUMENT);

    int32_t fps = 0;
    CHECK_EQ(x->suggest_fps_cap(60, &fps), NUE_OK);
    CHECK_EQ(fps, 60);  // fixture battery is at 64 %
    m.fs.add_file("/sys/class/power_supply/BAT1/capacity", "9\n");
    CHECK_EQ(x->suggest_fps_cap(60, &fps), NUE_OK);
    CHECK_EQ(fps, 40);

    int32_t glyph = -1;
    CHECK_EQ(x->controller_glyph(0x054c, 0x0ce6, &glyph), NUE_OK);
    CHECK_EQ(glyph, int32_t(NUE_LINUX_GLYPHS_PLAYSTATION));

    char buf[256];
    size_t need = 0;
    CHECK_EQ(x->shader_cache_dir(buf, sizeof buf, &need), NUE_OK);
    CHECK_EQ(std::string(buf), std::string("/home/deck/.local/share/Steam/steamapps/shadercache/1234567/nue"));
    CHECK_EQ(x->find_steam_app(1234567, buf, sizeof buf, &need), NUE_OK);
    CHECK_EQ(std::string(buf), std::string("/run/media/deck/GAMES/steamapps/common/NUE Test Game"));
    CHECK_EQ(x->find_steam_app(5, buf, sizeof buf, &need), NUE_ERR_NOT_FOUND);

    std::string tuning(512, 0);
    CHECK_EQ(x->describe_power_tuning(&tuning[0], tuning.size(), &need), NUE_OK);
    CHECK(std::string(tuning.c_str()).find("amdgpu.power_cap_w=15") != std::string::npos);
}

TEST(ops_api_011_hooks_and_platform_flags) {
    Machine m(fixtures::steamos_deck);
    m.env.vars["SteamDeck"] = "1";
    m.env.vars["SteamGamepadUI"] = "1";
    m.env.vars["SteamGameId"] = "1234567";
    m.env.vars["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = "0x28de/0x1205";
    m.fs.add_file("/proc/pressure/memory", "some avg10=20.00 avg60=1.00 avg300=0.00 total=1\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n");
    m.fs.add_file("/proc/self/status", "VmRSS:\t  2048 kB\n");
    m.fs.add_file("/sys/class/drm/card0/device/mem_info_gtt_total", "8589934592\n");
    m.fs.add_file("/sys/fs/cgroup/memory.max", "max\n");
    CHECK(m.ops->pre_init != nullptr && m.ops->memory_info_ex != nullptr && m.ops->gpu_probe != nullptr && m.ops->screen_keyboard != nullptr);
    CHECK(m.ops->window_request_refresh == nullptr);  // gamescope offers no API to request a refresh rate: the core limits in software
    CHECK(m.ops->owns_lifecycle == NUE_FALSE);
    CHECK(m.ops->pre_init(&m.bi) >= 0);  // before init: must not need the layer state
    char root[256];
    size_t need;
    CHECK(m.ops->storage_root(NUE_ROOT_USER, nullptr, root, sizeof root, &need) >= 0);  // works before init
    CHECK(m.init());

    nue_system_info si;
    NUE_STRUCT_INIT(&si);
    CHECK(m.ops->system_info(&si) >= 0);
    CHECK((si.platform_flags & NUE_PLATFORM_STEAM_DECK) && (si.platform_flags & NUE_PLATFORM_HANDHELD) && (si.platform_flags & NUE_PLATFORM_STEAMOS));
    CHECK((si.platform_flags & NUE_PLATFORM_GAMESCOPE) && (si.platform_flags & NUE_PLATFORM_STEAM_GAMING_MODE));
    CHECK((si.platform_flags & NUE_PLATFORM_READ_ONLY_ROOT) && (si.platform_flags & NUE_PLATFORM_REMOVABLE_STORAGE));
    CHECK(!(si.platform_flags & NUE_PLATFORM_FLATPAK));
    CHECK_EQ(std::string(si.platform_variant), std::string("Steam Deck LCD"));

    nue_power_info pi;
    NUE_STRUCT_INIT(&pi);
    CHECK(m.ops->power_info(&pi) >= 0);
    CHECK_EQ(pi.tdp_watts_current, 15);  // amdgpu power1_cap = 15 W
    CHECK_EQ(pi.tdp_watts_min, -1);

    nue_memory_info mi;
    NUE_STRUCT_INIT(&mi);
    mi.physical_total = 16ull << 30;
    mi.physical_available = 8ull << 30;
    CHECK(m.ops->memory_info_ex(&mi) >= 0);
    CHECK_EQ(mi.process_used, uint64_t(2048) * 1024);
    CHECK_EQ(mi.process_limit, uint64_t(0));  // cgroup limit "max" = none
    CHECK(mi.process_available == mi.physical_available);
    CHECK(mi.unified_memory);
    CHECK_EQ(mi.gpu_shared_total, uint64_t(8589934592ull));
    CHECK(mi.pressure == NUE_PRESSURE_HIGH);  // avg10 = 20 %

    nue_gpu_caps gc;
    NUE_STRUCT_INIT(&gc);
    CHECK(m.ops->gpu_probe(&gc) >= 0);
    CHECK(!gc.probed);  // no Vulkan loader in the host test: the conservative table, labelled as such
    CHECK(gc.format[NUE_FMT_BC1] & NUE_FMTSUP_SAMPLE);
    CHECK(!(gc.format[NUE_FMT_ASTC_LDR] & NUE_FMTSUP_SAMPLE));

    // the glyph override is installed on the first pump: the Deck pad is the one the player holds
    m.ops->pump();
    {
        std::lock_guard<std::mutex> l(stubs::mtx);
        CHECK(!stubs::glyph_overrides.empty());
        CHECK_EQ(stubs::glyph_overrides.back(), int(NUE_GLYPHS_STEAM));
    }
    nue_power_request rq;
    NUE_STRUCT_INIT(&rq);
    rq.tdp_watts = 12;
    nue_power_applied applied = 0;
    // power_apply exists only in the Linux runtime build; there the TDP bit must never be reported as applied
    if (m.ops->power_apply) {
        m.ops->power_apply(&rq, &applied);
        CHECK(!(applied & NUE_POWER_APPLIED_TDP));
    }
}
