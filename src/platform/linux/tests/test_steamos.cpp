// SteamOS support tests: detection of hardware / flavour / session mode, Steam libraries (VDF), Deck panel
// and frame pacing, Steam Input, on-screen keyboard, amdgpu status, fps caps, storage plan.
#include "fixtures.hpp"
#include "lx_steamos.hpp"
#include "lx_test.hpp"
#include "lx_text.hpp"

using namespace nue::lx;

namespace {
MapEnv deck_gaming_env() {
    MapEnv e;
    e.vars["HOME"] = "/home/deck";
    e.vars["SteamDeck"] = "1";
    e.vars["SteamGamepadUI"] = "1";
    e.vars["SteamGameId"] = "1234567";
    e.vars["SteamAppId"] = "1234567";
    e.vars["XDG_RUNTIME_DIR"] = "/run/user/1000";
    e.vars["DISPLAY"] = ":1";
    return e;
}
}  // namespace

TEST(steamos_detects_deck_gaming_mode) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    MapEnv env = deck_gaming_env();
    SessionInfo ses = detect_session(env, fs);
    SteamOsInfo s = detect_steamos(env, fs, ses);
    CHECK(s.hardware == SteamHardware::DeckLcd);
    CHECK(!s.hardware_is_heuristic);
    CHECK(s.flavor == SteamOsFlavor::SteamOS);
    CHECK_EQ(s.variant_id, std::string("steamdeck"));
    CHECK(s.immutable_root);
    CHECK(s.mode == SessionMode::Gaming);
    CHECK(s.launched_by_steam);
    CHECK_EQ(s.game_id, uint64_t(1234567));
    CHECK(!s.non_steam_shortcut);
    CHECK(!s.proton);
    CHECK_EQ(s.steam_root, std::string("/home/deck/.local/share/Steam"));
    CHECK_EQ(s.sd_cards.size(), size_t(1));
    CHECK_EQ(s.sd_cards[0], std::string("/run/media/deck/GAMES"));
}

TEST(steamos_detects_oled_and_valve_unknown_hardware) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    fs.add_file("/sys/class/dmi/id/product_name", "Galileo\n");
    SteamOsInfo oled = detect_steamos(MapEnv(), fs, SessionInfo());
    CHECK(oled.hardware == SteamHardware::DeckOled);
    fs.add_file("/sys/class/dmi/id/product_name", "SomethingNew\n");
    SteamOsInfo other = detect_steamos(MapEnv(), fs, SessionInfo());
    CHECK(other.steam_hardware());
    CHECK(other.hardware_is_heuristic);  // Steam Machine / Frame are inferred, never claimed as matched
    CHECK(!other.notes.empty());
    MemFs plain;
    CHECK(detect_steamos(MapEnv(), plain, SessionInfo()).hardware == SteamHardware::None);
    MapEnv e;
    e.vars["SteamDeck"] = "1";
    SteamOsInfo env_only = detect_steamos(e, plain, SessionInfo());
    CHECK(env_only.hardware == SteamHardware::DeckLcd && env_only.hardware_is_heuristic);
}

TEST(steamos_like_distros) {
    {
        MemFs fs;
        fixtures::bazzite_desktop(fs);
        MapEnv env;
        env.vars["XDG_CURRENT_DESKTOP"] = "GNOME";
        SessionInfo ses = detect_session(env, fs);
        SteamOsInfo s = detect_steamos(env, fs, ses);
        CHECK(s.flavor == SteamOsFlavor::Bazzite);
        CHECK(s.immutable_root);
        CHECK(s.mode == SessionMode::Desktop);
        CHECK(!s.steam_hardware());
    }
    const struct { const char* id; SteamOsFlavor flavor; } ids[] = {
        {"chimeraos", SteamOsFlavor::ChimeraOS}, {"holoiso", SteamOsFlavor::HoloISO}, {"steamfork", SteamOsFlavor::SteamFork}};
    for (const auto& c : ids) {
        MemFs fs;
        fs.add_file("/etc/os-release", std::string("ID=") + c.id + "\n");
        CHECK(detect_steamos(MapEnv(), fs, SessionInfo()).flavor == c.flavor);
    }
    MemFs nob;
    nob.add_file("/etc/os-release", "ID=nobara\nID_LIKE=fedora\n");
    CHECK(detect_steamos(MapEnv(), nob, SessionInfo()).flavor == SteamOsFlavor::None);  // plain Nobara is not a gaming image
    nob.add_file("/usr/bin/gamescope-session", "");
    CHECK(detect_steamos(MapEnv(), nob, SessionInfo()).flavor == SteamOsFlavor::Nobara);
    MemFs jov;
    jov.add_file("/etc/os-release", "ID=nixos\n");
    jov.add_file("/run/current-system/sw/bin/steamos-manager", "");
    CHECK(detect_steamos(MapEnv(), jov, SessionInfo()).flavor == SteamOsFlavor::Jovian);
    MemFs arch;
    arch.add_file("/etc/os-release", "ID=arch\n");
    CHECK(detect_steamos(MapEnv(), arch, SessionInfo()).flavor == SteamOsFlavor::None);
}

TEST(steamos_non_steam_shortcut_and_proton) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    MapEnv env = deck_gaming_env();
    env.vars["SteamGameId"] = "13255404457033203712";  // (appid << 32) | 0x02000000 style shortcut id
    SteamOsInfo s = detect_steamos(env, fs, detect_session(env, fs));
    CHECK(s.non_steam_shortcut);
    env.vars["SteamGameId"] = "2147483900";  // 32-bit id with the high bit
    CHECK(detect_steamos(env, fs, detect_session(env, fs)).non_steam_shortcut);
    env.vars["STEAM_COMPAT_DATA_PATH"] = "/home/deck/.local/share/Steam/steamapps/compatdata/1";
    SteamOsInfo p = detect_steamos(env, fs, detect_session(env, fs));
    CHECK(p.proton);
}

TEST(steamos_steam_in_flatpak) {
    MemFs fs;
    fs.add_dir("/home/u/.var/app/com.valvesoftware.Steam/.local/share/Steam");
    MapEnv env;
    env.vars["HOME"] = "/home/u";
    SteamOsInfo s = detect_steamos(env, fs, SessionInfo());
    CHECK(s.steam_in_flatpak);
    fs.add_file("/usr/bin/flatpak", "");
    KeyboardRequest k = plan_onscreen_keyboard(s, fs, env);
    CHECK(k.argv.size() == 5 && k.argv[1] == "run" && k.argv[4] == "steam://open/keyboard");
}

TEST(vdf_parser) {
    VdfNode root;
    CHECK(parse_vdf("// c\n\"a\" { \"b\" \"x\\\\y\" \"c\" { \"d\" \"1\" } }\n\"e\" \"v\"", root));
    const VdfNode* a = root.find("A");  // keys are case-insensitive
    CHECK(a && a->is_object);
    CHECK_EQ(a->get("b"), std::string("x\\y"));
    CHECK(a->find("c") && a->find("c")->get("d") == "1");
    CHECK_EQ(root.get("e"), std::string("v"));
    VdfNode bad;
    CHECK(!parse_vdf("\"a\" {", bad));
    CHECK(!parse_vdf("\"a\" }", bad));
    CHECK(!parse_vdf("\"a\" \"unterminated", bad));
}

TEST(steam_libraries_and_app_install) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    auto libs = steam_library_paths(fs, "/home/deck/.local/share/Steam");
    CHECK_EQ(libs.size(), size_t(2));
    CHECK_EQ(libs[1], std::string("/run/media/deck/GAMES"));
    CHECK_EQ(find_app_install(fs, libs, 1234567), std::string("/run/media/deck/GAMES/steamapps/common/NUE Test Game"));
    CHECK(find_app_install(fs, libs, 42).empty());
    fs.add_file("/run/media/deck/GAMES/steamapps/appmanifest_9.acf", "\"AppState\" { \"installdir\" \"../../etc\" }");
    CHECK(find_app_install(fs, libs, 9).empty());  // path traversal in a manifest is refused
    CHECK(steam_library_paths(fs, "").empty());
}

TEST(deck_panels_and_display_plan) {
    PanelInfo lcd = panel_for(SteamHardware::DeckLcd), oled = panel_for(SteamHardware::DeckOled);
    CHECK(lcd.known && lcd.width == 1280 && lcd.height == 800 && lcd.max_hz == 60 && !lcd.oled);
    CHECK(oled.known && oled.max_hz == 90 && oled.oled && oled.hdr);
    CHECK(!panel_for(SteamHardware::SteamMachine).known);

    MemFs fs;
    fixtures::steamos_deck(fs);
    fs.add_file("/sys/class/dmi/id/product_name", "Galileo\n");
    MapEnv env = deck_gaming_env();
    SessionInfo ses = detect_session(env, fs);
    SteamOsInfo s = detect_steamos(env, fs, ses);
    DisplayPlan d = plan_display(s, ses);
    CHECK(d.native_w == 1280 && d.native_h == 800 && d.fullscreen_borderless && d.allow_internal_scaling && d.hdr_available);
}

TEST(frame_pacing_under_gamescope) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    MapEnv env = deck_gaming_env();
    SessionInfo ses = detect_session(env, fs);
    SteamOsInfo lcd = detect_steamos(env, fs, ses);
    FramePacingRequest r;
    r.game_fps = 60;
    r.display_hz = 60;
    FramePacing p = plan_frame_pacing(lcd, ses, r);
    CHECK(p.present_vsync && !p.own_limiter && !p.judder_expected);  // vsync alone

    r.game_fps = 30;
    p = plan_frame_pacing(lcd, ses, r);
    CHECK(p.own_limiter && !p.judder_expected);  // 30 on a 60 Hz panel: sleep to 30

    fs.add_file("/sys/class/dmi/id/product_name", "Galileo\n");
    SteamOsInfo oled = detect_steamos(env, fs, ses);
    r.game_fps = 60;
    r.display_hz = 90;  // OLED at its default refresh: 60 does not divide 90
    p = plan_frame_pacing(oled, ses, r);
    CHECK(p.judder_expected && p.own_limiter);
    CHECK_EQ(p.suggested_steam_limit, 60);

    r.display_hz = 40;  // panel switched below the game's rate
    p = plan_frame_pacing(lcd, ses, r);
    CHECK(!p.judder_expected && p.suggested_steam_limit == 60);

    r.display_hz = 0;
    p = plan_frame_pacing(lcd, ses, r);
    CHECK(p.own_limiter && p.present_vsync);
}

TEST(glyphs_and_ignore_list) {
    CHECK(glyph_family_for(0x28de, 0x1205, true, true) == GlyphFamily::Steam);
    CHECK(glyph_family_for(0x045e, 0x028e, false, false) == GlyphFamily::Xbox);
    CHECK(glyph_family_for(0x054c, 0x0ce6, false, false) == GlyphFamily::PlayStation);
    CHECK(glyph_family_for(0x057e, 0x2009, false, false) == GlyphFamily::Nintendo);
    CHECK(glyph_family_for(0x1234, 1, false, false) == GlyphFamily::Generic);
    std::vector<std::pair<uint16_t, uint16_t>> l;
    CHECK(parse_ignore_devices("0x28de/0x1205, 0x054c/0x0ce6", l));
    CHECK(l.size() == 2 && l[0].first == 0x28de && l[1].second == 0x0ce6);
    CHECK(!parse_ignore_devices("garbage", l) && l.empty());
    CHECK(parse_ignore_devices("", l) && l.empty());
}

TEST(steam_input_plan) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    MapEnv env = deck_gaming_env();
    SessionInfo ses = detect_session(env, fs);
    SteamOsInfo s = detect_steamos(env, fs, ses);
    auto devs = read_input_devices(fs);
    // Steam hides the physical Deck pad and shows only the virtual one
    env.vars["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = "0x28de/0x1205";
    SteamInputPlan p = plan_steam_input(s, ses, env, devs);
    CHECK(p.active && p.physical_hidden);
    CHECK_EQ(p.visible_gamepads, 1);
    CHECK(!p.raw_deck_access && !p.gyro_via_sdl);
    CHECK(p.default_glyphs == GlyphFamily::Steam);
    // without the hiding variable the advice warns about double controllers
    env.vars.erase("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    p = plan_steam_input(s, ses, env, devs);
    CHECK_EQ(p.visible_gamepads, 2);
    bool warned = false;
    for (auto& a : p.advice) warned |= a.find("twice") != std::string::npos;
    CHECK(warned);
    // not launched by Steam: direct access to the Deck pad with gyro and back buttons
    MapEnv bare;
    bare.vars["HOME"] = "/home/deck";
    bare.vars["SteamDeck"] = "1";
    SteamOsInfo s2 = detect_steamos(bare, fs, detect_session(bare, fs));
    SteamInputPlan q = plan_steam_input(s2, detect_session(bare, fs), bare, devs);
    CHECK(!q.active && q.raw_deck_access && q.gyro_via_sdl && q.back_buttons_via_sdl);
    // EXCEPT list: only the listed devices are visible
    env.vars["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"] = "0x28de/0x11ff";
    p = plan_steam_input(s, ses, env, devs);
    CHECK_EQ(p.visible_gamepads, 1);
}

TEST(onscreen_keyboard_request) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    MapEnv env = deck_gaming_env();
    env.vars["PATH"] = "/usr/bin";
    SteamOsInfo s = detect_steamos(env, fs, detect_session(env, fs));
    KeyboardRequest k = plan_onscreen_keyboard(s, fs, env);
    CHECK(k.argv.size() == 3 && k.argv[0] == "/usr/bin/steam" && k.argv[1] == "-ifrunning" && k.argv[2] == "steam://open/keyboard");
    MemFs bare;
    KeyboardRequest none = plan_onscreen_keyboard(s, bare, env);
    CHECK(none.argv.empty() && !none.reason.empty());
    bare.add_file("/usr/bin/xdg-open", "");
    CHECK_EQ(plan_onscreen_keyboard(s, bare, env).argv[0], std::string("/usr/bin/xdg-open"));
}

TEST(amdgpu_status_and_fps_cap) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    AmdGpuStatus g = read_amdgpu_status(fs);
    CHECK(g.present);
    CHECK_EQ(g.busy_percent, 37);
    CHECK_EQ(g.perf_level, std::string("auto"));
    CHECK_EQ(g.sclk_mhz, 1100);
    CHECK_EQ(g.mclk_mhz, 800);
    CHECK(g.power_cap_w > 14.9 && g.power_cap_w < 15.1);
    CHECK(g.power_now_w > 9.2 && g.power_now_w < 9.3);
    CHECK_EQ(g.temp_c, 61);
    MemFs none;
    CHECK(!read_amdgpu_status(none).present);

    SteamOsInfo s;
    s.hardware = SteamHardware::DeckLcd;
    PowerInfo battery;
    BatteryInfo b;
    b.present = true;
    b.percent = 50;
    battery.batteries.push_back(b);
    battery.on_battery = true;
    CHECK_EQ(suggest_fps_cap(s, battery, false, 60), 60);
    battery.batteries[0].percent = 18;
    CHECK_EQ(suggest_fps_cap(s, battery, false, 60), 45);
    battery.batteries[0].percent = 8;
    CHECK_EQ(suggest_fps_cap(s, battery, false, 60), 40);
    CHECK_EQ(suggest_fps_cap(s, battery, true, 60), 30);   // low battery + thermal pressure
    battery.on_battery = false;
    CHECK_EQ(suggest_fps_cap(s, battery, false, 60), 60);  // on AC: no cap
    CHECK_EQ(suggest_fps_cap(SteamOsInfo(), battery, true, 60), 60);  // unknown panel: leave alone
}

TEST(steamos_storage_plan) {
    MemFs fs;
    fixtures::steamos_deck(fs);
    MapEnv env = deck_gaming_env();
    env.vars["MESA_SHADER_CACHE_DIR"] = "/home/deck/.local/share/Steam/steamapps/shadercache/1234567";
    SteamOsInfo s = detect_steamos(env, fs, detect_session(env, fs));
    XdgDirs xdg = resolve_xdg(env, fs);
    SteamOsPaths p = plan_steamos_paths(s, xdg, env, "NUE", "nfsu2");
    CHECK_EQ(p.user_data, std::string("/home/deck/.local/share/NUE/nfsu2"));
    CHECK(p.install_must_stay_in_home);
    CHECK_EQ(p.shader_cache, std::string("/home/deck/.local/share/Steam/steamapps/shadercache/1234567/nue"));
    env.vars.erase("MESA_SHADER_CACHE_DIR");
    p = plan_steamos_paths(s, xdg, env, "NUE", "nfsu2");
    CHECK_EQ(p.shader_cache, std::string("/home/deck/.cache/NUE/nfsu2/shaders"));
    CHECK(!p.notes.empty());
}

TEST(steamos_manager_introspection) {
    const std::string xml =
        "<node>\n <interface name=\"org.freedesktop.DBus.Introspectable\">\n  <method name=\"Introspect\"/>\n </interface>\n"
        " <interface name=\"com.steampowered.SteamOSManager1.TdpLimit1\">\n  <property type=\"u\" name=\"TdpLimit\" access=\"readwrite\"/>\n"
        "  <property type=\"u\" name=\"TdpLimitMin\" access=\"read\"/>\n  <property name=\"TdpLimitMax\" type=\"u\" access=\"read\"/>\n </interface>\n"
        " <interface name=\"com.steampowered.SteamOSManager1.GpuPerformanceLevel1\">\n  <property type=\"s\" name=\"GpuPerformanceLevel\"/>\n </interface>\n</node>";
    auto ifs = parse_introspection_xml(xml);
    CHECK_EQ(ifs.size(), size_t(3));
    CHECK_EQ(ifs[1].name, std::string("com.steampowered.SteamOSManager1.TdpLimit1"));
    CHECK_EQ(ifs[1].properties.size(), size_t(3));
    CHECK(ifs[1].properties[2].first == "TdpLimitMax" && ifs[1].properties[2].second == "u");  // attribute order does not matter
    CHECK(ifs[2].properties.size() == 1 && ifs[2].properties[0].second == "s");
    CHECK(is_steamos_manager_property_of_interest("TdpLimitMin"));
    CHECK(!is_steamos_manager_property_of_interest("Introspect"));
    CHECK(parse_introspection_xml("garbage <").empty());
}
