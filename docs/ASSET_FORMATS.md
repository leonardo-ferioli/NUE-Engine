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
| `0x80134000` ... `0x00134B03` | solid lists and solids = **geometry** of the cars (`GEOMETRY.BIN`) and of the city (`STREAM*.BUN`). Section 5 |
| `0x00030201`, `0x00030203`, `0x00030210`, `0x00034xxx`, `0x00035xxx`, `0x00036xxx`, `0x00037xxx`, `0x00039xxx`, `0x0003Bxxx`, `0x00E34xxx` | game tables and scripts (physics, collision, animation, sound ...). **Not interpreted** |
| `0` | padding |

The tool `nue_pack list FILE --nodes` prints the tree of any file. Census of the city files: `STREAM*.BUN` hold solids (section 5), texture packs (section 4)
and a few groups of tables (`0x80034100`, `0x80034130`, `0x80034150` ...) that are not interpreted; `L4R*.BUN` hold no geometry or textures, only tables
(`0x00039200`-`0x00039202`, `0x00037080`, `0x00037090`, `0x00037110`, `0x00037150`, `0x0003B6xx`, `0x00E34010` ...: most likely the scenery and track
definitions that place the solids of the streaming files). Those tables are kept as opaque chunks and round trip byte for byte; how an object of the city is
placed from them is **not** established here (the solids come out with the matrix of their own header, section 5).

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

### 4.6 Car texture packs (`TEXTURES.BIN`, `VINYLS.BIN`)

The same chunk skeleton, but the textures are separate compressed blobs instead of one pixel block:

```
0xB3300000
  0xB3310000
    0x33310001  pack header (as 4.1)
    0x33310002  N x { u32 name hash, u32 0 }
    0x33310003  N x descriptor (24 bytes)
  0xB3320000
    0x33320001  24 bytes
    0x33320002  the blobs
```

Descriptor: `u32 name hash, u32 file offset of the blob (absolute), u32 blob size, u32 decoded size, u32 header distance (0x100), u32 0`. Each blob starts with
the magic `JDLZ` (section 2) or `HUFF` (section 8) and decodes to `decoded size` bytes. The decoded buffer holds the pixels from byte 0 (all mip levels, as
4.4), and the 0x7C byte **texture record of 4.2** at `decoded size - header distance + 0x64`. The record's hash must equal the descriptor's (a texture where it
does not is left out of the list). Palettised textures keep the 1024 byte palette inside the same buffer, at `palette placement - image placement` (both from
the record). Width, height, type, mip count and sizes are validated exactly as in 4.2.

Seen: 2,186 textures in the `TEXTURES.BIN` files of 57 car folders decode (all 231 `.BIN` files of `CARS` open, rebuild and decode without a failure). The pack
is **read only**: replacing a texture is refused (`NUE_ASSET_RESFLAG_READONLY`), because the writer does not rebuild this layout; its chunks are copied as they
are. Empty `TEXTURES.BIN` files (the traffic cars share textures) are valid empty containers.

## 5. Solids: geometry of cars and city

```
0x80134010  solid
  0x00134011  header
  0x00134012  material slots: N x { u32 hash, u32 0 }
  0x00134013  shader slots, same layout
  0x80134100  mesh
    0x00134900  mesh header
    0x00134B01  vertices
    0x00134B02  material runs
    0x00134B03  indices
```

Cars: one `0x80134000` list per `GEOMETRY.BIN` with a solid per part and level of detail (`240SX_BASE_A`, `_B`, `_C`, ... = most to least detailed). City:
`STREAM*.BUN` hold 1,000 to 10,000 solids each. Solids without a mesh group (mount points) are not meshes.

- **Solid header** (192 bytes, preceded by 0, 4, 8 or 12 bytes of `0x11` filler that shift every offset below; the filler length is the size of the leading run of
  `0x11111111` words): `+0x20` bounding box minimum, `+0x30` maximum (three f32 each), `+0x40` a 4x4 f32 matrix, row-major with the translation in the last
  row (row vectors, Direct3D style), `+0xA4` the NUL terminated name. The matrix is the identity for most car parts and the **world placement** of the object
  for city solids (2,067 of the 2,642 solids of `STREAML4RB.BUN`).
- **Mesh header** (68 bytes, also possibly behind filler words): u32 field 3 = vertex format flags (`0x80`: the vertex has a normal), field 9 = triangle count,
  field 13 = vertex count; field 2 is 0x10, the rest zero or not understood.
- **Vertices**: `0x11` filler, then the vertices occupy the LAST `count x stride` bytes. Stride 36 (flag `0x80`, cars): position f32 x3, normal f32 x3, colour
  B,G,R,A, uv f32 x2. Stride 24 (no flag, city): position, colour, uv. The flag decides, not the size of the buffer: small meshes behind much filler would
  fit either stride. The colour is vertex lighting in the city; in the cars it is a constant.
- **Indices**: `0x11` filler, then `3 x triangles` u16 (triangle list), read forward from the end of the filler; a couple of padding bytes may follow.
- **Material runs**: filler, then N entries of 60 bytes at the end of the chunk: u32 at `+12` = index count, `+28` = slot in `0x00134012`, `+32` = slot in
  `0x00134013`, `+52` = first index. The runs tile the index buffer. A run's material hash is a texture (the name hash of a texture record, searched in the packs
  of the car and in the global packs) or a material/shader definition that is not a texture (paint ...): of the 46 distinct hashes of the 240SX, 18 are textures
  of its own `TEXTURES.BIN` plus `GlobalB.lzc`.

Checked on the files: every vertex buffer holds `count x stride` bytes, every index is below the vertex count and no vertex is outside +-1e6, in all
18,824 meshes of the car folders (4.1 M vertices, 3.2 M triangles) and the 7,098 meshes of the files `STREAML4RB/C/F/G/H/R.BUN` (`STREAML4RA/D.BUN` too with
`NUE_TEST_GAME_BIG`). Winding: of 487 car meshes with more than 50 triangles, 466 enclose a positive volume when the coordinates are read as right handed
(counter clockwise outwards), so triangles are exported as they are. Axes: X length, Y width, Z up.

Not understood: the tables that say where each city solid sits in a track (census in section 3) and the meaning of the unused header fields.

### 5.1 Model export

`nue_pack extract FILE-OR-CAR-FOLDER [more files] --model -o DIR [--format glb|obj] [--best-lod] [--filter TEXT] [--no-textures] [--keep-z-up]`
(`nue_asset_export_model` in the library). A folder gives `GEOMETRY.BIN` and `TEXTURES.BIN` (not `VINYLS.BIN`); extra files (`GLOBAL/GlobalB.lzc`) add
textures that the parts share. glTF 2.0 binary: one node per solid with its matrix, one primitive per material run, POSITION / NORMAL / TEXCOORD_0 and COLOR_0
(only when the colours vary), textures as embedded PNG on materials by hash (`BLEND` when the texture has alpha), a root node that turns Z up into Y up.
OBJ: positions with the matrix applied, `vt` with V flipped, an MTL and a folder of PNG next to the file. `--best-lod` keeps the lowest letter of each
`NAME_A`..`_D` family. Verified with a structural check of the glTF (accessors inside their views, indices below the vertex count, PNG signatures, min/max)
on `240SX.glb`, `STREAML4RH.glb` and a 76 MB `STREAML4RB.glb`, and by the tests. It has not been loaded in a third party viewer in this session.

## 6. How this was verified

`tests/assets/test_real_files.cpp` (opt-in: `NUE_TEST_GAME_DIR` = the game's data folder; `NUE_TEST_GAME_BIG=1` adds the files above 64 MiB). For each of the
70 `.BUN`/`.LZC` files: opening gives no repair; writing unchanged gives the original bytes; rebuilding from the node tree gives the decoded content;
regenerating all texture tables and pixel layouts from the records gives the decoded content; every texture decodes; the file packed into a `.nuepack`
(source variant) and exported again gives the decoded content; our JDLZ output decodes to the same content. 689 texture packs, 6,596 textures. A second test walks the 57 car folders and the city files: every mesh and texture of `GEOMETRY.BIN`,
`TEXTURES.BIN` (and `VINYLS.BIN` with `NUE_TEST_GAME_BIG`) reads, and the files write back unchanged and rebuilt. `nue_pack verify` over all 231 `.BIN` files of
`CARS` reports no failure.

## 7. Plugin interface

NFS is a plugin of a generic framework (`nue_asset_format` in `include/nue/assets.h`): `probe`, `open` (reports nodes), `index` (reports resources),
`get_image` / `get_native`, `write`. The framework owns sources, codec wrapping, memory budget, edits and the `.nuepack`. A later game or console adds
a format by registering a `nue_asset_format` (and codecs); `tests/assets/test_nfs.cpp` registers a toy format through the public interface only.

## 8. HUFF

Some car textures use EA's Huffman codec (magic `HUFF`, `src/assets/huff.cpp`, decoder only).

```
0    "HUFF"
4    u8 version (1), u8 header size (0x10), u16 flags
8    u32 size of the decoded data
12   u32 size of the stream WITHOUT this header
16   bit stream, most significant bit first
```

Bit stream: a 16 bit type word (all real files: `0x30FB`; bit 15 = 32 bit sizes follow instead of 24 bit; bit 8 = two extra words to skip), the decoded size
(24 or 32 bits), an 8 bit *clue* symbol, the **code lengths** (for length 1, 2, 3 ... a number saying how many symbols have that length, until the canonical code
is complete), the **symbols** (per code in order, the byte as a leap over the not yet used byte values) and the **codes**. A number is `p` zero bits, a one bit,
then `p + 2` value bits `v`, read as `v + 2^(p+2) - 4`. The clue symbol escapes: a number `n > 0` repeats the previous byte `n` times; `0` then bit 1 ends the
stream; `0` then bit 0 is followed by a raw byte. Types `0x32FB`/`0xB2FB` and `0x34FB`/`0xB4FB` ask for one or two running-sum passes over the output (never
seen in the game files, transcribed from the reference). Validated on the real files: all 2,186 car textures pass the record hash self check after decoding.
The decoder is strict about malformed streams (bit-flip fuzz in the tests); the tests also decode streams made by a small encoder in the test.
