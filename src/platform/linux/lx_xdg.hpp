// XDG Base Directory resolution (data / config / cache / state / runtime) plus user dirs,
// and the per-application directory layout of the engine on Linux.
#pragma once
#include <string>
#include <vector>

#include "lx_fs.hpp"

namespace nue::lx {

struct XdgDirs {
    std::string home;         // $HOME, or the fallback passed in (empty if unknown)
    std::string data_home;    // $XDG_DATA_HOME   (~/.local/share)
    std::string config_home;  // $XDG_CONFIG_HOME (~/.config)
    std::string cache_home;   // $XDG_CACHE_HOME  (~/.cache)
    std::string state_home;   // $XDG_STATE_HOME  (~/.local/state)
    std::string runtime_dir;  // $XDG_RUNTIME_DIR, empty when missing/invalid (never invented here)
    std::vector<std::string> data_dirs;    // $XDG_DATA_DIRS   (/usr/local/share:/usr/share)
    std::vector<std::string> config_dirs;  // $XDG_CONFIG_DIRS (/etc/xdg)
    std::string documents, downloads, desktop;  // from user-dirs.dirs, may be empty
};

// Relative values are ignored, as the specification requires. `home_fallback` is used when
// $HOME is unset (the caller passes the passwd entry).
XdgDirs resolve_xdg(const IEnv& env, const IFileSystem& fs, const std::string& home_fallback = std::string());

// Parses ~/.config/user-dirs.dirs content ($HOME expansion included).
void parse_user_dirs(const std::string& text, const std::string& home, XdgDirs& out);

struct AppDirs {
    std::string data;    // read-only-ish payload the engine installs (shaders cache index, mods)
    std::string config;  // settings
    std::string cache;   // shader/pipeline caches, safe to delete
    std::string state;   // logs, crash reports, history
    std::string saves;   // save games (under data)
    std::string logs;    // under state
    std::string crashes; // under state
};

// <home>/<app> below each base directory. `app_id` is a reverse-DNS or short lower-case id.
AppDirs app_dirs(const XdgDirs& xdg, const std::string& app_id);

// Every directory in the list that is non-empty and absolute, de-duplicated, in order.
std::vector<std::string> split_dir_list(const std::string& colon_separated);

}  // namespace nue::lx
