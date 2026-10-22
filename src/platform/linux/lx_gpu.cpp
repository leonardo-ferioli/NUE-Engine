#include "lx_gpu.hpp"

#include <algorithm>
#include <cstdio>

#include "lx_text.hpp"

namespace nue::lx {

const char* vendor_name(uint32_t v) {
    switch (v) {
        case 0x1002: return "AMD";
        case 0x10de: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x13b5: return "ARM (Mali)";
        case 0x5143: return "Qualcomm";
        case 0x14e4: return "Broadcom";
        case 0x106b: return "Apple";
        case 0x1af4: return "Red Hat (virtio)";
        case 0x15ad: return "VMware";
        case 0x1414: return "Microsoft";
        case 0x10005: return "Mesa";
        default: return "unknown";
    }
}

const char* to_string(GfxApi a) {
    switch (a) {
        case GfxApi::Vulkan: return "vulkan";
        case GfxApi::OpenGL: return "opengl";
        case GfxApi::OpenGLES: return "opengles";
        default: return "none";
    }
}
const char* bgfx_renderer_name(GfxApi a) {
    switch (a) {
        case GfxApi::Vulkan: return "Vulkan";
        case GfxApi::OpenGL: return "OpenGL";
        case GfxApi::OpenGLES: return "OpenGLES";
        default: return "";
    }
}

// ---------------------------------------------------------------------------- DRM

std::vector<DrmDevice> read_drm_devices(const IFileSystem& fs) {
    std::vector<DrmDevice> out;
    std::vector<DirEntry> ents;
    if (!fs.list_dir("/sys/class/drm", ents)) return out;
    std::sort(ents.begin(), ents.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    for (const auto& e : ents) {
        // "card0", but not connectors such as "card0-HDMI-A-1"
        if (!starts_with(e.name, "card") || e.name.find('-') != std::string::npos) continue;
        DrmDevice d;
        d.card = e.name;
        const std::string dev = "/sys/class/drm/" + e.name + "/device/";
        uint64_t n;
        if (parse_u64(fs.read_line(dev + "vendor"), n, 16)) d.vendor = static_cast<uint32_t>(n);
        if (parse_u64(fs.read_line(dev + "device"), n, 16)) d.device = static_cast<uint32_t>(n);
        d.platform = d.vendor == 0 && d.device == 0;
        std::string drv;
        if (fs.read_link(dev + "driver", drv)) {
            size_t sl = drv.rfind('/');
            d.kernel_driver = sl == std::string::npos ? drv : drv.substr(sl + 1);
        } else {
            std::string ue;
            if (fs.read_file(dev + "uevent", ue))
                for (std::string_view line : split_lines(ue)) {
                    if (starts_with(line, "DRIVER=")) d.kernel_driver = std::string(line.substr(7));
                    else if (starts_with(line, "PCI_SLOT_NAME=")) d.pci_slot = std::string(line.substr(14));
                }
        }
        if (d.pci_slot.empty()) {
            std::string ue;
            if (fs.read_file(dev + "uevent", ue))
                for (std::string_view line : split_lines(ue))
                    if (starts_with(line, "PCI_SLOT_NAME=")) d.pci_slot = std::string(line.substr(14));
        }
        d.boot_vga = fs.read_line(dev + "boot_vga") == "1";
        if (parse_u64(fs.read_line(dev + "mem_info_vram_total"), n)) d.vram_bytes = n;
        // Render node: /sys/class/drm/cardN/device/drm/renderD128
        std::vector<DirEntry> sub;
        if (fs.list_dir(dev + "drm", sub))
            for (const auto& s : sub)
                if (starts_with(s.name, "renderD")) d.render_node = s.name;
        // Dedicated: amdgpu with real VRAM (APUs report a small carve-out, so require > 1 GiB
        // and a PCI device that is not boot VGA on a hybrid laptop is judged by the planner).
        d.discrete_guess = d.vram_bytes > (1ull << 30) || d.kernel_driver == "nvidia" || d.kernel_driver == "nouveau";
        out.push_back(std::move(d));
    }
    return out;
}

// ---------------------------------------------------------------------------- ICDs

bool json_string_field(const std::string& json, const std::string& key, std::string& value) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos);
    if (pos == std::string::npos) return false;
    std::string out;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size()) {
            out += json[++i];
        } else if (c == '"') {
            value = out;
            return true;
        } else {
            out += c;
        }
    }
    return false;
}

VulkanIcd parse_icd_manifest(const std::string& manifest_path, const std::string& json) {
    VulkanIcd icd;
    icd.manifest = manifest_path;
    json_string_field(json, "library_path", icd.library_path);
    std::string api;
    if (json_string_field(json, "api_version", api)) {
        auto parts = split(api, '.');
        uint64_t ma = 1, mi = 0, pa = 0;
        if (parts.size() > 0) parse_u64(parts[0], ma);
        if (parts.size() > 1) parse_u64(parts[1], mi);
        if (parts.size() > 2) parse_u64(parts[2], pa);
        icd.api_version = static_cast<uint32_t>((ma << 22) | (mi << 12) | pa);
    }
    const std::string key = to_lower_ascii(manifest_path.substr(manifest_path.rfind('/') == std::string::npos ? 0 : manifest_path.rfind('/') + 1)) + " " + to_lower_ascii(icd.library_path);
    struct Rule { const char* needle; const char* driver; bool software; };
    static const Rule rules[] = {
        {"radeon_icd", "radv", false}, {"libvulkan_radeon", "radv", false}, {"amd_icd", "amdvlk", false},
        {"intel_icd", "anv", false}, {"libvulkan_intel", "anv", false}, {"intel_hasvk", "hasvk", false},
        {"nvidia_icd", "nvidia", false}, {"libglx_nvidia", "nvidia", false},{"nouveau", "nvk", false}, {"nvk", "nvk", false},
        {"lvp_icd", "lavapipe", true}, {"libvulkan_lvp", "lavapipe", true},
        {"freedreno", "turnip", false}, {"broadcom_icd", "v3dv", false}, {"asahi", "honeykrisp", false},
        {"panfrost", "panvk", false}, {"virtio", "venus", false}, {"gfxstream", "gfxstream", false},
        {"swiftshader", "swiftshader", true},
    };
    for (const auto& r : rules)
        if (key.find(r.needle) != std::string::npos) {
            icd.driver = r.driver;
            icd.software = r.software;
            break;
        }
    return icd;
}

std::vector<VulkanIcd> read_vulkan_icds(const IEnv& env, const IFileSystem& fs, const XdgDirs& xdg) {
    std::vector<std::string> files;
    std::string override_list = env.get_or("VK_DRIVER_FILES");
    if (override_list.empty()) override_list = env.get_or("VK_ICD_FILENAMES");
    if (!override_list.empty()) {
        for (std::string_view f : split(override_list, ':')) files.emplace_back(f);
    } else {
        std::vector<std::string> dirs;
        auto add = [&](const std::string& base) {
            if (base.empty()) return;
            std::string d = base + "/vulkan/icd.d";
            if (std::find(dirs.begin(), dirs.end(), d) == dirs.end()) dirs.push_back(d);
        };
        add(xdg.config_home);
        for (const auto& c : xdg.config_dirs) add(c);
        add("/etc");
        add(xdg.data_home);
        for (const auto& d : xdg.data_dirs) add(d);
        add("/usr/local/share");
        add("/usr/share");
        for (const auto& d : dirs) {
            std::vector<DirEntry> ents;
            if (!fs.list_dir(d, ents)) continue;
            std::sort(ents.begin(), ents.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
            for (const auto& e : ents)
                if (ends_with(e.name, ".json")) files.push_back(d + "/" + e.name);
        }
    }
    std::vector<VulkanIcd> out;
    for (const auto& f : files) {
        std::string json;
        if (!fs.read_file(f, json, 65536)) continue;
        VulkanIcd icd = parse_icd_manifest(f, json);
        if (!icd.library_path.empty()) out.push_back(std::move(icd));
    }
    return out;
}

// ---------------------------------------------------------------------------- planner

namespace {

struct Candidate {
    std::string name;
    uint32_t vendor = 0, device = 0;
    bool discrete = false, integrated = false, software = false, boot = false;
    uint32_t api_version = 0;
};

std::string hex4(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%04x", v);
    return b;
}

void add_env(RendererPlan& p, const char* k, const std::string& v) { p.env.emplace_back(k, v); }

}  // namespace

RendererPlan plan_renderer(const GraphicsProbe& probe, const PlanRequest& req, const SessionInfo& session) {
    RendererPlan plan;
    std::vector<Candidate> cands;

    if (probe.vk.attempted && probe.vk.instance_ok) {
        for (const auto& d : probe.vk.devices) {
            Candidate c;
            c.name = d.name;
            c.vendor = d.vendor_id;
            c.device = d.device_id;
            c.discrete = d.type == 2;
            c.integrated = d.type == 1;
            c.software = d.type == 4;
            c.api_version = d.api_version;
            for (const auto& g : probe.drm)
                if (g.vendor == d.vendor_id && g.device == d.device_id) c.boot = g.boot_vga;
            cands.push_back(std::move(c));
        }
    } else {
        // No live probe: infer from sysfs, assuming each GPU has a driver in the ICD list.
        for (const auto& g : probe.drm) {
            Candidate c;
            c.name = std::string(vendor_name(g.vendor)) + " " + hex4(g.vendor) + ":" + hex4(g.device) + (g.kernel_driver.empty() ? "" : " (" + g.kernel_driver + ")");
            c.vendor = g.vendor;
            c.device = g.device;
            c.discrete = g.discrete_guess;
            c.integrated = !g.discrete_guess;
            c.boot = g.boot_vga;
            cands.push_back(std::move(c));
        }
    }
    const bool vulkan_possible = (probe.vk.attempted ? probe.vk.instance_ok && !probe.vk.devices.empty() : !probe.icds.empty());
    const bool vulkan_loader_ok = probe.vk.attempted ? probe.vk.loader_found : true;

    // Is a software ICD the only thing there?
    bool only_software = !cands.empty() && std::all_of(cands.begin(), cands.end(), [](const Candidate& c) { return c.software; });
    if (!probe.vk.attempted && !probe.icds.empty() && std::all_of(probe.icds.begin(), probe.icds.end(), [](const VulkanIcd& i) { return i.software; })) only_software = true;

    // ---- device choice
    const Candidate* chosen = nullptr;
    auto usable = [&](const Candidate& c) { return !c.software || req.allow_software || only_software; };
    if (req.vendor_id != 0) {
        for (const auto& c : cands)
            if (c.vendor == req.vendor_id && (req.device_id == 0 || c.device == req.device_id) && usable(c)) { chosen = &c; break; }
        if (!chosen) plan.warnings.push_back("requested GPU " + hex4(req.vendor_id) + ":" + hex4(req.device_id) + " not found, using automatic choice");
    }
    if (!chosen && req.device_index >= 0 && static_cast<size_t>(req.device_index) < cands.size()) chosen = &cands[static_cast<size_t>(req.device_index)];
    if (!chosen) {
        auto pick = [&](auto pred) -> const Candidate* {
            for (const auto& c : cands)
                if (usable(c) && pred(c)) return &c;
            return nullptr;
        };
        if (req.prefer_low_power) {
            chosen = pick([](const Candidate& c) { return c.integrated && !c.software; });
            if (!chosen) chosen = pick([](const Candidate& c) { return !c.discrete && !c.software; });
        }
        if (!chosen) chosen = pick([](const Candidate& c) { return c.discrete && !c.software; });
        if (!chosen) chosen = pick([](const Candidate& c) { return c.integrated && !c.software; });
        if (!chosen) chosen = pick([](const Candidate&) { return true; });
    }
    plan.hybrid_gpu = std::count_if(cands.begin(), cands.end(), [](const Candidate& c) { return !c.software; }) > 1;
    if (chosen) {
        plan.device_name = chosen->name;
        plan.software = chosen->software;
        plan.bgfx_vendor_id = static_cast<uint16_t>(chosen->vendor);
        plan.bgfx_device_id = static_cast<uint16_t>(chosen->device);
        if (chosen->api_version && chosen->api_version < (1u << 22 | 1u << 12))
            plan.warnings.push_back("Vulkan API version below 1.1 on the chosen device; bgfx may refuse it");
    }

    // ---- API choice
    GfxApi preferred = GfxApi::Vulkan;
    if (req.force_api) {
        preferred = req.api;
        plan.notes.push_back(std::string("renderer forced to ") + to_string(req.api) + " by settings");
    }
    std::vector<GfxApi> order;
    auto push = [&](GfxApi a) {
        if (std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a);
    };
    push(preferred);
    if (!req.force_api) {
        push(GfxApi::Vulkan);
        // Wayland-only and SoC GPUs expose GLES; desktop GL where libGL/libOpenGL exists.
        const bool arm_soc = !probe.drm.empty() && std::all_of(probe.drm.begin(), probe.drm.end(), [](const DrmDevice& d) { return d.platform; });
        if (arm_soc || !(probe.gl.gl || probe.gl.opengl)) {
            push(GfxApi::OpenGLES);
            push(GfxApi::OpenGL);
        } else {
            push(GfxApi::OpenGL);
            push(GfxApi::OpenGLES);
        }
    }
    auto available = [&](GfxApi a) {
        switch (a) {
            case GfxApi::Vulkan: return vulkan_loader_ok && vulkan_possible;
            case GfxApi::OpenGL: return probe.gl.gl || probe.gl.opengl;
            case GfxApi::OpenGLES: return probe.gl.gles2 || probe.gl.egl;
            default: return false;
        }
    };
    for (GfxApi a : order) {
        if (!available(a)) continue;
        if (plan.api == GfxApi::None) plan.api = a;
        else plan.fallbacks.push_back(a);
    }
    if (plan.api == GfxApi::None) {
        plan.warnings.push_back("no usable graphics API found: install a Vulkan driver (Mesa radv/anv or NVIDIA) and libvulkan1");
        return plan;
    }
    if (plan.api != GfxApi::Vulkan && !req.force_api)
        plan.notes.push_back(std::string("Vulkan unavailable: falling back to ") + to_string(plan.api));
    if (plan.software && !req.allow_software)
        plan.warnings.push_back("only a software Vulkan driver (llvmpipe/lavapipe) is present: expect a few frames per second");

    // ---- surface extensions vs display server
    if (probe.vk.attempted && probe.vk.instance_ok) {
        if (!probe.vk.ext_surface) plan.warnings.push_back("VK_KHR_surface missing in the Vulkan loader");
        if (session.display_server == DisplayServer::Wayland && !probe.vk.ext_wayland) plan.warnings.push_back("Wayland session but VK_KHR_wayland_surface is not offered; falling back to X11 may be needed (SDL_VIDEO_DRIVER=x11)");
        if (session.display_server == DisplayServer::X11 && !(probe.vk.ext_xcb || probe.vk.ext_xlib)) plan.warnings.push_back("X11 session but no xcb/xlib Vulkan surface extension");
    }

    // ---- environment for multi-GPU machines (set before the loader / GL is initialised)
    if (plan.hybrid_gpu && chosen && !chosen->software) {
        plan.notes.push_back("hybrid GPU system: selecting " + plan.device_name);
        const std::string sel = hex4(chosen->vendor) + ":" + hex4(chosen->device);
        add_env(plan, "MESA_VK_DEVICE_SELECT", sel);
        add_env(plan, "MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1");
        if (chosen->vendor == kVendorNvidia) {
            add_env(plan, "__NV_PRIME_RENDER_OFFLOAD", "1");
            add_env(plan, "__VK_LAYER_NV_optimus", "NVIDIA_only");
            add_env(plan, "__GLX_VENDOR_LIBRARY_NAME", "nvidia");
        } else if (!chosen->boot) {
            // The selected GPU is not the one driving the display: render there and copy over PRIME.
            add_env(plan, "DRI_PRIME", "1");
        }
    }
    if (session.steam_deck) plan.notes.push_back("Steam Deck: single APU, no GPU selection needed");
    return plan;
}

}  // namespace nue::lx
