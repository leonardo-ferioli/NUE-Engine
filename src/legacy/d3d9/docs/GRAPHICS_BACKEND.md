# Graphics backend decision: bgfx first

Status: **chosen, not yet validated. bgfx is the only renderer.** The first step is a measured spike (see "Validation").

## Requirements

1. The game's graphics calls stay at **Direct3D 9 semantics** behind our own API; the translated game does not know the backend.
2. **Android:** the player can switch between **Vulkan and OpenGL ES** from the game's menu, whatever the device.
3. **iPhone/iPad:** must run (Metal).
4. PC (Windows, Linux, macOS) shares the same code path. **There is no native Direct3D 9 backend in the end state**: on Windows bgfx runs on its Direct3D 11 / 12 / Vulkan renderers. The native Direct3D 9 pass-through in `native32.cpp` is a temporary bridge, used to record reference traces and images, and is deleted once bgfx reaches parity (see `PLATFORM_API.md`, section 5).
5. Process budget of 2 GB. No dependency that forces its own main loop on the game.

## Options considered

| Option | Vulkan | OpenGL ES | Metal | Notes |
|---|---|---|---|---|
| **bgfx** (BSD-2-Clause) | yes | yes (3.0+) | yes | Renderer only (no window/input/audio); renderer chosen at init; very active; Android, iOS 16+, macOS 13+, Linux, Windows |
| SDL_gpu (zlib) | yes (Android: "certain devices") | **no** | yes | Includes window/input/audio via SDL; young API; shader translation via SDL_shadercross (preview) |
| The Forge (Apache-2.0) | yes | **no** (removed) | yes | Wants to own the main loop; Windows is Direct3D 12 only; large dependency; consoles need a commercial license |
| DXVK (+ MoltenVK) | yes | **no** | via MoltenVK | Vulkan-shaped design; d9mt documents that the per-draw cost comes from that plumbing |
| d9mt (Direct3D 9 to Metal) | – | – | yes | Wine/CrossOver and Apple Silicon only; tested with one game; **no license** (not reusable) |
| DXMT (Direct3D 10/11 to Metal) | – | – | yes | LGPL-2.1; Direct3D 10/11 and Wine-bound; a reference, not a component |
| Own backends only | to write | to write | to write | Most control; driver-bug diversity on mobile is the real cost; roughly twice the code |

Only bgfx covers all three requirements at once.

## Architecture

```
translated game  ->  Direct3D 9 semantics layer (ours, written once)
                 ->  small GPU interface (ours; explicit, close to SDL_gpu's model)
                 ->  bgfx adapter  ->  Vulkan | OpenGL ES 3 | Metal | D3D11/12
                 ->  (later) own Vulkan / Metal backends where profiling demands
```

- The Direct3D 9 layer can lean on DXVK's Direct3D 9 front-end for semantics (zlib license) and on Direct3D 9-on-12 as a reference.
- Fixed-function Direct3D 9 has no equivalent in modern APIs: it is emulated with generated shaders, and the game's own Direct3D 9
  shader bytecode is translated (own IR -> SPIR-V -> SPIRV-Cross -> GLSL ES / MSL / HLSL, wrapped in bgfx's shader container by our code,
  cached on disk). `shaderc` is used only at our build time for our own shaders. Decision, alternatives and licences in `PLATFORM_API.md`, 3.4.
- bgfx applies resource updates before the frame's draws, while Direct3D 9 Lock/Unlock is ordered with draws: the layer versions buffers and
  copies the range each draw reads (`PLATFORM_API.md`, 3.5).
- Switching renderer from the menu is a device reset: Direct3D 9 already defines device loss and recreation, and the game recreates
  its default-pool resources itself. The layer keeps "managed" resources (memory cost to be weighed against the 2 GB budget).
- Window, input, audio and files are separate (SDL3 candidate). The game keeps its own main loop on its own thread; bgfx's thread and
  the application thread talk to it through queues.

## Risks (known, unmeasured)

- OpenGL ES has high per-draw overhead; the game issues about 58,000 Direct3D 9 calls per frame, so batching and state caching decide
  whether GLES is usable.
- bgfx submission differs from Direct3D 9 Lock/Unlock (the game locks vertex buffers about 90 times per frame): shadow copies and per-draw uploads needed.
- **Shaders are the first risk** (translation of Direct3D 9 bytecode and fixed-function emulation, compile time on phones): spike S1 in `PLATFORM_API.md`.
- Compressed texture formats (DXT) are not universal on mobile: conversion (e.g. to ASTC) is ours.
- Apple distribution rules were not researched.

## Validation

A spike before writing the Direct3D 9 layer: a textured quad and tens of thousands of draws with state and texture changes per
frame through bgfx on Windows, Android (Vulkan and OpenGL ES, including a low-end device) and an iPhone, measuring nanoseconds per
draw, CPU use, memory and start-up success. If a requirement fails, fall back to own backends for that platform behind the same
interface.
