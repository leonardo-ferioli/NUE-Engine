#!/usr/bin/env bash
# Builds the NUE iOS sample for the device or the simulator with CMake + Xcode.
#   usage: scripts/build_ios.sh [device|simulator] [Debug|Release]
# Needs macOS, Xcode and CMake >= 3.21. Never run by the author (written on Windows): see nue_engine/docs/PLATFORM_IOS.md.
set -euo pipefail

PLATFORM="${1:-device}"
CONFIG="${2:-Release}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_DIR="$(cd "$HERE/.." && pwd)"

case "$PLATFORM" in
  device)    PRESET=ios-device ;;
  simulator) PRESET=ios-simulator ;;
  *) echo "platform must be device or simulator" >&2; exit 2 ;;
esac

cd "$IOS_DIR"
cmake --preset "$PRESET" ${NUE_IOS_TEAM_ID:+-DNUE_IOS_TEAM_ID="$NUE_IOS_TEAM_ID"} ${NUE_IOS_BUNDLE_ID:+-DNUE_IOS_BUNDLE_ID="$NUE_IOS_BUNDLE_ID"}
BUILD_DIR="$IOS_DIR/../../build_ios_$([ "$PLATFORM" = device ] && echo device || echo sim)"   # same as binaryDir in CMakePresets.json
cmake --build "$BUILD_DIR" --config "$CONFIG" --target nue_ios_sample -- -allowProvisioningUpdates 2>&1 | tail -n 40
echo "Built: $(find "$BUILD_DIR" -name 'nue_ios_sample.app' -maxdepth 3 | head -n1)"
