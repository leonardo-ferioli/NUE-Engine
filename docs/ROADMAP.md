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

Every platform is listed one by one in the [console catalogue](#console-catalogue) below. What a layer has to answer is the same list
for each of them: a graphics API or command stream, audio, input, system services and a memory map. Nothing in the catalogue is
started except the Direct3D 9 layer.

## Console catalogue

Every platform below is in scope, listed one by one, and each one needs its own legacy layer. Layer names follow `legacy_<code>`.
All are **planned** except the Direct3D 9 layer: none of the others is started, and a layer is only written when a recompilation
project needs it.

| Maker | Platform | Year | Kind | Layer | State |
|---|---|---|---|---|---|
| Microsoft | Windows, Direct3D 9 | - | PC | `legacy_d3d9` | In development |
| Microsoft | Windows, Win32 subset | - | PC | `legacy_win32` | Planned |
| Microsoft | Windows, DirectSound | - | PC | `legacy_dsound` | Planned |
| Microsoft | Windows, DirectInput | - | PC | `legacy_dinput` | Planned |
| Microsoft | Xbox | 2001 | Console | `legacy_xbox` | Planned |
| Microsoft | Xbox 360 | 2005 | Console | `legacy_x360` | Planned |
| Microsoft | Xbox One | 2013 | Console | `legacy_xone` | Planned |
| Microsoft | Xbox Series X and S | 2020 | Console | `legacy_xsx` | Planned |
| Sony | PlayStation | 1994 | Console | `legacy_ps1` | Planned |
| Sony | PlayStation 2 | 2000 | Console | `legacy_ps2` | Planned |
| Sony | PlayStation 3 | 2006 | Console | `legacy_ps3` | Planned |
| Sony | PlayStation 4 | 2013 | Console | `legacy_ps4` | Planned |
| Sony | PlayStation 5 | 2020 | Console | `legacy_ps5` | Planned |
| Sony | PSP | 2004 | Handheld | `legacy_psp` | Planned |
| Sony | PlayStation Vita | 2011 | Handheld | `legacy_vita` | Planned |
| Nintendo | NES | 1983 | Console | `legacy_nes` | Planned |
| Nintendo | SNES | 1990 | Console | `legacy_snes` | Planned |
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
| Sega | Master System | 1985 | Console | `legacy_sms` | Planned |
| Sega | Mega Drive / Genesis | 1988 | Console | `legacy_md` | Planned |
| Sega | Game Gear | 1990 | Handheld | `legacy_gg` | Planned |
| Sega | Saturn | 1994 | Console | `legacy_saturn` | Planned |
| Sega | Dreamcast | 1998 | Console | `legacy_dc` | Planned |
| Atari | Atari 2600 | 1977 | Console | `legacy_a2600` | Planned |
| Atari | Lynx | 1989 | Handheld | `legacy_lynx` | Planned |
| Atari | Jaguar | 1993 | Console | `legacy_jaguar` | Planned |
| Others | PC Engine | 1987 | Console | `legacy_pce` | Planned |
| Others | Neo Geo | 1990 | Console | `legacy_neogeo` | Planned |
| Others | 3DO | 1993 | Console | `legacy_3do` | Planned |
| Others | WonderSwan | 1999 | Handheld | `legacy_ws` | Planned |
| Others | N-Gage | 2003 | Handheld | `legacy_ngage` | Planned |

Consoles that share an architecture family can share most of a layer, so the table is a list of targets and not a list of
separate pieces of work.

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
