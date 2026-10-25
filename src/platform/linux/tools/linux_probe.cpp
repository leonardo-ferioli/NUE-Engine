// linux_probe: prints what the machine (or a captured machine tree) supports, using exactly the
// code the engine uses to decide its settings. Opens no window and no audio device.
//
//   linux_probe                     probe this machine
//   linux_probe --live              also load libvulkan/libEGL/... and enumerate GPUs (Linux)
//   linux_probe --root DIR          read /proc, /sys, /etc from DIR instead of "/" (fixtures)
//   linux_probe --env NAME=VALUE    with --root: define an environment variable (repeatable)
//   linux_probe --headless          plan for headless operation (CI)
#include <cstdio>
#include <cstring>
#include <string>

#include "lx_fs.hpp"
#include "lx_report.hpp"

#if defined(NUE_LINUX_RUNTIME)
#include "lx_runtime.hpp"
#endif

using namespace nue::lx;

int main(int argc, char** argv) {
    std::string root;
    ReportOptions opt;
    MapEnv fixture_env;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--live")) opt.live_graphics = true;
        else if (!std::strcmp(argv[i], "--headless")) opt.headless = true;
        else if (!std::strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!std::strcmp(argv[i], "--env") && i + 1 < argc) {
            std::string a = argv[++i];
            size_t eq = a.find('=');
            if (eq != std::string::npos) fixture_env.vars[a.substr(0, eq)] = a.substr(eq + 1);
        } else {
            std::printf("usage: linux_probe [--live] [--headless] [--root DIR [--env NAME=VALUE]...]\n");
            return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    StdFs fs(root);
    ProcessEnv process_env;
    const IEnv& env = root.empty() ? static_cast<const IEnv&>(process_env) : static_cast<const IEnv&>(fixture_env);
    if (!root.empty()) opt.fixture_label = "fixture " + root;

    std::fputs(build_system_report(fs, env, opt).c_str(), stdout);

#if defined(NUE_LINUX_RUNTIME)
    if (root.empty()) {
        std::printf("\n== runtime services ==\n");
        print_runtime_capabilities();
    }
#endif
    std::printf("\n");
    return 0;
}
