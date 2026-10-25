# Asset container formats

Layouts established by the asset module (`src/assets`, `include/nue/assets.h`). Everything here was **observed on the files of an installed
copy of Need for Speed Underground 2 (PC)** and checked by rebuilding the files (see "How this was verified"). Offsets are file offsets,
all integers are little endian. Where a field is not understood it is said so and the bytes are kept untouched.

Sources: the layouts come from reading the files. The community project PryHUB (`gizmo-nfs`, MIT/Apache-2.0) was read as a cross-check for the
texture pack chunk ids and the palettised texture types (see `docs/THIRD_PARTY_ASSETS.md`); the GPL editors were not used.

## 1. Files

| Extension | What it is |
|---|---|
| `.BUN` | a chunk stream (section 3). Not compressed |
| `.LZC` | a `.BUN` wrapped in one JDLZ stream (section 2). `GlobalB.lzc` decodes to exactly `GLOBALB.BUN`, `InGameCommon.lzc` to `InGameCommon.bun` |
| `STREAM*.BUN`, `L4R*.BUN` | the same chunk stream (tracks / city); same rules, world chunks are not interpreted |

The installed game has 70 `.BUN`/`.LZC` files (67 + 3); one (`FRONTEND/FRONTA.BUN`) is empty (0 bytes) and is a valid, empty container.

## 2. JDLZ stream

```
0    "JDLZ"
4    u8 version = 2, u8 header size = 0x10, u16 0
8    u32 size of the decoded data
12   u32 size of the whole stream, header included (== the file size of an .LZC)
16   u8 flags A, u8 flags B, then tokens
```

Tokens are read one at a time. Flags A has one bit per token, least significant bit first: `0` literal (one byte copied), `1` match (two bytes
`f s`). Flags B has one bit per match, least significant bit first: `1` near match, `0` far match.

```
near  (flag B = 1):  count = ((f & 0xF0) << 4 | s) + 3        3..4098      distance = (f & 0x0F) + 1             1..16
far   (flag B = 0):  count = (f & 0x1F) + 3                   3..34        distance = ((f & 0xE0) << 3 | s) + 17  17..2064
```

The distance counts back from the next output byte; a match may overlap its own output (runs). When the 8 bits of a flag byte are used up, the
next flag byte is in the stream right after the token that used the last bit: flags A first, then flags B when both ran out on the same token. The
decoder stops as soon as the decoded size is reached, without fetching a further flag byte. The game's own files carry one such unread trailing
byte (the encoder writes the flag byte eagerly); this encoder does the same.

- The near/far kind is fixed by the distance, so a token sequence has exactly one encoding.
- Decoder: strict. A match reaching before the start, beyond the declared size, or input that ends early is `NUE_ERR_CORRUPT`; nothing is
  read or written out of bounds (checked with 2000 bit-flipped streams in `test_jdlz`).
- Encoder (`nue_jdlz_compress`): LZ77 with a hash chain over the 2064 byte window. Levels 0-2 greedy, 3-6 lazy, 7-9 optimal parse (a literal costs
  9 bits, any match 18 bits). Deterministic. On `InGameCommon.bun` (1,595,640 bytes): level 1 32.2 %, level 6 30.0 %, level 9 29.5 %; the game's file is
  30.1 %. Output is decoded by the independent reference decoder in the test and by the real game file's decoder semantics (the decoded size of the
  game's own `InGameCommon.lzc` is the 1,595,640 bytes the game's decompressor produces).
- The original encoder is not reproduced bit for bit (its parse choices are unknown); an unchanged `.LZC` is therefore written back from its
  original bytes, and only changed content is compressed again.

## 3. Chunk stream (`.BUN`)

```
u32 id, u32 size, size bytes of payload, next chunk immediately after
```

- Bit 31 of the id set: the payload is itself a chunk sequence that fills it exactly (a *group*). 100 % of the 70 observed files parse this way.
- id 0 is **padding**: its payload is filler (zeros in every observed case) that keeps the following chunk header on an alignment boundary. A padding
  chunk of size 0 (8 bytes) exists when no filler is needed. The alignment is not stored; observed starts of the following chunk: 16 (in `0x80134000`
  sections), 64 (before `0xB3310000`), and at least 128 (before big data chunks; many are 256 ... 32768 by chance).
- Chunks that are not groups are opaque to the framework; unknown chunks are kept byte for byte.

Rewriting rule used by the NFS plugin (reproduces all observed files exactly): for a padding chunk followed by a sibling, `A = min(lowest set bit of
the sibling's header offset, 32768)` is recorded. When the layout changes the padding payload becomes `floor(old size / A) * A + ((A - payload start %
A) % A)`: any extra whole blocks are kept, the end is aimed at the alignment again. Unchanged layouts give the original size by construction.

Chunk ids seen (count over the whole install, depth 0 unless noted; meaning only where established):

| id | what |
|---|---|
| `0xB3300000` | **texture pack** (TPK), 689 in the install. Section 4 |
| `0x80134000` ... `0x00134B03` | solid / geometry sections of the cars (2849 sections, 29,170 solids). **Not interpreted**, kept opaque |
| `0x00030201`, `0x00030203`, `0x00030210`, `0x00034xxx`, `0x00035xxx`, `0x00036xxx`, `0x00037xxx`, `0x00039xxx`, `0x0003Bxxx`, `0x00E34xxx` | game tables and scripts (physics, collision, animation, sound ...). **Not interpreted** |
| `0` | padding |

The tool `nue_pack list FILE --nodes` prints the tree of any file. World/streaming chunks (`STREAM*.BUN`, `L4R*.BUN`) parse as chunk streams and
round trip, but their meaning is not documented here.

## 4. Texture pack (`0xB3300000`)

```
0xB3300000  texture pack
  0x00000000  padding (aligns the next header to 64)
  0xB3310000  table group
    0x33310001  pack header (0x7C bytes)
    0x33310002  N x { u32 name hash, u32 0 }
    0x33310004  N x texture record (0x7C bytes each)
    0x33310005  N x format record (32 bytes each)
  0x00000000  padding (aligns the next header to 128 or more)
  0xB3320000  data group
    0x33320001  24 bytes { 0, 0, 1, pack hash, 0, 0 }
    0x00000000  padding (header of the next chunk on 128 or more)
    0x33320002  pixel data
```

### 4.1 Pack header (`0x33310001`, 0x7C bytes)

| offset | field |
|---|---|
| 0x00 | u32 version (5) |
| 0x04 | char name[28], NUL terminated (e.g. `GLOBALMESSAGE`) |
| 0x20 | char path[64] (e.g. `Global\GlobalMessageTextures.tpk`) |
| 0x60 | u32 pack hash (same value as in `0x33320001`) |
| rest | zero / not understood, kept |

### 4.2 Texture record (`0x33310004`, 0x7C bytes)

| offset | field |
|---|---|
| 0x00 | 12 zero bytes |
| 0x0C | char name[24], NUL terminated (names are cut to 23 characters) |
| 0x24 | u32 name hash (equals the entry of `0x33310002` with the same index) |
| 0x28 | u32 class hash (`0x001A93CF` for plain textures, others for animated / special) |
| 0x2C | u32 0 |
| 0x30 | u32 image placement: offset of the first mip level from the data base (4.4) |
| 0x34 | u32 palette placement: offset of this texture's palette inside the palette pool (running total of the palettes before it; also nonzero for non-palettised textures) |
| 0x38 | u32 image size: all mip levels, level 0 first, tightly packed |
| 0x3C | u32 palette size: 0 or 1024 |
| 0x40 | u32 size of level 0 |
| 0x44 | u16 width, u16 height |
| 0x48 | u8 log2 width, u8 log2 height, u8 **compression type**, u8 0 |
| 0x4C | u8 0, u8 0, u8 mip level count, u8 0 |
| 0x50 ... 0x7B | flags and scroll/animation constants (0x100 = 1.0 style values), **not interpreted, kept**; 0x60 = 0x01000000, 0x64 = 0x100 in every record |

Compression types seen over the 6,596 textures of the install: `0x22` DXT1 (4,729), `0x24` DXT3 (1,729), `0x08` palettised (122), `0x20` A8R8G8B8 (16). All 689 packs
have the layout described here. Types: `0x22` DXT1, `0x24` DXT3, `0x20` A8R8G8B8 (bytes B,G,R,A), `0x08` palettised. (`0x26` DXT5 and the 16/64 colour palettised
types `0x80`, `0x81` are known from other packs and handled; `0x26` is also what a replaced DXT3 texture becomes.) Palettised textures store one
index byte per pixel for each mip level and a palette of 256 x { B, G, R, A } in the palette pool.

Checked on every texture: `image size` equals the sum of the mip levels computed from width, height, type and mip count; `top size` equals level 0;
mip levels are the sizes `max(1, w >> l)` x `max(1, h >> l)`, blocks of 4x4 for DXT (so a 64x64 texture with 4 levels stops at 8x8).

### 4.3 Format record (`0x33310005`, 32 bytes)

`{ 0, 0, a, 5, b, code, 0, 0 }` with `code` = D3DFMT: `0x31545844` "DXT1", `0x33545844` "DXT3", `21` A8R8G8B8, `41` P8. `a` (0 or 1) and `b` (2 or 6)
are kept as they are; only `code` is rewritten when a texture changes format.

### 4.4 Pixel data (`0x33320002`)

The payload starts with filler bytes `0x11` up to the next 128 byte boundary of the **file** (`base`); `base` is where all placements count from.
Then:

1. the palette pool: the 1024 byte palettes of the palettised textures, in table order (empty when there are none);
2. the images, in table order, each starting on a multiple of 128 from `base` (gaps are zero); the pool is followed by the first image at the next
   multiple of 128. The chunk ends exactly at the end of the last image (no trailing padding).

`placement(i) = align128(placement(i-1) + imagesize(i-1))`, first one `align128(pool size)`. This is the rule the writer regenerates; for all 6,596
textures of the install the regenerated bytes equal the original bytes.

### 4.5 What the writer changes when a texture is replaced

Per replaced texture: dimensions, log2 sizes, compression type, mip count, image size, level 0 size, palette size (0 when a palettised texture
becomes BGRA8), format record `code`. For the pack: all placements and palette placements, the data chunk size (and its leading filler, because the
base depends on where the chunk lands), then every size of the enclosing groups and all padding chunks after it. Textures that are not replaced keep
their bytes. Other chunk payloads are copied unchanged.

Limits: the NFS pack stores 2D textures only; a replacement with layers, faces or depth > 1 is refused. If the game stores file offsets or sizes of
chunks anywhere outside the texture pack (not found in the files examined, but the world files were not decoded) those references are not updated.

## 5. How this was verified

`tests/assets/test_real_files.cpp` (opt-in: `NUE_TEST_GAME_DIR` = the game's data folder; `NUE_TEST_GAME_BIG=1` adds the files above 64 MiB). For each of the
70 `.BUN`/`.LZC` files: opening gives no repair; writing unchanged gives the original bytes; rebuilding from the node tree gives the decoded content;
regenerating all texture tables and pixel layouts from the records gives the decoded content; every texture decodes; the file packed into a `.nuepack`
(source variant) and exported again gives the decoded content; our JDLZ output decodes to the same content. 689 texture packs, 6,596 textures.

## 6. Plugin interface

NFS is a plugin of a generic framework (`nue_asset_format` in `include/nue/assets.h`): `probe`, `open` (reports nodes), `index` (reports resources),
`get_image` / `get_native`, `write`. The framework owns sources, codec wrapping, memory budget, edits and the `.nuepack`. A later game or console adds
a format by registering a `nue_asset_format` (and codecs); `tests/assets/test_nfs.cpp` registers a toy format through the public interface only.
