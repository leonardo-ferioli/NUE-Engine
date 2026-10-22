<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/nue-logo-dark.png">
    <img alt="NUE Engine" src="assets/nue-logo-light.png" width="420">
  </picture>
</p>

<p align="center">
  <b>The runtime for static recompilations.<br>
  Bring the translated game code. NUE provides everything around it, on Windows, Linux (including SteamOS), Android and iOS.</b>
</p>

## What is NUE Engine?

A static recompilation turns a game's machine code into source code you can build for any target. The translated code is only part of
the work: the original program still expects its operating system, its graphics, sound and input APIs, its file layout and its
32-bit memory model. Every recompilation project ends up rewriting the same surface from scratch.

NUE Engine is that surface, written once. It is a cross-platform runtime with a stable C API, a C++ wrapper and a compatibility ABI
for Windows-era games. A recompilation links against it and only has to supply what is specific to the game.

## How it fits together

```
  recompiled game code (generated C/C++)          native tools and ports
              |                                            |
   +----------v------------------+                         |
   |  Legacy ABI  (nue_legacy)   |                         |
   |  Win32 subset, Direct3D 9,  |                         |
   |  DirectSound, DirectInput,  |                         |
   |  guest pointers and handles |                         |
   +----------+------------------+                         |
              |                                            |
   +----------v--------------------------------------------v--+
   |  Native ABI  (nue)                                       |
   |  core, lifecycle, window, input, audio, storage,         |
   |  power and game mode, graphics, textures                 |
   +----------+-----------------------------------------------+
              |
   +----------v-----------------------------------------------+
   |  Platform layers: Windows, Linux and SteamOS, Android,   |
   |  iOS        (SDL3 underneath, native code where needed)  |
   +----------------------------------------------------------+
```

The legacy ABI sits on top of the native one and never the other way round, so a native program links no legacy code.

## What you write and what NUE provides

| Part of a recompilation | Who provides it |
|---|---|
| Translating the original code | Your recompilation project |
| Game-specific patches and data formats | Your recompilation project |
| Window, display modes, DPI, safe areas | NUE |
| Keyboard, mouse, gamepads, touch, last-used-device detection | NUE |
| Audio devices, streams and a mixer | NUE |
| File access with case-insensitive paths, user data, data import | NUE |
| Game mode, thermal and battery state, keep-awake, performance hints | NUE |
| Application lifecycle: pause, resume, low memory, focus changes | NUE |
| Win32 subset, handle tables and 32-bit guest pointers | NUE (legacy ABI) |
| Direct3D 9 on top of bgfx (Vulkan, Direct3D 11/12, Metal, OpenGL ES) | NUE (legacy ABI) |
| DirectSound and DirectInput on top of the native audio and input modules | NUE (legacy ABI) |
| BC/DXT, ETC, ASTC and PVRTC detection, transcoding and caching | NUE |
| Memory budgets, logging, crash reports, configuration | NUE |

## Modules

| Module | What it does | State |
|---|---|---|
| Core | Initialisation, logging, errors, configuration, threads, clocks, memory budgets | Tested on Windows x64 and x86 |
| SDL3 backend | Window, input, audio and timing through SDL3 | Tested with SDL's dummy drivers |
| Windows layer | Game mode, power and scheduling, CPU topology, WASAPI audio, storage rules, diagnostics | Tested on Windows |
| Linux layer | X11 and Wayland policy, PipeWire, GameMode, XDG paths, SteamOS | In development |
| Android layer | Lifecycle, memory, thermal, performance hints, storage access framework | In development |
| iOS layer | Lifecycle, memory, Game Mode, Documents, controllers | Builds on macOS, not yet run on a device |
| Textures | Format tables, codecs, GPU capability detection, transcoding, cache | Tested on Windows |
| Legacy Direct3D 9 | Direct3D 9 translated to bgfx, with a shader translator | Builds and matches the system runtime in state tests |
| Legacy Win32, DirectSound, DirectInput | Compatibility layers for recompiled games | Planned |

The per-feature breakdown is kept in [docs/STATUS.md](docs/STATUS.md).

## Platforms

| Platform | State |
|---|---|
| Windows (x86 and x64) | In development |
| Linux and SteamOS | In development |
| Android (arm64) | In development |
| iOS (arm64) | In development |

## Reference port

The first game running on NUE is the recompilation of *Need for Speed: Underground 2*, which drives the design of the legacy ABI.

## Build

```
git submodule update --init --depth 1 third_party/SDL
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
```

Presets: `windows-x86`, `windows-x64`, `linux-x64`, `android-arm64`, `ios-arm64`. The Direct3D 9 layer is built with
`-DNUE_WITH_LEGACY_D3D9=ON` after initialising `third_party/bgfx.cmake`.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Native API](docs/API.md)
- [The two ABIs](docs/ABI.md)
- [Porting a platform](docs/PORTING.md)
- [Graphics and the Direct3D 9 seam](docs/GFX_D3D9_SEAM.md)
- [Status](docs/STATUS.md)
- [Working in the repository](docs/AGENT_GUIDE.md)

## Built on

[SDL3](https://github.com/libsdl-org/SDL), [bgfx](https://github.com/bkaradzic/bgfx) and SPIRV-Cross,
[astc-encoder](https://github.com/ARM-software/astc-encoder), [etcpak](https://github.com/wolfpld/etcpak),
[bcdec](https://github.com/iOrange/bcdec) and [stb_image](https://github.com/nothings/stb).
