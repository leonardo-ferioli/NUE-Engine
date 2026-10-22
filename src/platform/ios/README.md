# NUE platform layer: ios

Owned by the `nue-ios` session. Implements `nue_platform_ios_get_ops()` (`adapter/platform_ios.mm`) on top of the iOS backend glue.

```
adapter/    platform_ios.mm        the nue_platform_ops table (thin)
apple/      *.mm                   Objective-C++ glue behind nue_ios.h (lifecycle, power, storage, audio, input, display/Metal, system, diagnostics)
logic/      *.cpp                  pure C++17, host-tested (lifecycle machine, policies, mapping to nue_ types, paths, import planning)
include/    nue_ext_ios.h          extension "nue.ios.v1" (Metal caps / DXT policy, CAMetalLayer setup, declared Game Mode flags)
CMakeLists.txt                     included by nue_engine/CMakeLists.txt for the iOS target
```

Documentation: `nue_engine/docs/PLATFORM_IOS.md` (English, with the verification status) and `PLATFORM_IOS.es.md` (build steps in Spanish).
Host tests: `cmake -S nue_engine/ios -B build_ios_host -G Ninja && cmake --build build_ios_host && ctest --test-dir build_ios_host`.
Nothing under `apple/` or `adapter/` has been compiled yet (written on Windows).
