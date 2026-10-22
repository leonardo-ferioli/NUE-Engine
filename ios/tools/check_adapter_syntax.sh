#!/usr/bin/env bash
# Host-side syntax check of src/platform/ios/adapter/platform_ios.mm (Objective-C++ with ARC + blocks) against the REAL NUE headers and the
# hand-written stand-ins in darwin_stub.h for the few Foundation/UIKit/Darwin declarations the adapter uses. Needs clang on any OS.
# It checks C++/NUE type consistency only; it does NOT prove compatibility with the real Apple SDK (the glue in apple/ is not covered).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE="$(cd "$HERE/../.." && pwd)"
STUBS="$(mktemp -d)"
trap 'rm -rf "$STUBS"' EXIT
cp "$HERE/darwin_stub.h" "$STUBS/darwin_stub.h"
mkdir -p "$STUBS/Foundation" "$STUBS/UIKit" "$STUBS/os" "$STUBS/sys"
for h in Foundation/Foundation.h UIKit/UIKit.h os/log.h sys/ucontext.h; do printf '#include "../darwin_stub.h"\n' > "$STUBS/$h"; done
for h in execinfo.h pthread.h unistd.h fcntl.h signal.h; do printf '#include "darwin_stub.h"\n' > "$STUBS/$h"; done
# NUE_CORE_ROOT: directory holding include/ and src/core/internal (default: this repository; set it to check against another core revision).
CORE="${NUE_CORE_ROOT:-$ENGINE}"
cd "$ENGINE"
clang++ -x objective-c++ -std=c++17 -fobjc-arc -fblocks -fobjc-runtime=macosx -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
  -Wno-nullability-completeness -Wno-nullability-extension -Wno-deprecated-declarations \
  -I"$STUBS" -I"$CORE/include" -I"$CORE/src/core" -Isrc/platform/ios -Isrc/platform/ios/apple -Isrc/platform/ios/logic -Isrc/platform/ios/include \
  src/platform/ios/adapter/platform_ios.mm
echo "adapter syntax check: OK"
