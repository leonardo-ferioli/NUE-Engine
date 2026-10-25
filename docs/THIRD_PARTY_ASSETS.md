# Third party knowledge used by the asset module

No third party source code is included in the repository or ported into it. What was used as reference:

| Source | Licence | Used for |
|---|---|---|
| PryHUB `crates/gizmo-nfs` (https://github.com/bdrtr/PryHUB) | MIT OR Apache-2.0 | Read as a cross-check: the ids of the texture pack chunks (`0xB3300000` ...), the existence of palettised texture types (`0x08`, `0x80`, `0x81`) and the `0x11` filler. The layouts in `docs/ASSET_FORMATS.md` were derived from the game files and verified by byte exact rebuilds; the TPK of the car `TEXTURES.BIN` files described there (24 byte descriptors, compressed per-texture blobs) is a different layout from the one in `.BUN` files and is not implemented |
| The project's own validated JDLZ reference decoder (a benchmark of the recompilation project) | project internal | Read to confirm the token format; the decoder and the encoder in `src/assets/jdlz.cpp` were written from that description and checked against the game's `.lzc` files |
| GvennLcr `globalc-tool` (MIT) | MIT | Pointed out as a JDLZ reference; **not read**, not used |
| SpeedReflect / NFSTools `Binary` (GPL-3.0) | GPL-3.0 | **Not used**: no code read, copied or translated |

If code from a permissively licensed project is ever ported, its notice goes in this file.
