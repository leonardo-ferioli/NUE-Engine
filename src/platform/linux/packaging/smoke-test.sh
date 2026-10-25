#!/bin/sh
# Headless smoke test of a NUE runtime package or build tree: no window, no sound, no GPU.
#
#   smoke-test.sh [dir-with-binaries]        (default: ./bin next to this script, else the script's directory)
#
# Runs with SDL's dummy drivers, a private HOME and XDG_RUNTIME_DIR, so it also works in a clean container with no desktop libraries.
# Exit status: 0 = everything that exists passed; 1 = a check failed. Missing optional binaries are reported as SKIP.
here="$(cd "$(dirname "$0")" && pwd)"
bin="${1:-}"
if [ -z "$bin" ]; then
    if [ -d "$here/bin" ]; then bin="$here/bin"; else bin="$here"; fi
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export HOME="$tmp/home" XDG_RUNTIME_DIR="$tmp/run" XDG_DATA_HOME="$tmp/home/.local/share" XDG_CONFIG_HOME="$tmp/home/.config" XDG_CACHE_HOME="$tmp/home/.cache"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

fail=0
ok()   { printf 'ok    %s\n' "$1"; }
bad()  { printf 'FAIL  %s\n' "$1"; fail=1; }
skip() { printf 'SKIP  %s\n' "$1"; }

# 1. no missing shared libraries (ldd is absent in some minimal images: skip then)
if command -v ldd >/dev/null 2>&1; then
    for exe in "$bin"/*; do
        [ -f "$exe" ] && [ -x "$exe" ] || continue
        if ldd "$exe" 2>&1 | grep -q 'not found'; then bad "ldd: unresolved libraries in $(basename "$exe")"; ldd "$exe" | grep 'not found'; else ok "ldd $(basename "$exe")"; fi
    done
else
    skip "ldd not installed"
fi

# 2. linux_probe prints the platform report and exits 0
if [ -x "$bin/linux_probe" ]; then
    out="$("$bin/linux_probe" --headless 2>&1)"; rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q 'NUE Linux platform report'; then ok "linux_probe --headless"; else bad "linux_probe --headless (exit $rc)"; printf '%s\n' "$out" | head -20; fi
else
    skip "linux_probe"
fi

# 3. audio sample against the dummy device
if [ -x "$bin/nue_sample_hello_audio" ]; then
    if "$bin/nue_sample_hello_audio" 1 >"$tmp/audio.log" 2>&1; then ok "nue_sample_hello_audio 1"; else bad "nue_sample_hello_audio (see below)"; tail -20 "$tmp/audio.log"; fi
else
    skip "nue_sample_hello_audio"
fi

# 4. window sample against the dummy video driver: it never exits by itself, so a timeout (124) is the success case
if [ -x "$bin/nue_sample_hello_window" ]; then
    if command -v timeout >/dev/null 2>&1; then
        timeout 3 "$bin/nue_sample_hello_window" >"$tmp/window.log" 2>&1; rc=$?
        if [ $rc -eq 0 ] || [ $rc -eq 124 ]; then ok "nue_sample_hello_window (ran 3 s, exit $rc)"; else bad "nue_sample_hello_window (exit $rc)"; tail -20 "$tmp/window.log"; fi
    else
        skip "nue_sample_hello_window (no timeout command)"
    fi
else
    skip "nue_sample_hello_window"
fi

# 5. the engine's own Linux tests, when shipped
for t in nue_linux_tests nue_linux_ops_tests; do
    if [ -x "$bin/$t" ]; then
        if "$bin/$t" >"$tmp/$t.log" 2>&1; then ok "$t"; else bad "$t"; grep -E 'FAIL' "$tmp/$t.log" | head -20; fi
    fi
done

[ $fail -eq 0 ] && echo "smoke test passed" || echo "smoke test FAILED"
exit $fail
