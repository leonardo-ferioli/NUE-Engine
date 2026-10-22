// Writes one of the built-in machine fixtures (steam_deck, intel_hybrid, hybrid_gpu_laptop,
// arm_big_little, raspberry_pi3) as a directory tree, so `linux_probe --root DIR` can run on any host.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../tests/fixtures.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: make_fixture <steam_deck|intel_hybrid|hybrid_gpu_laptop|arm_big_little|raspberry_pi3|steamos_deck|bazzite_desktop> <outdir>\n");
        return 2;
    }
    nue::lx::MemFs fs;
    const std::string n = argv[1];
    if (n == "steam_deck") fixtures::steam_deck(fs);
    else if (n == "intel_hybrid") fixtures::intel_hybrid(fs);
    else if (n == "hybrid_gpu_laptop") fixtures::hybrid_gpu_laptop(fs);
    else if (n == "arm_big_little") fixtures::arm_big_little(fs);
    else if (n == "raspberry_pi3") fixtures::raspberry_pi3(fs);
    else if (n == "steamos_deck") fixtures::steamos_deck(fs);
    else if (n == "bazzite_desktop") fixtures::bazzite_desktop(fs);
    else { std::printf("unknown fixture %s\n", argv[1]); return 2; }
    namespace stdfs = std::filesystem;
    int count = 0;
    fs.for_each_file([&](const std::string& path, const std::string& data) {
        stdfs::path p = stdfs::path(argv[2]) / stdfs::path(path.substr(1));
        stdfs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << data;
        ++count;
    });
    std::printf("wrote %d files to %s\n", count, argv[2]);
    return 0;
}
