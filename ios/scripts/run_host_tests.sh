#!/usr/bin/env bash
# Runs the iOS backend's host tests (pure logic + Info.plist check). Works on macOS, Linux and Git Bash on Windows (with clang or gcc).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${1:-${TMPDIR:-/tmp}/nue_ios_host}"
cmake -S "$HERE/.." -B "$BUILD" -G Ninja
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure
