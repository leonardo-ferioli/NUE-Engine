# Packaging the NUE runtime on Linux

Scripts to produce the SDK, the sample runtime, an AppImage and a Flatpak, and to prove the result runs in a clean container. **Status: written, none of it has been run**
(by project rule Linux builds and tests run only in GitHub Actions). The jobs `linux-sanitizers`, `linux-musl`, `linux-arch`, `linux-packaging` and `linux-flatpak` in `.github/workflows/ci.yml` run all of it on GitHub (they are the only place Linux packaging and the extra Linux tests run: the project rule is local builds for Windows only); until a run is green, treat every step as unverified.

| Script | What it makes | Needs |
|---|---|---|
| `build-redist.sh [--preset linux-x64] [--out dist] [--shared] [--static-cxx]` | `nue-sdk-<ver>-linux-<arch>.tar.gz` (headers, libnue, CMake package) and `nue-runtime-<ver>-linux-<arch>.tar.gz` (samples, `linux_probe`, `smoke-test.sh`, `RUNTIME_DEPENDENCIES.txt`) | cmake, ninja, C++17 compiler, SDL3 build dependencies |
| `build-appimage.sh --runtime-tar FILE [--tool appimagetool]` | `NUE-Sample-<arch>.AppImage` (`--probe`, `--audio`, `--smoke`, default: the window sample) | appimagetool |
| `build-flatpak.sh [--install]` | `org.nue.Sample.flatpak` from `flatpak/org.nue.Sample.yml` | flatpak, flatpak-builder, Flathub runtimes 24.08 |
| `smoke-test.sh [bin-dir]` | headless checks: `ldd` finds every library, `linux_probe`, the audio sample on the dummy device, the window sample on the dummy video driver, the engine's own tests when present | `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` are set by the script |
| `clean-container-test.sh <artifact> [image]` | runs an artifact in `debian:stable-slim` (default), `ubuntu:22.04`, `archlinux`, `fedora`... through docker or podman | docker or podman |

## Dependencies (details in `RUNTIME_DEPENDENCIES.txt`)

* Always: glibc (built on the oldest target: 2.31 covers Ubuntu 20.04+, Debian 11+, Steam Runtime soldier/sniper, SteamOS 3), plus `libstdc++` unless `--static-cxx`.
* Never bundled, `dlopen`ed from the host so they match its GPU and compositor: Vulkan loader, EGL/GL, Wayland and X11 client libraries, **PipeWire** (preferred), PulseAudio, ALSA, udev, libdbus, **GameMode**.
* A package therefore runs with none of them installed (dummy drivers, no sound, no GPU), and gains each feature when the library exists. The headless smoke test is that claim made executable.

## Which package for which target

| Target | Use |
|---|---|
| Any distribution, developers | the SDK tarball (static `libnue`, CMake package `find_package(nue)`) |
| Players, one file | the AppImage (needs FUSE 2, or run with `--appimage-extract-and-run` / `APPIMAGE_EXTRACT_AND_RUN=1`) |
| Sandboxed desktops, Steam Deck desktop mode | the Flatpak (permissions in the manifest; add `--filesystem` for game data) |
| SteamOS / Steam Deck | the tarball with `dist/steamos/nue-launch.sh` added as a Non-Steam game with Steam Linux Runtime 3.0 (sniper); see `docs/PLATFORM_LINUX.md` |
| musl systems (Alpine) | build from source (`cmake -S src/platform/linux`); a glibc package does not run there |

## Known limits

* The root `CMakeLists.txt` installs only `nue`, `nue_entry` and the headers; the scripts copy the sample binaries from the build tree. A `install(TARGETS ...)` for the samples
  (a core change, see `docs/REQUESTS_linux.md`) would simplify `build-redist.sh` and the Flatpak manifest.
* `appimagetool` is downloaded by the CI template from the `continuous` release: pin a version and checksum before depending on it.
* The Flatpak manifest builds from the local checkout (`type: dir`), so the SDL3 submodule must be present; Flathub itself would need a pinned git source instead.
* The AppImage and the tarball built on a recent distribution will not run on an older glibc. Build on the oldest one, or with the Zig toolchain file (`-DNUE_ZIG_TARGET=x86_64-linux-gnu.2.31`).
