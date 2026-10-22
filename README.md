<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/nue-logo-dark.png">
    <img alt="NUE Engine" src="assets/nue-logo-light.png" width="420">
  </picture>
</p>

<p align="center">
  <b>A general-purpose runtime, SDK and C API for games.<br>
  Window, input, audio, storage, power and game mode, graphics and texture compression behind one interface.<br>
  Windows, Linux (including SteamOS), Android and iOS.</b>
</p>

## What is NUE Engine?

NUE Engine is a cross-platform runtime with a stable C API (and a C++ wrapper) that controls everything a game needs from the
operating system. It is built to run many games, starting with the NUE port of *Need for Speed: Underground 2*.

## Modules

- **Core:** initialisation, logging, errors, configuration, threads, high-resolution clock, memory budgets.
- **Application lifecycle:** events for pause, resume, low memory, focus and display changes.
- **Window and display:** windowed, borderless and full-screen modes, DPI, safe areas, refresh rate, multi-monitor.
- **Input:** keyboard, mouse, gamepads, touch and sensors, with last-used-device detection.
- **Audio:** device and stream API with a mixer.
- **Storage:** virtual file system for game data and user data, case-insensitive paths, per-platform data import.
- **Power and game mode:** game mode on every platform, thermal and battery state, keep-awake, performance hints.
- **Graphics:** bgfx (Vulkan, Direct3D 11/12, Metal, OpenGL ES) with a Direct3D 9 compatibility layer.
- **Textures:** every common compressed format (BC/DXT, ETC, ASTC, PVRTC), capability detection, transcoding and a cache.

## Platforms

| Platform | State |
|---|---|
| Windows | In development |
| Linux and SteamOS | In development |
| Android | In development |
| iOS | In development |

Built on [SDL3](https://github.com/libsdl-org/SDL) with platform-specific code where it is needed.
