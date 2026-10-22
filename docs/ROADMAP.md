# Roadmap

NUE Engine started with one source platform, Windows and Direct3D 9, and one game. The design does not stop there. The goal is a
runtime that any static recompilation can sit on, whatever machine the game was written for, and that runs the result on Windows,
Linux and SteamOS, Android and iOS.

States used below: **in development** (code exists and is exercised), **planned** (designed, not started).

## The idea

A recompiled game talks to the machine it was written for: a graphics API, an audio API, an input API, a memory map, a clock.
NUE answers those calls with one **legacy layer** per source platform, built on the native ABI. The native ABI and the platform
layers underneath do not change from one source platform to the next.

```
 recompiled game (any source platform)
        |
 legacy layer for that platform        <- the only part that is written again
        |
 native ABI                            <- shared by every layer
        |
 platform layers: Windows, Linux and SteamOS, Android, iOS
```

The first legacy layer, `legacy_d3d9` (the `nue_d3d9` translator), is the reference for the rest. [LEGACY_LAYERS.md](LEGACY_LAYERS.md)
describes what a layer is made of and how to build one.

## Source platforms

| Family | What a layer has to answer | State |
|---|---|---|
| Windows, Direct3D 9 | Direct3D 9, plus a Win32 subset, DirectSound and DirectInput | Direct3D 9 in development, the rest planned |
| Other Direct3D versions | The graphics API of the game's era | Planned, as projects need them |
| Xbox One | Its DirectX 11 and 12 flavours, audio, input and system services | Planned |
| PlayStation family | The console's graphics API, audio, input and system services | Planned |
| Nintendo 64 | The reality co-processor display lists (RSP and RDP), audio, controller, big-endian memory | Planned |
| Handhelds | PSP, Vita, DS, 3DS, GBA and similar, each with its own graphics and system layer | Planned |

Everything in between is in scope when a project needs it. Nothing in this table is started except the Direct3D 9 layer.

## What every layer needs from the engine

Each new source platform leans on the same shared parts, so those come first:

- **Guest memory.** Address spaces that differ from the host: 32-bit pointers on a 64-bit host, big-endian data, fixed memory maps,
  handle tables. This is the legacy core, planned and shared by all layers.
- **Texture formats.** Many consoles use tiled, swizzled or palettised formats. The texture module is where those get decoded and
  transcoded, next to BC, ETC, ASTC and PVRTC.
- **Display lists and command streams.** Older consoles do not call a graphics API. They feed a command stream to a processor.
  The graphics layer for those is an interpreter of that stream that drives the same small GPU interface the Direct3D 9 layer uses.
- **Audio.** Streams, sequenced and synthesised sound, all mixed by the native mixer.
- **Input.** Controllers mapped to the native input module, with last-used-device glyphs.
- **Timing.** Fixed refresh rates and frame pacing that the original game relied on.

## Making reverse engineering easy

Writing a layer means understanding what a game really does with an API. The engine is being shaped so that is quick to observe:
record what the game calls, replay it without the game, compare the result with a reference. That workflow already exists for
Direct3D 9 and is described in [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md). The plan is to make it the same for every layer.

## Order of work

1. Finish the native ABI and the four platform layers (in development).
2. Legacy core: guest memory and handle tables (planned).
3. Win32, DirectSound and DirectInput layers next to the Direct3D 9 layer (planned).
4. A shared trace format and replay harness for every layer (planned).
5. The first non-Windows legacy layer, chosen when a game needs it (planned).

## What NUE does not contain

NUE contains no game data, no console firmware and no code taken from a console SDK. A legacy layer is written from public
documentation and from observing a game, and a recompilation project supplies its own game files.
