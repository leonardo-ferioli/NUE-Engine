# .nuepack

The engine's own resource package: what `nue_pack convert` writes and the engine reads. Memory mappable, several texture variants per platform
class, a chunk index, content hashes and a version. Implemented in `src/assets/nuepack.cpp` (public API in `include/nue/assets.h`, `nue_pack_*`).

Goals: open a pack with a few reads (header + tables) and get any entry without parsing the rest; texture data is stored raw and aligned so it can be
uploaded straight from a mapped file; one pack holds the variants for every platform so a build ships one file; a pack made from a game container
keeps its chunk tree and can be turned back into the game's own `.BUN`/`.LZC`.

All integers little endian. Deterministic: the same input and options give the same bytes (no timestamps, no paths).

## Layout

```
header            128 bytes
entry table       entry_count x 136 bytes            (8 byte aligned)
tree table        tree_count  x 48 bytes             (8 byte aligned; empty for loose-file packs)
string table      NUL terminated names, offset 0 = ""
data blocks       each on a multiple of data_alignment (64); identical blocks are stored once and shared
```

### Header (128 bytes)

| offset | field |
|---|---|
| 0 | u32 magic `0x5045554E` ("NUEP") |
| 4 | u16 version major (1), u16 version minor (0). A reader refuses another major; a higher minor only adds fields |
| 8 | u32 header size (128) |
| 12 | u32 flags: bit 0 = the pack holds a container tree |
| 16 | u64 file size |
| 24 | u64 entry table offset; 32: u32 entry count; 36: u32 entry size (136) |
| 40 | u64 string table offset; 48: u64 string table size |
| 56 | u64 tree table offset; 64: u32 tree count; 68: u32 tree entry size (48) |
| 72 | u32 variant mask (bit `1 << nue_pack_variant`) |
| 76 | u32 data alignment (power of two, 64) |
| 80 | u32 string table offset of the source name (free text, e.g. `GlobalB.lzc`) |
| 88 | char game format[24] ("nfs-bun"), 112: char codec[8] ("jdlz" or "") |
| 120 | u32 CRC-32 of this header with this field zero |
| 124 | u32 CRC-32 of everything from the entry table to the end of the string table |

### Entry (136 bytes)

| offset | field |
|---|---|
| 0 | u32 kind (0 blob, 1 texture, 2 texture pack, 3 other) |
| 4 | u32 variant: 0 source, 1 bc, 2 astc, 3 etc2, 4 pvrtc, 5 rgba8 |
| 8 | u32 flags: bit 0 palettised source (indices + 1024 byte palette), bit 1 payload of a tree node |
| 12 | u32 name offset in the string table |
| 16 | u64 name hash (FNV-1a 64 of the lower-cased name; the reader keeps a sorted index by it) |
| 24 | u64 data offset (absolute); 32: u64 stored size; 40: u64 raw size |
| 48 | u32 codec (0 none, 1 JDLZ); 52: u32 CRC-32 of the raw bytes |
| 56 | u32 `nue_tex_format` of the pixels; 60: width; 64: height; 68: depth; 72: mip levels; 76: layers; 80: faces |
| 84 | u32 native format (the game's own code, e.g. the NFS compression byte; 0 when not from a game file) |
| 88 | u32 tree node the entry belongs to (0xFFFFFFFF none); 92: u32 index of the texture inside its owner |
| 96 | u8 SHA-256 of the raw bytes [32] |
| 128 | u64 resource key (the game's name hash) |

Texture entries hold the mip levels back to back, largest first, layers/faces/levels in the order of `nue_tex_image`; they are never compressed, so
`data offset` points at pixels that can be handed to the GPU. Blobs larger than 256 bytes are JDLZ compressed when that makes them at least 16 bytes
smaller. A texture exists once per variant with the same name; a reader asks for a variant and falls back to the others (`nue_pack_entry_find`).

### Tree entry (48 bytes) - packs made from a game container

| offset | field |
|---|---|
| 0 | u64 id (format specific: the NFS chunk id) |
| 8 | u32 parent node (0xFFFFFFFF root); 12: u32 kind (blob, group, padding, tail); 16: u32 flags (bit 8: payload is generated from the textures); 20: u32 header size |
| 24 | u32 padding alignment; 28: u32 entry holding the payload (0xFFFFFFFF none); 32: u64 payload size; 40: u32 name offset or 0xFFFFFFFF |

Nodes are in pre-order, as in the container. Padding made of zeros has no entry (its size is enough); the pixel data chunk of a texture pack has none
either: it is generated from the textures.

## Variants

`nue_pack_variant`: **source** keeps the pixels as the game file had them (DXT blocks, BGRA8, palettised indices + palette) so the pack can be exported
back losslessly; **bc** desktop, **astc** Android/Apple (ASTC LDR), **etc2**, **pvrtc** (old Apple GPUs), **rgba8** universal fallback. The target
format of each variant comes from the texture module's planner for the matching device class (`nue_tex_make_plan`, `docs/TEXTURE.md`), so the choices
follow the same rules as the runtime. When a variant equals the source bytes (BC for DXT sources) the data is stored once. `--max-dim` drops top mip
levels in the non-source variants. A texture that cannot be transcoded for a variant is stored as RGBA8 there and the conversion returns
`NUE_OK_PARTIAL`.

## Reading

- `nue_pack_open_file` reads header and tables; entry data is read on demand with positioned reads. `NUE_PACK_OPEN_MAP` maps the file instead and
  uncompressed entries are returned as pointers into the mapping. `nue_pack_open_memory` works on any buffer.
- Opening checks the magic, the version, both CRCs, that every table and entry lies inside the file, alignment, sizes and tree links. Damaged files
  give `NUE_ERR_CORRUPT` (`NUE_ERR_VERSION_MISMATCH` for another major); `NUE_PACK_OPEN_VERIFY` / `nue_pack_verify` also check every entry's SHA-256 and CRC.
  Of 400 random single bit flips in a test pack, all but the unused alignment bytes were caught.
- `nue_pack_export_container` rebuilds the game container (needs the tree and a registered format) from the source variant; writing it gives the
  original `.BUN` byte for byte (tested on all 70 game files). Writing with the pack's codec recompresses with the engine's encoder, so an `.LZC`
  produced this way is equivalent, not identical, to the game's original.

## Mount (storage source)

`nue_pack_mount_create(ctx, path, preferred_variant)` makes a read-only named source: `nue_pack_file_open(mount, "PACK/TEXTURE")`, `nue_pack_file_read_at`,
and `nue_pack_file_read_async`, which for entries stored raw in a pack on disk is a real `nue_io_read_async` on the pack file (poll/wait/release with the
`nue_io_*` functions); entries that had to be decoded (JDLZ) or live in memory are read before the call returns (`*out_request == 0`, callback already
called). `nue_pack_mount_create_container` does the same for an opened `.BUN`/`.LZC` (`node/<index>` payloads, `<texture>.dds`).
Plugging the mount into the VFS paths themselves (`nue_file_open("pack:...")`) needs a hook in the core VFS: see `docs/REQUESTS_assets.md`.

## Versioning

Major 1. Compatible additions (new entry kinds, variants, flags) raise the minor; a reader ignores kinds and variants it does not know. A change of any
existing field's meaning or size raises the major.
