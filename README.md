<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/nue-logo-dark.png">
    <img alt="NUE Engine" src="assets/nue-logo-light.png" width="420">
  </picture>
</p>

<h3 align="center">The runtime for static recompilations.</h3>

<p align="center">
  Bring the translated game code. NUE provides everything around it:<br>
  window, input, audio, storage, power, graphics and Win32 / Direct3D 9 compatibility,<br>
  on Windows, Linux (including SteamOS), Android and iOS.
</p>

<p align="center">
  <a href="#why">Why</a> &middot;
  <a href="#the-stack">The stack</a> &middot;
  <a href="#beyond-direct3d-9">Beyond Direct3D 9</a> &middot;
  <a href="#built-for-reverse-engineering">Reverse engineering</a> &middot;
  <a href="#what-you-get">What you get</a> &middot;
  <a href="#status">Status</a> &middot;
  <a href="#build">Build</a> &middot;
  <a href="#documentation">Documentation</a>
</p>

<p align="center">
  <img alt="platforms" src="https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20Android%20%7C%20iOS-555?style=flat-square">
  <img alt="API" src="https://img.shields.io/badge/API-C%20%2B%20C%2B%2B%20wrapper-555?style=flat-square">
  <img alt="base" src="https://img.shields.io/badge/base-SDL3%20%2B%20bgfx-555?style=flat-square">
</p>

## Why

A static recompilation turns a game's machine code into source code you can build for any target. The translated code is only part
of the work. The original program still expects its operating system, its graphics, sound and input APIs, its file layout and its
32-bit memory model, and every recompilation project ends up rewriting that same surface from scratch.

NUE Engine is that surface, written once. A recompilation links against it and only has to supply what is specific to the game.
There are two ways in:

- **Port a Windows-era game.** Link the legacy ABI and the recompiled code keeps calling the Win32, Direct3D 9, DirectSound and
  DirectInput functions it was built for, backed by the native modules underneath.
- **Build something native.** Use the native C API directly (or the C++ wrapper) for a tool, a port or a new game that needs the
  same platform services.

## The stack

<p align="center">
  <img alt="The NUE Engine stack: the recompiled game on top of the legacy ABI, the native ABI and the platform layers" src="assets/stack.svg" width="860">
</p>

The legacy ABI sits on top of the native one and never the other way round, so a native program links no legacy code. Platform
code is isolated in one layer per operating system, with SDL3 underneath for the parts that are the same everywhere.

This is how one call travels through it:

<p align="center">
  <img alt="A Direct3D 9 call travelling from the recompiled game through the legacy ABI, the native ABI and the platform layer to the GPU" src="assets/flow.gif" width="860">
</p>

## Beyond Direct3D 9

Direct3D 9 is where NUE started, not where it ends. The plan is to extend the same engine to the machines games were written for,
from Xbox One and PlayStation down to the Nintendo 64, handhelds included. Each source platform gets its own legacy layer, the
equivalent of `nue_d3d9`, on top of the same native ABI. Only that layer is written again. Everything underneath, on the four
platforms NUE runs on, stays the same.

<p align="center">
  <img alt="The legacy layer changes with the source platform while the native ABI and the platform layers stay the same" src="assets/legacy-layers.gif" width="860">
</p>

Every platform below is in scope, one by one, and each one gets its own legacy layer. Only the Direct3D 9 layer exists today. All
the others are planned and are written when a recompilation project needs them.

| Maker | Platform | Year | Kind | Layer | State |
|---|---|---|---|---|---|
| Microsoft | Windows, Direct3D 9 | - | PC | `legacy_d3d9` | In development |
| Microsoft | Windows, Win32 subset | - | PC | `legacy_win32` | Planned |
| Microsoft | Windows, DirectSound | - | PC | `legacy_dsound` | Planned |
| Microsoft | Windows, DirectInput | - | PC | `legacy_dinput` | Planned |
| Microsoft | Xbox | 2001 | Console | `legacy_xbox` | Planned |
| Microsoft | Xbox 360 | 2005 | Console | `legacy_x360` | Planned |
| Microsoft | Xbox One | 2013 | Console | `legacy_xone` | Planned |
| Sony | PlayStation | 1994 | Console | `legacy_ps1` | Planned |
| Sony | PlayStation 2 | 2000 | Console | `legacy_ps2` | Planned |
| Sony | PlayStation 3 | 2006 | Console | `legacy_ps3` | Planned |
| Sony | PlayStation 4 | 2013 | Console | `legacy_ps4` | Planned |
| Sony | PSP | 2004 | Handheld | `legacy_psp` | Planned |
| Nintendo | Nintendo 64 | 1996 | Console | `legacy_n64` | Planned |
| Nintendo | GameCube | 2001 | Console | `legacy_gc` | Planned |
| Nintendo | Wii | 2006 | Console | `legacy_wii` | Planned |
| Nintendo | Wii U | 2012 | Console | `legacy_wiiu` | Planned |
| Nintendo | Switch | 2017 | Console and handheld | `legacy_switch` | Planned |
| Nintendo | Game Boy | 1989 | Handheld | `legacy_gb` | Planned |
| Nintendo | Game Boy Color | 1998 | Handheld | `legacy_gbc` | Planned |
| Nintendo | Game Boy Advance | 2001 | Handheld | `legacy_gba` | Planned |
| Nintendo | Nintendo DS | 2004 | Handheld | `legacy_nds` | Planned |
| Nintendo | Nintendo 3DS | 2011 | Handheld | `legacy_3ds` | Planned |
| Sega | Saturn | 1994 | Console | `legacy_saturn` | Planned |
| Sega | Dreamcast | 1998 | Console | `legacy_dc` | Planned |

The design is in the [roadmap](docs/ROADMAP.md) and [how to write a legacy layer](docs/LEGACY_LAYERS.md) says what each one is made of.
Consoles that share an architecture can share most of a layer, so this is a list of targets and not a list of separate pieces of work.

## Built for reverse engineering

Writing a layer means finding out what a game really asks of its platform. NUE is built around a loop that makes that quick:
record the calls a game makes, keep them in a versioned trace, replay them with no game running, and compare the result with a
reference. The loop exists today for Direct3D 9, with a call recorder, trace tools, a headless replay tool with a deterministic
call hash, a bisect by draw range and ground-truth tests against the real runtime. The plan is the same loop for every layer.

<p align="center">
  <img alt="The reverse engineering loop: record, trace, replay, compare, and a difference goes back to the start" src="assets/reverse-engineering.gif" width="860">
</p>

The tools and the workflow are in [Reverse engineering with NUE](docs/REVERSE_ENGINEERING.md).

## Native on every platform

NUE does not stop at the lowest common denominator. Each platform layer uses what that system actually offers, and the native API
exposes the result the same way everywhere.

<p align="center">
  <img alt="Native services used on Windows, Linux and SteamOS, Android and iOS" src="assets/platforms.svg" width="860">
</p>

## What you get

| Feature | What you get |
|---|---|
| Two ABIs | A stable native C API for new code and a legacy ABI for recompiled Windows-era games. |
| Window and display | Windowed, borderless and full-screen modes, DPI, safe areas, refresh rate, multi-monitor. |
| Input | Keyboard, mouse, gamepads, touch and sensors, with last-used-device detection for the right button glyphs. |
| Audio | Devices and streams with a mixer, conversion and resampling, and latency control. |
| Storage | A virtual file system for game data and user data with case-insensitive paths, async reads, mounts, manifests and per-platform data import. |
| Power and game mode | Game mode on every platform, thermal and battery state, keep-awake and performance hints. |
| Lifecycle | Pause, resume, low memory, focus and display changes as events, the same on every platform. |
| Memory budgets | Budgets are runtime parameters with a default policy per platform. Nothing is hard-coded. |
| Graphics | bgfx (Vulkan, Direct3D 11/12, Metal, OpenGL ES) and a Direct3D 9 layer with a shader translator. |
| Textures | BC/DXT, ETC, ASTC and PVRTC: capability detection, transcoding and a cache. |
| Assets | Open a game's resource files, convert them (textures included) and pack them into `.nuepack`, or back into the original format. A command-line tool, `nue_pack`, does the same. |
| 32-bit guests | Handle tables and guest pointers for recompiled code that expects a 32-bit address space (planned, part of the legacy ABI). |
| Diagnostics | Logging, crash reports and counters, with a deterministic null backend so tests need no window, sound card or GPU. |

## Graphics

Recompiled Windows games draw with Direct3D 9. The Direct3D 9 layer keeps that API and renders it through bgfx, so the recompiled
code does not change per platform. Direct3D 11 is the backend exercised so far; the others come from bgfx.

<p align="center">
  <img alt="Direct3D 9 calls go through the Direct3D 9 layer and the shader translator to bgfx and the GPU" src="assets/graphics.svg" width="860">
</p>

## Textures

Old games ship textures in formats that many GPUs do not sample. The texture module checks what the GPU supports, uploads what it
can as is, transcodes the rest and caches the result.

<p align="center">
  <img alt="A texture goes through a capability check, then is uploaded as is or transcoded, cached and sent to the GPU" src="assets/textures.svg" width="860">
</p>

## Status

| Module | State |
|---|---|
| Core | Tested on Windows x64 and x86, and on Linux |
| SDL3 backend | Tested with SDL's dummy drivers |
| Windows layer | Tested on Windows |
| Textures | Tested on Windows |
| Assets (NFSU2 BUN and LZC, JDLZ, `.nuepack`) | Tested on Windows, including byte-identical round trips on the game's own files |
| Direct3D 9 layer | Builds on Windows and matches the system runtime in state tests |
| Linux and SteamOS layer | In development |
| Android layer | In development, builds for arm64 |
| iOS layer | Builds on macOS, not yet run on a device |
| Win32, DirectSound and DirectInput layers, guest memory | Planned |

The per-feature breakdown is kept in [docs/STATUS.md](docs/STATUS.md).

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
- [Roadmap](docs/ROADMAP.md)
- [Writing a legacy layer](docs/LEGACY_LAYERS.md)
- [Reverse engineering with NUE](docs/REVERSE_ENGINEERING.md)
- [Contributing](CONTRIBUTING.md)
- [Porting a platform](docs/PORTING.md)
- [Graphics and the Direct3D 9 seam](docs/GFX_D3D9_SEAM.md)
- [Status](docs/STATUS.md)
- [Working in the repository](docs/AGENT_GUIDE.md)

## Built on

[SDL3](https://github.com/libsdl-org/SDL), [bgfx](https://github.com/bkaradzic/bgfx) and SPIRV-Cross,
[astc-encoder](https://github.com/ARM-software/astc-encoder), [etcpak](https://github.com/wolfpld/etcpak),
[bcdec](https://github.com/iOrange/bcdec) and [stb_image](https://github.com/nothings/stb).
