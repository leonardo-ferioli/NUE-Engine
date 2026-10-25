#!/usr/bin/env bash
# Builds the NUE sample runtime as a Flatpak bundle.
#
#   packaging/build-flatpak.sh [--out dist] [--install]
#
# Needs: flatpak, flatpak-builder, and the org.freedesktop.Platform / Sdk 24.08 runtimes (installed from Flathub with --install-deps-from=flathub).
# The SDL3 submodule must be checked out (git submodule update --init --depth 1 third_party/SDL): the build has no network access.
# Status: written, not run.
set -euo pipefail

out=dist
install=0
while [ $# -gt 0 ]; do
    case "$1" in
        --out) out="$2"; shift 2 ;;
        --install) install=1; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../../.." && pwd)"
[ -f "$repo/third_party/SDL/CMakeLists.txt" ] || { echo "third_party/SDL is empty: run git submodule update --init --depth 1 third_party/SDL" >&2; exit 1; }
command -v flatpak-builder >/dev/null || { echo "flatpak-builder not found" >&2; exit 1; }

mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
flatpak-builder --force-clean --user --disable-rofiles-fuse --install-deps-from=flathub \
    --repo="$work/repo" "$work/build" "$here/flatpak/org.nue.Sample.yml"
flatpak build-bundle "$work/repo" "$out/org.nue.Sample.flatpak" org.nue.Sample
echo "wrote $out/org.nue.Sample.flatpak"
if [ "$install" = 1 ]; then
    flatpak install --user -y "$out/org.nue.Sample.flatpak"
    echo "run:  flatpak run org.nue.Sample          probe: flatpak run --command=linux_probe org.nue.Sample"
    echo "headless smoke test: flatpak run --command=smoke-test.sh --env=SDL_VIDEODRIVER=dummy --env=SDL_AUDIODRIVER=dummy org.nue.Sample /app/bin"
fi
