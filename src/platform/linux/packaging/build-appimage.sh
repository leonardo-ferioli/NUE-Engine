#!/usr/bin/env bash
# Builds the NUE sample runtime as an AppImage.
#
#   packaging/build-appimage.sh [--runtime-dir DIR | --runtime-tar FILE] [--out dist] [--tool PATH-to-appimagetool]
#
# Input: the runtime redistributable made by build-redist.sh (tarball or extracted directory).
# Needs: appimagetool (https://github.com/AppImage/appimagetool). Not bundled: no Vulkan / GL / audio / X11 / Wayland / D-Bus / udev library,
# because they must match the host's GPU, compositor and sound server (see RUNTIME_DEPENDENCIES.txt; the engine dlopens them).
# Build the redistributable with --static-cxx so the AppImage needs nothing but glibc.
set -euo pipefail

rtdir=""
rttar=""
out=dist
tool="${APPIMAGETOOL:-appimagetool}"
while [ $# -gt 0 ]; do
    case "$1" in
        --runtime-dir) rtdir="$2"; shift 2 ;;
        --runtime-tar) rttar="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --tool) tool="$2"; shift 2 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
here="$(cd "$(dirname "$0")" && pwd)"
command -v "$tool" >/dev/null 2>&1 || [ -x "$tool" ] || { echo "appimagetool not found (set --tool or APPIMAGETOOL)" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
if [ -n "$rttar" ]; then
    mkdir -p "$work/rt"
    tar -C "$work/rt" -xzf "$rttar"
    rtdir="$work/rt"
fi
[ -d "$rtdir/bin" ] || { echo "need --runtime-dir with a bin/ folder or --runtime-tar" >&2; exit 2; }

app="$work/NUE-Sample.AppDir"
mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps" "$app/usr/share/doc/nue"
cp "$rtdir"/bin/* "$app/usr/bin/"
cp "$rtdir/smoke-test.sh" "$app/usr/bin/" 2>/dev/null || cp "$here/smoke-test.sh" "$app/usr/bin/"
cp -r "$rtdir"/share/doc/nue/* "$app/usr/share/doc/nue/" 2>/dev/null || true
install -m 755 "$here/AppRun" "$app/AppRun"
install -m 644 "$here/nue-sample.desktop" "$app/nue-sample.desktop"
install -m 644 "$here/nue-sample.desktop" "$app/usr/share/applications/nue-sample.desktop"
install -m 644 "$here/nue-sample.png" "$app/nue-sample.png"
install -m 644 "$here/nue-sample.png" "$app/usr/share/icons/hicolor/256x256/apps/nue-sample.png"
ln -s nue-sample.png "$app/.DirIcon"

mkdir -p "$out"
arch="$(uname -m)"
# --appimage-extract-and-run lets appimagetool itself run where FUSE is missing (containers, CI)
ARCH="$arch" "$tool" "$app" "$out/NUE-Sample-$arch.AppImage"
echo "wrote $out/NUE-Sample-$arch.AppImage"
