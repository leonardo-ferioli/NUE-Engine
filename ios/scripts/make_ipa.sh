#!/usr/bin/env bash
# Wraps a built .app into an .ipa (Payload/<app>.app, zipped) for sideloading tools (AltStore, Sideloadly, TrollStore, Xcode "Devices" window).
#   usage: scripts/make_ipa.sh path/to/nue_ios_sample.app [output.ipa]
# The IPA is NOT signed by this script: AltStore/Sideloadly re-sign it with the user's Apple ID. For an Xcode-signed IPA use `xcodebuild archive`
# + `xcodebuild -exportArchive` (see nue_engine/docs/PLATFORM_IOS.md). Never run by the author (written on Windows).
set -euo pipefail
APP="${1:?path to .app}"
IPA="${2:-$(dirname "$APP")/$(basename "$APP" .app).ipa}"
case "$IPA" in /*) ;; *) IPA="$PWD/$IPA" ;; esac   # absolute: the zip runs from a temp dir
[ -d "$APP" ] || { echo "not a directory: $APP" >&2; exit 2; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/Payload"
cp -R "$APP" "$WORK/Payload/"
(cd "$WORK" && /usr/bin/zip -qry "$IPA" Payload)
echo "Created $IPA ($(du -h "$IPA" | cut -f1))"
