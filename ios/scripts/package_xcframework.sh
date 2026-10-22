#!/usr/bin/env bash
# Packages the iOS backend (logic + Objective-C++ glue) as NueIos.xcframework (device arm64 + simulator arm64) with the nue_ios.h header.
#   usage: scripts/package_xcframework.sh [output-dir]
# Needs macOS, Xcode and CMake >= 3.21. Never run by the author (written on Windows): see nue_engine/docs/PLATFORM_IOS.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_DIR="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$IOS_DIR/../.." && pwd)"
OUT="${1:-$REPO/build_ios_xcframework}"
rm -rf "$OUT"; mkdir -p "$OUT"

build_slice() {   # $1 = device|simulator
  local preset="ios-$1" cfg=Release
  [ "$1" = simulator ] && preset=ios-simulator
  (cd "$IOS_DIR" && cmake --preset "$preset" -DNUE_IOS_TEAM_ID="" >/dev/null)
  local bdir="$REPO/build_ios_$([ "$1" = device ] && echo device || echo sim)"
  cmake --build "$bdir" --config "$cfg" --target nue_ios_glue nue_ios_logic >/dev/null
  local sdkdir="$cfg-$([ "$1" = device ] && echo iphoneos || echo iphonesimulator)"
  local glue logic
  glue="$(find "$bdir" -path "*$sdkdir*" -name libnue_ios_glue.a | head -n1)"
  logic="$(find "$bdir" -path "*$sdkdir*" -name libnue_ios_logic.a | head -n1)"
  [ -n "$glue" ] && [ -n "$logic" ] || { echo "static libraries not found for $1" >&2; exit 1; }
  mkdir -p "$OUT/$1"
  libtool -static -o "$OUT/$1/libNueIos.a" "$glue" "$logic"
}

build_slice device
build_slice simulator

mkdir -p "$OUT/headers"
cp "$IOS_DIR/../src/platform/ios/nue_ios.h" "$OUT/headers/"
cat > "$OUT/headers/module.modulemap" <<'EOF'
module NueIos {
    header "nue_ios.h"
    export *
}
EOF
xcodebuild -create-xcframework \
  -library "$OUT/device/libNueIos.a" -headers "$OUT/headers" \
  -library "$OUT/simulator/libNueIos.a" -headers "$OUT/headers" \
  -output "$OUT/NueIos.xcframework"
echo "Created $OUT/NueIos.xcframework"
