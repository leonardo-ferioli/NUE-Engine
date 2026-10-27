// Registry: the Config-backed tree on its own, then the same call script through it and through the real Windows registry (a scratch key under
// HKEY_CURRENT_USER\Software, deleted at the end) with every return code and value compared.
#include "nue_legacy/win32/registry.hpp"
#include "nue_legacy/platform/std_platform.hpp"
#include "test_support.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
#include "nue_legacy/win32/host/windows_registry.hpp"
#include <windows.h>
#endif

using namespace nue::legacy::win32;
namespace stdlib = nue::legacy::platform::stdlib;

static void test_registry() {
    auto storage = stdlib::make_storage();
    auto config = stdlib::make_config(storage.get());
    auto reg = make_config_registry(*config);
    Handle k = 0, k2 = 0;
    uint32_t disposition = 0;
    CHECK_EQ(reg->open_key(hkey::current_user, "Software\\Missing", k), uint32_t(err::file_not_found));
    CHECK_EQ(reg->create_key(hkey::current_user, "Software\\EA Games\\Game", k, &disposition), 0u);
    CHECK_EQ(disposition, uint32_t(reg_disposition::created_new_key));
    CHECK(k != 0);
    CHECK_EQ(reg->create_key(hkey::current_user, "software\\ea games\\GAME", k2, &disposition), 0u);   // case-insensitive
    CHECK_EQ(disposition, uint32_t(reg_disposition::opened_existing_key));
    // Values of each type.
    const uint8_t dword[4] = {0x78, 0x56, 0x34, 0x12};
    CHECK_EQ(reg->set_value(k, "Resolution", reg::dword, dword, 4), 0u);
    CHECK_EQ(reg->set_value(k, "Name", reg::sz, reinterpret_cast<const uint8_t*>("Hello"), 6), 0u);
    uint32_t type = 0, size = 0;
    uint8_t buffer[16] = {};
    CHECK_EQ(reg->query_value(k2, "RESOLUTION", &type, nullptr, &size), 0u);                // through the other handle, other case
    CHECK(type == reg::dword && size == 4);
    size = 2;
    CHECK_EQ(reg->query_value(k, "Resolution", &type, buffer, &size), uint32_t(err::more_data));
    CHECK_EQ(size, 4u);
    size = 16;
    CHECK_EQ(reg->query_value(k, "Resolution", &type, buffer, &size), 0u);
    CHECK(size == 4 && std::memcmp(buffer, dword, 4) == 0);
    CHECK_EQ(reg->query_value(k, "Nope", &type, buffer, &size), uint32_t(err::file_not_found));
    // The default value has the empty name.
    CHECK_EQ(reg->set_value(k, "", reg::sz, reinterpret_cast<const uint8_t*>("x"), 2), 0u);
    size = 16;
    CHECK_EQ(reg->query_value(k, "", &type, buffer, &size), 0u);
    // Subkeys, enumeration, deletion.
    Handle sub = 0;
    CHECK_EQ(reg->create_key(k, "Video", sub, nullptr), 0u);
    std::string name;
    CHECK_EQ(reg->enum_key(k, 0, name), 0u);
    CHECK(name == "Video");
    CHECK_EQ(reg->enum_key(k, 1, name), uint32_t(err::no_more_items));
    CHECK_EQ(reg->enum_value(k, 2, name, &type), 0u);
    CHECK_EQ(reg->delete_key(hkey::current_user, "Software\\EA Games\\Game"), uint32_t(err::access_denied));   // has a subkey
    CHECK_EQ(reg->delete_value(k, "Name"), 0u);
    CHECK_EQ(reg->delete_value(k, "Name"), uint32_t(err::file_not_found));
    // Handles.
    CHECK_EQ(reg->close_key(sub), 0u);
    CHECK_EQ(reg->close_key(sub), uint32_t(err::invalid_handle));
    size = 16;
    CHECK_EQ(reg->query_value(sub, "x", &type, buffer, &size), uint32_t(err::invalid_handle));
    CHECK_EQ(reg->close_key(hkey::local_machine), 0u);
    CHECK_EQ(reg->open_key(0x1234, "x", k2), uint32_t(err::invalid_handle));
    // Persistence: save the config, load it into a fresh one, and the tree is back.
    const std::string text = config->save_text();
    auto config2 = stdlib::make_config(storage.get());
    config2->load_text(text);
    auto reg2 = make_config_registry(*config2);
    Handle again = 0;
    CHECK_EQ(reg2->open_key(hkey::current_user, "Software\\EA Games\\Game\\Video", again), 0u);
    CHECK_EQ(reg2->open_key(hkey::current_user, "Software\\EA Games\\Game", again), 0u);
    size = 16;
    CHECK_EQ(reg2->query_value(again, "Resolution", &type, buffer, &size), 0u);
    CHECK(size == 4 && std::memcmp(buffer, dword, 4) == 0);
    CHECK_EQ(reg2->query_value(again, "Name", &type, buffer, &size), uint32_t(err::file_not_found));   // deleted before saving
}
REGISTER_TEST("registry", test_registry);

#if defined(_WIN32)
namespace {
// The call script: every observable result becomes one line of text.
std::vector<std::string> run_script(Registry& reg, const std::string& base) {
    std::vector<std::string> log;
    auto line = [&](const std::string& what, uint32_t code, const std::string& extra = "") { log.push_back(what + " -> " + std::to_string(code) + (extra.empty() ? "" : " " + extra)); };
    Handle root = 0, a = 0, b = 0, c = 0;
    uint32_t disposition = 0, type = 0, size = 0;
    uint8_t buffer[64];
    line("create", reg.create_key(hkey::current_user, base, root, &disposition), std::to_string(disposition));
    line("create again", reg.create_key(hkey::current_user, base, a, &disposition), std::to_string(disposition));
    line("open", reg.open_key(hkey::current_user, base, b));
    line("open missing", reg.open_key(root, "missing\\deeper", c));
    line("open empty sub", reg.open_key(root, "", c));
    reg.close_key(c);
    const uint8_t dword[4] = {1, 2, 3, 4};
    const uint8_t multi[] = {'a', 0, 'b', 'c', 0, 0};
    line("set dword", reg.set_value(root, "Dword", reg::dword, dword, 4));
    line("set sz", reg.set_value(root, "Text", reg::sz, reinterpret_cast<const uint8_t*>("hello"), 6));
    line("set binary", reg.set_value(root, "Blob", reg::binary, buffer, 0));
    line("set multi", reg.set_value(root, "Multi", reg::multi_sz, multi, 6));
    line("set expand", reg.set_value(root, "Expand", reg::expand_sz, reinterpret_cast<const uint8_t*>("%PATH%"), 7));
    line("set default", reg.set_value(root, "", reg::sz, reinterpret_cast<const uint8_t*>("dflt"), 5));
    line("overwrite", reg.set_value(root, "dword", reg::sz, reinterpret_cast<const uint8_t*>("now text"), 9));   // other case, other type
    for (const char* name : {"Dword", "DWORD", "Text", "Blob", "Multi", "Expand", "", "Absent"}) {
        size = sizeof buffer;
        std::memset(buffer, 0xCC, sizeof buffer);
        const uint32_t code = reg.query_value(root, name, &type, buffer, &size);
        std::string extra = "type=" + std::to_string(type) + " size=" + std::to_string(size);
        for (uint32_t i = 0; code == 0 && i < size && i < 16; ++i) extra += " " + std::to_string(buffer[i]);
        line(std::string("query ") + name, code, extra);
        uint32_t needed = 0;
        const uint32_t sized = reg.query_value(root, name, nullptr, nullptr, &needed);
        line(std::string("query size ") + name, sized, std::to_string(needed));
        uint32_t tiny = 1;
        uint8_t one[1];
        const uint32_t more = reg.query_value(root, name, &type, one, &tiny);
        line(std::string("query small ") + name, more, std::to_string(tiny));
    }
    // Subkeys and enumeration (compared as sorted sets: the registry's enumeration order is not specified).
    Handle sub1 = 0, sub2 = 0;
    line("create sub1", reg.create_key(root, "Alpha", sub1, nullptr));
    line("create sub2", reg.create_key(root, "beta\\deep", sub2, nullptr));
    std::vector<std::string> names;
    std::string name;
    uint32_t code;
    for (uint32_t i = 0; (code = reg.enum_key(root, i, name)) == 0; ++i) { std::string lower = name; for (auto& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch))); names.push_back(lower); }
    line("enum_key end", code);
    std::sort(names.begin(), names.end());
    std::string joined;
    for (auto& n : names) joined += n + ",";
    line("subkeys", 0, joined);
    names.clear();
    for (uint32_t i = 0; (code = reg.enum_value(root, i, name, &type)) == 0; ++i) { std::string lower = name; for (auto& ch : lower) ch = char(std::tolower(static_cast<unsigned char>(ch))); names.push_back(lower + ":" + std::to_string(type)); }
    line("enum_value end", code);
    std::sort(names.begin(), names.end());
    joined.clear();
    for (auto& n : names) joined += n + ",";
    line("values", 0, joined);
    // Deletion rules.
    line("delete value", reg.delete_value(root, "TEXT"));
    line("delete value again", reg.delete_value(root, "TEXT"));
    line("delete key with child", reg.delete_key(root, "beta"));
    line("delete leaf", reg.delete_key(root, "beta\\deep"));
    line("delete empty parent", reg.delete_key(root, "beta"));
    line("delete missing", reg.delete_key(root, "beta"));
    // Handle rules.
    line("close sub1", reg.close_key(sub1));
    line("close sub1 again", reg.close_key(sub1));
    size = sizeof buffer;
    line("query on closed", reg.query_value(sub1, "x", &type, buffer, &size));
    line("close sub2", reg.close_key(sub2));
    line("close a", reg.close_key(a));
    line("close b", reg.close_key(b));
    line("set on closed", reg.set_value(a, "x", reg::dword, dword, 4));
    reg.close_key(root);
    return log;
}
}  // namespace

static void test_registry_against_windows() {
    const std::string base = "Software\\nue_legacy_win32_test_" + std::to_string(GetCurrentProcessId());
    // The real one first (and its cleanup), then the portable one on a plain Config.
    auto real = nue::legacy::win32::host::make_windows_registry();
    const auto expected = run_script(*real, base);
    {
        Handle h = 0;
        if (real->open_key(hkey::current_user, base, h) == 0) real->close_key(h);
        RegDeleteKeyA(HKEY_CURRENT_USER, (base + "\\Alpha").c_str());
        RegDeleteKeyA(HKEY_CURRENT_USER, base.c_str());
    }
    HKEY probe = nullptr;
    CHECK(RegOpenKeyExA(HKEY_CURRENT_USER, base.c_str(), 0, KEY_READ, &probe) != ERROR_SUCCESS);   // the scratch key is gone
    if (probe) RegCloseKey(probe);
    auto storage = stdlib::make_storage();
    auto config = stdlib::make_config(storage.get());
    auto mine_reg = make_config_registry(*config);
    const auto mine = run_script(*mine_reg, base);
    CHECK_EQ(expected.size(), mine.size());
    int differences = 0;
    for (size_t i = 0; i < (std::min)(expected.size(), mine.size()); ++i) {
        if (expected[i] != mine[i]) { ++differences; std::fprintf(stderr, "step %zu\n  windows: %s\n  mine:    %s\n", i, expected[i].c_str(), mine[i].c_str()); }
    }
    CHECK_EQ(differences, 0);
}
REGISTER_TEST("registry_windows", test_registry_against_windows);
#endif
