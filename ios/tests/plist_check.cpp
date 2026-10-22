// Checks that the sample Info.plist carries the keys the iOS backend relies on (text-level check, no plist parser needed).
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: plist_check <Info.plist>\n"); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { std::printf("cannot open %s\n", argv[1]); return 2; }
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    struct Req { const char* key; const char* expect; };   // expect: substring that must follow the key's tag ("<true/>"), or nullptr
    const Req required[] = {
        {"UIFileSharingEnabled", "<true/>"},
        {"LSSupportsOpeningDocumentsInPlace", "<true/>"},
        {"GCSupportsGameMode", "<true/>"},
        {"LSApplicationCategoryType", "public.app-category"},
        {"UIRequiredDeviceCapabilities", "arm64"},
        {"UILaunchScreen", nullptr},
        {"MinimumOSVersion", "16.0"},
        {"UISupportedInterfaceOrientations", "UIInterfaceOrientationLandscape"},
    };
    int bad = 0;
    for (const auto& r : required) {
        const std::string tag = std::string("<key>") + r.key + "</key>";
        auto at = text.find(tag);
        if (at == std::string::npos) { std::printf("missing key %s\n", r.key); ++bad; continue; }
        if (r.expect) {
            auto next_key = text.find("<key>", at + tag.size());
            auto val = text.find(r.expect, at + tag.size());
            if (val == std::string::npos || (next_key != std::string::npos && val > next_key)) { std::printf("key %s lacks %s\n", r.key, r.expect); ++bad; }
        }
    }
    // Must NOT be present: background audio would make the OS keep the process alive and the policy is "suspend".
    if (text.find("<key>UIBackgroundModes</key>") != std::string::npos) { std::printf("UIBackgroundModes must not be set\n"); ++bad; }
    std::printf("plist_check: %d problem(s)\n", bad);
    return bad ? 1 : 0;
}
