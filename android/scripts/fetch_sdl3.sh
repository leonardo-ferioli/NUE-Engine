#!/bin/sh
# Fetches the SDL3 source tree used by the Android build (Java classes + libSDL3.so) into nue_engine/android/.deps/SDL3.
set -e
TAG="${1:-release-3.4.18}"
DIR="$(cd "$(dirname "$0")/.." && pwd)/.deps/SDL3"
if [ -f "$DIR/CMakeLists.txt" ]; then echo "SDL3 already present at $DIR"; exit 0; fi
mkdir -p "$(dirname "$DIR")"
git clone --depth 1 --branch "$TAG" https://github.com/libsdl-org/SDL.git "$DIR"
echo "SDL3 $TAG ready in $DIR"
