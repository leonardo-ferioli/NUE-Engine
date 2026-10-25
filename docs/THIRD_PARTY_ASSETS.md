# Third party knowledge used by the asset module

No third party source code is included in the repository or ported into it. What was used as reference:

| Source | Licence | Used for |
|---|---|---|
| PryHUB `crates/gizmo-nfs` (https://github.com/bdrtr/PryHUB) | MIT OR Apache-2.0 | Read as a cross-check: the ids of the texture pack chunks (`0xB3300000` ...), the existence of palettised texture types (`0x08`, `0x80`, `0x81`) and the `0x11` filler. The layouts in `docs/ASSET_FORMATS.md` were derived from the game files and verified by byte exact rebuilds; the car `TEXTURES.BIN` layout (4.6: 24 byte descriptors, compressed per-texture blobs) differs from the one of `.BUN` files and is implemented read only |
| The project's own validated JDLZ reference decoder (a benchmark of the recompilation project) | project internal | Read to confirm the token format; the decoder and the encoder in `src/assets/jdlz.cpp` were written from that description and checked against the game's `.lzc` files |
| PryHUB `crates/gizmo-nfs` (second reading: `compression/huff.rs`, `geometry/*`, `texture/decode.rs`) | MIT OR Apache-2.0 | The HUFF decoder in `src/assets/huff.cpp` is an independent C++ rewrite of the algorithm as described and implemented there (the bit reader is simplified to a plain most-significant-bit-first stream; the length number, code length table, symbol leap table and clue escape follow the same format). The chunk ids and field offsets of the solid / mesh layout (section 5 of `docs/ASSET_FORMATS.md`) and the car texture pack layout (4.6) were first learned from the PryHUB sources and then re-checked and extended on the game files (header filler shifts, the vertex format flag, the matrix, the city statistics). Attribution: Copyright the PryHUB / Gizmo Engine authors, MIT OR Apache-2.0. No source file was copied |
| GvennLcr `globalc-tool` (MIT) | MIT | Pointed out as a JDLZ reference; **not read**, not used |
| SpeedReflect / NFSTools `Binary` (GPL-3.0) | GPL-3.0 | **Not used**: no code read, copied or translated |

If code from a permissively licensed project is ever ported, its notice goes in this file.
