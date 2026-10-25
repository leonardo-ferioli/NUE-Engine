# Contributing

Thanks for helping. This page is the short version; [docs/AGENT_GUIDE.md](docs/AGENT_GUIDE.md) has the repository layout, ownership
and branch model.

## Build and test

```
git submodule update --init --depth 1 third_party/SDL
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
```

Presets: `windows-x86`, `windows-x64`, `linux-x64`, `android-arm64`, `ios-arm64`. Tests use the null and dummy backends, so no
window, sound card or GPU is needed. The Direct3D 9 layer needs `-DNUE_WITH_LEGACY_D3D9=ON` and `third_party/bgfx.cmake`.

## Rules of the road

- Keep each change inside one area: core, a platform layer, textures or a legacy layer. Ask the owner of the area for changes outside it.
- Write tests with the change. Prefer the null backend and deterministic checks.
- Say what you built and what you ran. Code that compiles but was never run is reported as exactly that.
- Memory limits are run-time parameters. Do not hard-code them.
- No game data, keys, signing material or personal paths in the repository.

## Adding a legacy layer

Start from [docs/LEGACY_LAYERS.md](docs/LEGACY_LAYERS.md): the shape of the layer, a checklist, and what the Direct3D 9 layer
learned. Then add the layer under `src/legacy/<api>/` with its own tests and a coverage table.

## Reporting a problem

Open an issue with the platform, the preset you built with and the output of the failing test. For a rendering difference, include
the trace that reproduces it.
