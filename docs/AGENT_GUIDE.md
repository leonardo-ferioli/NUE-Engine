# Working in the NUE Engine repository

This is the single home of the NUE Engine (runtime, SDK, native C API, legacy ABI, platform backends, texture module, Direct3D 9
compatibility layer). Nothing of the engine lives anywhere else.

## Where things are

- Remote: `https://github.com/leonardo-ferioli/nue-engine` (private). Default branch: `main`.
- Local clone of the main session: `C:\Users\Leonardo\Projects\nue-engine`.
- Layout (repository root):

```
include/nue/            native C API (public headers)          owner: core
include/nuepp/          C++ wrapper                             owner: core
include/nue_legacy/     legacy ABI headers                      owner: core + legacy
src/core/               platform independent code               owner: core
src/platform/sdl3/      shared SDL3 backend                     owner: core
src/platform/windows/   Windows backend                         owner: windows
src/platform/linux/     Linux and SteamOS backend               owner: linux
src/platform/android/   Android backend (+ android/ Gradle)     owner: android
src/platform/ios/       iOS backend (+ ios/ Xcode project)      owner: ios
src/texture/            texture formats, codecs, cache          owner: texture
src/legacy/             legacy ABI layers (win32, d3d9, dsound, dinput)   owner: legacy
tests/  samples/  tools/  cmake/  docs/  third_party/
```

Ownership rule: edit only the folders you own. If you need a change elsewhere, write `docs/REQUESTS_<area>.md` and message the
owner session (resolve names with ListAgents).

## Branches

- `main`: always builds and passes the tests. Only the main session merges into it.
- Area branches, one per session: `core`, `windows`, `linux`, `android`, `ios`, `texture`, `legacy-d3d9`. Short-lived topic branches (git cannot hold `core` and `core/x` together, so use a dash)
  are named `<area>-<topic>`.
- During the migration the first version of each area branch is extracted from the old game repository with
  `git subtree split --prefix=nue_engine`, so the history is kept and the old `nue_engine/` prefix disappears from paths.
- You never push. The main session pushes your branch and merges it when the checks are green.

## Your worktree

Do not work in the main clone. The main session creates one worktree per session:

```
git -C C:\Users\Leonardo\Projects\nue-engine worktree add ..\nue-engine-<area> <area>
```

You will be told the exact path. Commit with explicit `git add <paths>`, never `git add -A`. Do not switch branches in the main
clone. To bring in other areas' work: `git merge origin/main` (or the owner's branch) in your worktree.

## Build and test

- CMake presets (`CMakePresets.json`): `windows-x86`, `windows-x64`, `linux-x64`, `android-arm64`, `ios-arm64`.
- `cmake --preset <name>`, `cmake --build --preset <name>`, `ctest --preset <name>`.
- Tests must run with the null or dummy backends: no window, no sound, no GPU needed. Never open the game.

## CI and releases (GitHub Actions, `.github/workflows/ci.yml`)

- Runs on every push and pull request: Windows x86 and x64, Linux x64 (with SDL dummy drivers), Android arm64 (NDK, build only),
  iOS arm64 (macOS runner, build only), each followed by the host tests where they can run.
- A job is skipped while the files it needs are not there yet.
- The main session reads the result with `gh run list` and `gh run view --log-failed` and reports it to you; fix what fails in your
  worktree.
- Releases: a tag `v<major>.<minor>.<patch>` builds the SDK packages (headers + libraries + CMake package config) as artifacts.
  Version rules: the native ABI follows semantic versioning; the legacy ABI is versioned per API table (see `docs/ABI.md`).
- No secrets, no signing keys and no game data in the repository or in CI.

## Reports

Say clearly what is implemented, compiled, run on a device, and only written. The user reads your final message in Spanish; code and
documents stay in English.
