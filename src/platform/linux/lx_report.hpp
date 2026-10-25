// Human-readable system report: the text printed by linux_probe and written to the engine log /
// crash reports. Built from the same detection code the engine uses to make its decisions.
#pragma once
#include <string>

#include "lx_fs.hpp"
#include "lx_gpu.hpp"
#include "lx_session.hpp"

namespace nue::lx {

struct ReportOptions {
    bool live_graphics = false;  // dlopen Vulkan/EGL/GL and enumerate devices (Linux only)
    bool headless = false;
    std::string fixture_label;   // shown in the title when reading a captured machine tree
};

// Everything the report decided, for callers that want the data and not the text.
struct ReportData {
    SessionInfo session;
    GraphicsProbe graphics;
    RendererPlan renderer;
};

std::string build_system_report(const IFileSystem& fs, const IEnv& env, const ReportOptions& opt, ReportData* data = nullptr);

}  // namespace nue::lx
