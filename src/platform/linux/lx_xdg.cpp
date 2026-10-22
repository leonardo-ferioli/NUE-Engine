#include "lx_xdg.hpp"

#include <algorithm>

#include "lx_text.hpp"

namespace nue::lx {

namespace {

bool absolute(const std::string& p) { return !p.empty() && p[0] == '/'; }

std::string strip_trailing(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

std::string pick(const IEnv& env, const char* var, const std::string& fallback) {
    std::string v;
    if (env.get(var, v) && absolute(v)) return strip_trailing(v);
    return fallback;
}

}  // namespace

std::vector<std::string> split_dir_list(const std::string& s) {
    std::vector<std::string> out;
    for (std::string_view part : split(s, ':')) {
        std::string p = strip_trailing(std::string(part));
        if (absolute(p) && std::find(out.begin(), out.end(), p) == out.end()) out.push_back(std::move(p));
    }
    return out;
}

void parse_user_dirs(const std::string& text, const std::string& home, XdgDirs& out) {
    for (std::string_view line : split_lines(text)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(trim(line.substr(0, eq)));
        std::string val(trim(line.substr(eq + 1)));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
        if (starts_with(val, "$HOME")) val = home + val.substr(5);
        if (!absolute(val)) continue;
        val = strip_trailing(val);
        if (key == "XDG_DOCUMENTS_DIR") out.documents = val;
        else if (key == "XDG_DOWNLOAD_DIR") out.downloads = val;
        else if (key == "XDG_DESKTOP_DIR") out.desktop = val;
    }
}

XdgDirs resolve_xdg(const IEnv& env, const IFileSystem& fs, const std::string& home_fallback) {
    XdgDirs x;
    x.home = pick(env, "HOME", strip_trailing(home_fallback));
    const std::string h = x.home;
    x.data_home = pick(env, "XDG_DATA_HOME", h.empty() ? "" : h + "/.local/share");
    x.config_home = pick(env, "XDG_CONFIG_HOME", h.empty() ? "" : h + "/.config");
    x.cache_home = pick(env, "XDG_CACHE_HOME", h.empty() ? "" : h + "/.cache");
    x.state_home = pick(env, "XDG_STATE_HOME", h.empty() ? "" : h + "/.local/state");
    x.runtime_dir = pick(env, "XDG_RUNTIME_DIR", "");
    x.data_dirs = split_dir_list(env.get_or("XDG_DATA_DIRS"));
    if (x.data_dirs.empty()) x.data_dirs = {"/usr/local/share", "/usr/share"};
    x.config_dirs = split_dir_list(env.get_or("XDG_CONFIG_DIRS"));
    if (x.config_dirs.empty()) x.config_dirs = {"/etc/xdg"};
    std::string ud;
    if (!x.config_home.empty() && fs.read_file(x.config_home + "/user-dirs.dirs", ud, 65536)) parse_user_dirs(ud, h, x);
    return x;
}

AppDirs app_dirs(const XdgDirs& x, const std::string& app_id) {
    AppDirs a;
    auto join = [&](const std::string& base) { return base.empty() ? std::string() : base + "/" + app_id; };
    a.data = join(x.data_home);
    a.config = join(x.config_home);
    a.cache = join(x.cache_home);
    a.state = join(x.state_home);
    if (!a.data.empty()) a.saves = a.data + "/saves";
    if (!a.state.empty()) {
        a.logs = a.state + "/logs";
        a.crashes = a.state + "/crashes";
    }
    return a;
}

}  // namespace nue::lx
