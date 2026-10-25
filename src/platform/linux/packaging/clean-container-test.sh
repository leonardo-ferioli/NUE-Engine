#!/usr/bin/env bash
# Runs a packaged NUE runtime in a CLEAN container (no desktop, no sound server, no Vulkan) and executes the headless smoke test.
#
#   packaging/clean-container-test.sh <runtime.tar.gz | NUE-Sample-<arch>.AppImage> [image]
#
# image defaults to debian:stable-slim. Good matrix: debian:stable-slim, ubuntu:22.04, archlinux:latest (SteamOS stand-in), fedora:latest.
# (alpine uses musl: glibc builds do not run there; use a musl build, see README.md.)
# Uses docker or podman, whichever exists. The artifact is mounted read-only; nothing is installed in the container except what the test needs
# (libstdc++ is added when the image lacks it and the package was not built with --static-cxx).
set -euo pipefail

artifact="${1:?usage: clean-container-test.sh <artifact> [image]}"
image="${2:-debian:stable-slim}"
engine="$(command -v docker || command -v podman || true)"
[ -n "$engine" ] || { echo "neither docker nor podman is installed" >&2; exit 1; }
artifact="$(readlink -f "$artifact")"
name="$(basename "$artifact")"

case "$name" in
    *.AppImage)
        cmd='cp /pkg/'"$name"' /tmp/app && chmod +x /tmp/app && APPIMAGE_EXTRACT_AND_RUN=1 /tmp/app --smoke' ;;
    *.tar.gz|*.tgz)
        cmd='mkdir /tmp/rt && tar -C /tmp/rt -xzf /pkg/'"$name"' && /tmp/rt/smoke-test.sh /tmp/rt/bin' ;;
    *) echo "unsupported artifact: $name" >&2; exit 2 ;;
esac

# Make sure the C++ runtime and basic tools exist without pulling any desktop library.
prep='if ! ldconfig -p 2>/dev/null | grep -q libstdc++.so.6; then
  if command -v apt-get >/dev/null; then apt-get update -qq >/dev/null && apt-get install -y -qq libstdc++6 >/dev/null;
  elif command -v pacman >/dev/null; then pacman -Sy --noconfirm gcc-libs >/dev/null;
  elif command -v dnf >/dev/null; then dnf install -y -q libstdc++ >/dev/null; fi
fi'

echo "== $name in $image ($(basename "$engine"))"
"$engine" run --rm -v "$(dirname "$artifact"):/pkg:ro" "$image" sh -c "$prep; $cmd"
