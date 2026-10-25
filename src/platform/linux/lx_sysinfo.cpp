#include "lx_sysinfo.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "lx_text.hpp"

namespace nue::lx {

namespace {

// "key : value" / "key: value" -> (key, value). Returns false for lines without a colon.
bool kv_colon(std::string_view line, std::string_view& k, std::string_view& v) {
    size_t c = line.find(':');
    if (c == std::string_view::npos) return false;
    k = trim(line.substr(0, c));
    v = trim(line.substr(c + 1));
    return true;
}

std::string sys_cpu(int id, const char* leaf) { return "/sys/devices/system/cpu/cpu" + std::to_string(id) + "/" + leaf; }

}  // namespace

CpuInfoText parse_cpuinfo(const std::string& text) {
    CpuInfoText c;
    std::string impl, part;
    bool in_stanza = false;
    auto flush = [&] {
        if (in_stanza && (!impl.empty() || !part.empty())) c.arm_parts.push_back(impl + ":" + part);
        impl.clear();
        part.clear();
    };
    std::string hardware, model;
    for (std::string_view line : split_lines(text)) {
        std::string_view k, v;
        if (!kv_colon(line, k, v)) continue;
        if (k == "processor") {
            flush();
            in_stanza = true;
            ++c.processors;
        } else if (k == "model name" && c.model_name.empty()) {
            c.model_name = v;
        } else if (k == "vendor_id" && c.vendor.empty()) {
            c.vendor = v;
        } else if ((k == "flags" || k == "Features") && c.flags.empty()) {
            c.flags = v;
            if (k == "Features") c.is_arm = true;
        } else if (k == "CPU implementer") {
            impl = v;
            c.is_arm = true;
        } else if (k == "CPU part") {
            part = v;
            c.is_arm = true;
        } else if (k == "Hardware") {
            hardware = v;
        } else if (k == "Model") {
            model = v;
        }
    }
    flush();
    if (c.model_name.empty()) c.model_name = !model.empty() ? model : hardware;
    if (c.vendor.empty() && c.is_arm && !c.arm_parts.empty()) c.vendor = "ARM implementer " + c.arm_parts[0].substr(0, c.arm_parts[0].find(':'));
    return c;
}

bool cpu_has_flag(const CpuInfoText& c, const std::string& flag) {
    for (std::string_view f : split(c.flags, ' '))
        if (f == flag) return true;
    return false;
}

MemInfo parse_meminfo(const std::string& text) {
    MemInfo m;
    for (std::string_view line : split_lines(text)) {
        std::string_view k, v;
        if (!kv_colon(line, k, v)) continue;
        uint64_t n = 0;
        size_t sp = v.find(' ');
        if (!parse_u64(sp == std::string_view::npos ? v : v.substr(0, sp), n)) continue;
        if (k == "MemTotal") m.total_kb = n;
        else if (k == "MemFree") m.free_kb = n;
        else if (k == "MemAvailable") m.available_kb = n;
        else if (k == "SwapTotal") m.swap_total_kb = n;
        else if (k == "SwapFree") m.swap_free_kb = n;
    }
    if (m.available_kb == 0) m.available_kb = m.free_kb;  // very old kernels
    return m;
}

OsRelease parse_os_release(const std::string& text) {
    OsRelease o;
    for (std::string_view line : split_lines(text)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key(line.substr(0, eq));
        std::string val(trim(line.substr(eq + 1)));
        if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front()) val = val.substr(1, val.size() - 2);
        if (key == "ID") o.id = val;
        else if (key == "ID_LIKE") o.id_like = val;
        else if (key == "NAME") o.name = val;
        else if (key == "PRETTY_NAME") o.pretty_name = val;
        else if (key == "VERSION_ID") o.version_id = val;
        else if (key == "VARIANT_ID") o.variant_id = val;
        else if (key == "BUILD_ID") o.build_id = val;
    }
    return o;
}

bool parse_proc_stat_cpu(const std::string& text, CpuTimes& out) {
    for (std::string_view line : split_lines(text)) {
        if (!starts_with(line, "cpu ")) continue;
        std::vector<uint64_t> f;
        for (std::string_view tok : split(line.substr(4), ' ')) {
            uint64_t n;
            if (!parse_u64(tok, n)) return false;
            f.push_back(n);
        }
        if (f.size() < 4) return false;
        uint64_t total = 0;
        for (size_t i = 0; i < f.size() && i < 8; ++i) total += f[i];
        uint64_t idle = f[3] + (f.size() > 4 ? f[4] : 0);
        out.total = total;
        out.busy = total > idle ? total - idle : 0;
        return true;
    }
    return false;
}

double cpu_busy_fraction(const CpuTimes& a, const CpuTimes& b) {
    if (b.total <= a.total) return 0.0;
    double d = static_cast<double>(b.total - a.total);
    double busy = b.busy >= a.busy ? static_cast<double>(b.busy - a.busy) : 0.0;
    return std::min(1.0, busy / d);
}

ProcStatus parse_proc_status(const std::string& text) {
    ProcStatus s;
    for (std::string_view line : split_lines(text)) {
        std::string_view k, v;
        if (!kv_colon(line, k, v)) continue;
        uint64_t n = 0;
        size_t sp = v.find(' ');
        std::string_view num = sp == std::string_view::npos ? v : v.substr(0, sp);
        if (k == "VmRSS" && parse_u64(num, n)) s.vm_rss_kb = n;
        else if (k == "VmPeak" && parse_u64(num, n)) s.vm_peak_kb = n;
        else if (k == "Threads" && parse_u64(num, n)) s.threads = static_cast<int>(n);
        else if (k == "Cpus_allowed_list") s.cpus_allowed_list = v;
    }
    return s;
}

// ---------------------------------------------------------------------------- topology

std::vector<int> CpuTopology::cpus_of_class(CoreClass c) const {
    std::vector<int> out;
    for (const auto& l : cpus)
        if (l.online && l.klass == c) out.push_back(l.id);
    return out;
}

int CpuTopology::physical_core_count() const {
    std::set<std::pair<int, int>> cores;
    for (const auto& l : cpus)
        if (l.online) cores.insert({l.package, l.core});
    return static_cast<int>(cores.size());
}

CpuTopology read_cpu_topology(const IFileSystem& fs) {
    CpuTopology t;
    const std::string root = "/sys/devices/system/cpu";
    std::vector<int> ids;
    std::string present = fs.read_line(root + "/present");
    if (!parse_cpulist(present, ids) || ids.empty()) {
        parse_cpulist(fs.read_line(root + "/online"), ids);
    }
    std::vector<int> online;
    if (!parse_cpulist(fs.read_line(root + "/online"), online) || online.empty()) online = ids;
    t.online_list = online;
    std::set<int> online_set(online.begin(), online.end());

    for (int id : ids) {
        LogicalCpu l;
        l.id = id;
        l.online = online_set.count(id) != 0;
        uint64_t n;
        if (parse_u64(fs.read_line(sys_cpu(id, "topology/physical_package_id")), n)) l.package = static_cast<int>(n);
        if (parse_u64(fs.read_line(sys_cpu(id, "topology/core_id")), n)) l.core = static_cast<int>(n);
        else l.core = id;
        std::string sib = fs.read_line(sys_cpu(id, "topology/thread_siblings_list"));
        if (sib.empty()) sib = fs.read_line(sys_cpu(id, "topology/core_cpus_list"));
        if (!parse_cpulist(sib, l.siblings) || l.siblings.empty()) l.siblings = {id};
        int64_t khz;
        if (parse_i64(fs.read_line(sys_cpu(id, "cpufreq/cpuinfo_max_freq")), khz)) l.max_khz = khz;
        if (parse_u64(fs.read_line(sys_cpu(id, "cpu_capacity")), n)) l.capacity = static_cast<int>(n);
        l.governor = fs.read_line(sys_cpu(id, "cpufreq/scaling_governor"));
        t.cpus.push_back(std::move(l));
    }
    if (!t.cpus.empty()) {
        t.governor = t.cpus.front().governor;
        t.epp = fs.read_line(sys_cpu(t.cpus.front().id, "cpufreq/energy_performance_preference"));
        t.scaling_driver = fs.read_line(sys_cpu(t.cpus.front().id, "cpufreq/scaling_driver"));
    }

    // 1) Intel hybrid exposes the PMU CPU sets.
    std::vector<int> core_set, atom_set, lp_set;
    bool have_core = parse_cpulist(fs.read_line("/sys/devices/cpu_core/cpus"), core_set) && !core_set.empty();
    bool have_atom = parse_cpulist(fs.read_line("/sys/devices/cpu_atom/cpus"), atom_set) && !atom_set.empty();
    parse_cpulist(fs.read_line("/sys/devices/cpu_lowpower/cpus"), lp_set);
    if (have_core && (have_atom || !lp_set.empty())) {
        std::set<int> eff(atom_set.begin(), atom_set.end());
        eff.insert(lp_set.begin(), lp_set.end());
        for (auto& l : t.cpus) l.klass = eff.count(l.id) ? CoreClass::Efficiency : CoreClass::Performance;
        t.hybrid = true;
        t.hybrid_source = "intel-sysfs";
        return t;
    }
    // 2) arm: cpu_capacity tiers.
    std::set<int> caps;
    for (const auto& l : t.cpus)
        if (l.capacity > 0 && l.online) caps.insert(l.capacity);
    if (caps.size() >= 2) {
        const int lo = *caps.begin(), hi = *caps.rbegin();
        for (auto& l : t.cpus) {
            if (l.capacity <= 0) continue;
            if (l.capacity == lo) l.klass = CoreClass::Efficiency;
            else if (l.capacity == hi && caps.size() >= 3) l.klass = CoreClass::Prime;
            else l.klass = CoreClass::Performance;
        }
        t.hybrid = true;
        t.hybrid_source = "arm-capacity";
        return t;
    }
    // 3) Fallback: a clear gap in maximum frequency. AMD boost differences between CCDs stay below this.
    int64_t fmin = INT64_MAX, fmax = 0;
    for (const auto& l : t.cpus)
        if (l.online && l.max_khz > 0) {
            fmin = std::min(fmin, l.max_khz);
            fmax = std::max(fmax, l.max_khz);
        }
    if (fmax > 0 && static_cast<double>(fmin) / static_cast<double>(fmax) < 0.75) {
        const int64_t mid = (fmin + fmax) / 2;
        for (auto& l : t.cpus)
            if (l.max_khz > 0) l.klass = l.max_khz < mid ? CoreClass::Efficiency : CoreClass::Performance;
        t.hybrid = true;
        t.hybrid_source = "max-freq";
    }
    return t;
}

AffinityPlan plan_affinity(const CpuTopology& t) {
    AffinityPlan p;
    p.all_allowed = t.online_list;
    if (t.cpus.empty()) return p;

    // Primary thread of every core: the lowest-numbered sibling that is online.
    auto is_primary = [](const LogicalCpu& l) { return l.siblings.empty() || *std::min_element(l.siblings.begin(), l.siblings.end()) == l.id; };
    std::vector<const LogicalCpu*> prim, smt, eff;
    for (const auto& l : t.cpus) {
        if (!l.online) continue;
        if (t.hybrid && l.klass == CoreClass::Efficiency) {
            eff.push_back(&l);
        } else if (is_primary(l)) {
            prim.push_back(&l);
        } else {
            smt.push_back(&l);
        }
    }
    auto faster = [](const LogicalCpu* a, const LogicalCpu* b) {
        if (a->klass != b->klass) return a->klass > b->klass;
        if (a->max_khz != b->max_khz) return a->max_khz > b->max_khz;
        return a->id < b->id;
    };
    std::sort(prim.begin(), prim.end(), faster);
    if (!prim.empty()) p.main_thread = {prim[0]->id};
    if (prim.size() > 1) p.render_thread = {prim[1]->id};
    for (size_t i = 2; i < prim.size(); ++i) p.workers.push_back(prim[i]->id);
    for (const auto* l : smt) p.workers.push_back(l->id);
    for (const auto* l : eff) p.background.push_back(l->id);
    if (p.workers.empty()) p.workers = p.all_allowed;
    p.use_pinning = t.hybrid;
    return p;
}

// ---------------------------------------------------------------------------- power / thermal

int PowerInfo::best_percent() const {
    int best = -1;
    for (const auto& b : batteries) best = std::max(best, b.percent);
    return best;
}

PowerInfo read_power(const IFileSystem& fs) {
    PowerInfo p;
    std::vector<DirEntry> dirs;
    if (!fs.list_dir("/sys/class/power_supply", dirs)) return p;
    std::sort(dirs.begin(), dirs.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    bool discharging = false;
    for (const auto& d : dirs) {
        const std::string base = "/sys/class/power_supply/" + d.name + "/";
        const std::string type = fs.read_line(base + "type");
        if (type == "Battery") {
            if (fs.read_line(base + "scope") == "Device") continue;  // controller/mouse batteries
            if (!fs.read_line(base + "present").empty() && fs.read_line(base + "present") == "0") continue;
            BatteryInfo b;
            b.present = true;
            b.name = d.name;
            std::string st = fs.read_line(base + "status");
            b.charging = st == "Charging";
            b.full = st == "Full";
            discharging = discharging || st == "Discharging";
            uint64_t cap;
            if (parse_u64(fs.read_line(base + "capacity"), cap)) b.percent = static_cast<int>(std::min<uint64_t>(cap, 100));
            // energy (uWh, uW) or charge (uAh, uA) families
            uint64_t now = 0, full = 0, rate = 0;
            bool e = parse_u64(fs.read_line(base + "energy_now"), now) && parse_u64(fs.read_line(base + "energy_full"), full);
            bool c = !e && parse_u64(fs.read_line(base + "charge_now"), now) && parse_u64(fs.read_line(base + "charge_full"), full);
            if (e) parse_u64(fs.read_line(base + "power_now"), rate);
            else if (c) parse_u64(fs.read_line(base + "current_now"), rate);
            if (rate > 0 && (e || c)) {
                if (st == "Discharging") b.seconds_remaining = static_cast<int64_t>(3600.0 * static_cast<double>(now) / static_cast<double>(rate));
                else if (st == "Charging" && full > now) b.seconds_remaining = static_cast<int64_t>(3600.0 * static_cast<double>(full - now) / static_cast<double>(rate));
            }
            p.batteries.push_back(std::move(b));
        } else if (type == "Mains" || type == "USB") {
            if (fs.read_line(base + "online") == "1") p.ac_online = true;
        }
    }
    p.on_battery = discharging && !p.ac_online;
    return p;
}

std::vector<ThermalReading> read_thermal(const IFileSystem& fs) {
    std::vector<ThermalReading> out;
    std::vector<DirEntry> dirs;
    auto milli = [&](const std::string& path, double& c) {
        int64_t v;
        if (!parse_i64(fs.read_line(path), v)) return false;
        c = static_cast<double>(v) / 1000.0;
        return c > -50.0 && c < 200.0;
    };
    if (fs.list_dir("/sys/class/thermal", dirs)) {
        for (const auto& d : dirs) {
            if (!starts_with(d.name, "thermal_zone")) continue;
            ThermalReading r;
            if (!milli("/sys/class/thermal/" + d.name + "/temp", r.celsius)) continue;
            r.source = fs.read_line("/sys/class/thermal/" + d.name + "/type");
            if (r.source.empty()) r.source = d.name;
            out.push_back(std::move(r));
        }
    }
    if (fs.list_dir("/sys/class/hwmon", dirs)) {
        for (const auto& d : dirs) {
            const std::string base = "/sys/class/hwmon/" + d.name + "/";
            std::string name = fs.read_line(base + "name");
            std::vector<DirEntry> files;
            if (!fs.list_dir(base, files)) continue;
            for (const auto& f : files) {
                if (!starts_with(f.name, "temp") || !ends_with(f.name, "_input")) continue;
                ThermalReading r;
                if (!milli(base + f.name, r.celsius)) continue;
                std::string label = fs.read_line(base + f.name.substr(0, f.name.size() - 6) + "_label");
                r.source = name + (label.empty() ? "" : ":" + label);
                out.push_back(std::move(r));
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const ThermalReading& a, const ThermalReading& b) { return a.source < b.source; });
    return out;
}

double max_celsius(const std::vector<ThermalReading>& r) {
    double m = 0;
    for (const auto& t : r) m = std::max(m, t.celsius);
    return m;
}

// ---------------------------------------------------------------------------- machine

MachineInfo read_machine_info(const IFileSystem& fs) {
    MachineInfo m;
#if defined(__x86_64__) || defined(_M_X64)
    m.arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    m.arch = "aarch64";
#else
    m.arch = "unknown";
#endif
    m.kernel_release = fs.read_line("/proc/sys/kernel/osrelease");
    std::string t;
    if (fs.read_file("/etc/os-release", t) || fs.read_file("/usr/lib/os-release", t)) m.os = parse_os_release(t);
    if (fs.read_file("/proc/cpuinfo", t)) m.cpu = parse_cpuinfo(t);
    if (fs.read_file("/proc/meminfo", t)) m.mem = parse_meminfo(t);
    m.topo = read_cpu_topology(fs);
    m.dmi_vendor = fs.read_line("/sys/class/dmi/id/sys_vendor");
    m.dmi_product = fs.read_line("/sys/class/dmi/id/product_name");
    m.dmi_board = fs.read_line("/sys/class/dmi/id/board_name");
    std::string ver;
    fs.read_file("/proc/version", ver, 4096);
    m.is_wsl = contains_ci(ver, "microsoft") || contains_ci(ver, "wsl");
    m.in_container = fs.exists("/.dockerenv") || fs.exists("/run/.containerenv");
    return m;
}

}  // namespace nue::lx
