// Test environment: initialises the engine on the null backend with a private temporary user/data folder.
#pragma once
#include "nue/nue.h"
#include "nue/null_backend.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace nue_test {

inline std::string unique_temp_dir(const char* tag) {
    namespace fs = std::filesystem;
    static int counter = 0;
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path p = fs::temp_directory_path() / ("nue_test_" + std::string(tag) + "_" + std::to_string(t) + "_" + std::to_string(counter++));
    fs::create_directories(p);
    return p.generic_string();
}

inline void write_file(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f << content;
}

struct Engine {
    std::string root, data, user;
    bool ok = false;
    explicit Engine(uint32_t extra_flags = NUE_INIT_DETERMINISTIC, bool log_file = false) {
        root = unique_temp_dir("env");
        data = root + "/data";
        user = root + "/user";
        std::filesystem::create_directories(data);
        nue_init_desc d;
        NUE_STRUCT_INIT(&d);
        d.api_version = NUE_API_VERSION;
        d.app_name = "NUE test";
        d.app_id = "nue-test";
        d.org_name = "NUE";
        d.backend = NUE_BACKEND_NULL;
        d.flags = NUE_INIT_HEADLESS | NUE_INIT_NO_CRASH_HANDLER | extra_flags | (log_file ? 0 : NUE_INIT_NO_LOG_FILE);
        d.game_data_path = data.c_str();
        d.user_data_path = user.c_str();
        d.log_level = NUE_LOG_WARN;
        ok = NUE_SUCCEEDED(nue_init(&d));
        if (!ok) std::printf("  nue_init failed: %s\n", nue_get_last_error_message());
    }
    ~Engine() {
        nue_shutdown();
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
};

// Drain the queue into a vector of event types (does not pump).
inline int drain(nue_event* out, int max) {
    int n = 0;
    while (n < max && nue_poll_event(&out[n])) ++n;
    return n;
}

}  // namespace nue_test
