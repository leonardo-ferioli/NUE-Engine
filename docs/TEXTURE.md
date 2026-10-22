# NUE texture module (`nue/texture.h`, library `nue_texture`)

Status of this document: **design + first block**. The public C API (`include/nue/texture.h`) is written and frozen as draft 1; the third-party codecs
are chosen, pinned and vendored (`THIRD_PARTY_TEXTURE.md`). The implementation (`src/texture/*.cpp`), tests, CLI and benchmarks are the next block;
the *Status* table at the end says exactly what exists. Nothing here has run on a device.

## 1. Why it exists

The recompiled games ship Direct3D 9 textures: DXT1/DXT3/DXT5 (BC1/BC2/BC3), sometimes ATI1/ATI2 (BC4/BC5). Target GPUs:

| Platform | Samples compressed | Does not |
|---|---|---|
| Windows / Linux desktop | BC1-7 | ETC2, ASTC (some iGPUs emulate them) |
| Android (GLES 3 / Vulkan) | ETC2/EAC (always), ASTC LDR (~all current), BC (a few: Tegra, Intel) | BC on most |
| iOS / Apple GPU | ASTC (A8+), PVRTC (all), ETC2 (A8+), HDR ASTC (A13+) | BC |
| Apple silicon Mac | BC + ASTC | PVRTC |
| anything | RGBA8 | |

Today the Direct3D 9 layer (`formats.cpp`: `gpu_format_for`, `decode_dxt`) falls back to **RGBA8** when the GPU lacks BC. That multiplies texture memory
by 8 (DXT1) or 4 (DXT3/5). With the audit numbers from the layer (800-1600 textures, **17-70 MB resident in menus, more in races**) a phone would need
roughly 70 MB x 4..8 = **280-560 MB for the menus alone** instead of 70 MB, which is why a general transcoding layer is needed (the 2 GB process limit on
Android includes everything else the game keeps). The layer's DXT decoder is also a private 3-format implementation; this module replaces it.

## 2. Architecture and justification

```
 source bytes (DXT in D3D9 locks | DDS | KTX2 | PNG)            nue_tex_caps (device)     nue_tex_policy
        |                                                              |                      |
        v                                                              v                      v
  format registry --(nue_tex_plan)--> PLAN: action, target format, swizzle, skip_mips, explanation
        |                                   |
        |            cache hit? (SHA-256 key: source+plan+codec version) --yes--> KTX2 entry --> GPU
        |                                   | no
        v                                   v
  decoders (bcdec | etcpak decode | astcenc | javelin | own)  ->  RGBA8 / RGBA32F  ->  mip generation (only if missing)
                                                                      |
                                              encoders (astcenc | etcpak ETC | etcpak BC1/3/4/5/7 | javelin PVRTC) on the job executor
                                                                      |
                                                              KTX2 cache entry (atomic write) + GPU upload
```

Decisions (each with the reason):

1. **Own thin facade over existing codecs; no monolithic library.** Every required codec exists as a small, permissively licensed, SIMD-optimised
   project (see THIRD_PARTY_TEXTURE.md). Writing codecs ourselves would be slower and worse; adopting bimg or Compressonator wholesale pulls in `bx` /
   Qt / OpenCV-style trees and loses the freedom to pin each codec independently. What is *missing* upstream and written here: format registry and all API
   mappings, capability model, planner, level/image/mip API, DDS and KTX2, cache, importer, budget, metrics, D3D9 seam, and the few decoders no vendored
   project has (ETC2 A1, PVRTC 2 bpp, PVRTC2, ATC).
2. **Decode to RGBA, encode from RGBA.** A direct block-to-block transcode (BC1 -> ETC1 by remapping endpoints) is faster but only exists for a few pairs and
   loses more quality than a proper decode + encode, because ETC2/ASTC can represent more than the 4-colour palette of a DXT block. The planner therefore
   always goes through RGBA8 (RGBA32F for HDR). Quality is checked with PSNR/SSIM/delta-E in the tests.
3. **Planner is pure logic** (`nue_tex_make_plan`): inputs are the source format, device caps, content hints (usage, alpha scan) and a policy; the output is
   deterministic and carries a text explanation. It can be unit tested for every device class without a GPU.
4. **Capabilities are data.** `nue_tex_caps` holds per-format flags (sample, filter, render, sRGB, **emulated**) and limits. Backends fill it from bgfx
   (`nue_tex_caps_from_bgfx` takes `bgfx::Caps::formats`; `BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED` means the driver decompresses at upload, which saves no
   memory, so the planner treats it as unsupported) or from Vulkan/Metal/GL probes (platform sessions call `nue_tex_caps_set_format`). Pure fallback
   tables per device class exist for tests and for the first frame before a probe runs.
5. **Executor with priorities, host-pluggable.** Encoding is parallel over bands of block rows and over textures. `nue_tex_executor` owns worker threads
   (`std::thread`, so the module builds standalone on all four platforms) or forwards jobs to the engine's job system via `desc.submit`. Background import
   runs at LOW priority; a texture about to be drawn is requested at HIGH. (The core thread API in `core.h` is declared but not implemented yet; when it
   lands, workers move onto `nue_thread_create` and `power.h` priorities through that hook without an API change.)
6. **KTX2 as the cache container** (and the exchange format): the format has Vulkan format ids (which we map anyway), a level index and key/value pairs for
   our key and CRC. Entries are written to a temp file and renamed (atomic), keyed by SHA-256, and are safe to delete at any time. Supercompression is not
   used (GPU blocks barely compress; zero-copy reads are worth more).
7. **SIMD**: etcpak and astc-encoder select SIMD at compile time. Default build: SSE4.1 on x86 (2008+), NEON on arm64, scalar elsewhere; options for
   SSE2 / AVX2. The desktop (BC) path does not transcode at all, so the lowest-common-denominator choice costs nothing where speed matters.

## 3. Planner rules (implemented in the next block, specified here)

Hard rules: render targets, dynamic textures (Lock every frame) and `FORCE_UNCOMPRESSED` never get compressed; a format the GPU samples **and** filters
and does not emulate is uploaded as is (`KEEP_SOURCE_FORMAT`). Otherwise, per source family, candidates in order (first one the caps allow):

| Source | Candidates (BALANCED) | Notes |
|---|---|---|
| BC1, opaque content | ETC2 RGB8 (4 bpp) > ASTC at <= 1.1x bpp (6x5 4.27) > RGBA8 / RGB565 | alpha scan: 3-colour blocks mean alpha is used |
| BC1 with punch-through | ASTC 6x5 / 4x4 > ETC2 RGBA8 (8 bpp) > RGBA8 | etcpak has no A1 encoder |
| BC2, BC3 (and DXT2/4) | ASTC 4x4 (same 8 bpp) > ETC2 RGBA8 (EAC) > RGBA8; if the alpha scan says opaque, treat as BC1 (halves memory) | PMA kept as a flag, alpha mode recorded in the plan |
| BC4 | EAC R11 > ASTC (R,R,R,1 swizzle) > R8 | |
| BC5 (normal maps) | EAC RG11 > ASTC 4x4 with normal-map mode (`rrrg`, needs shader swizzle unless NO_SHADER_SWIZZLE) > RG8 | |
| BC7 | ASTC 4x4 > ETC2 RGBA8 > RGBA8 | |
| BC6H, ASTC HDR | ASTC HDR (if caps) > RGB9E5 / RG11B10 / RGBA16F > RGBA8 (precision loss flag) | |
| ETC / ASTC / PVRTC on a BC-only device | BC7 (QUALITY) / BC1 or BC3 (others) | assets authored for mobile played on desktop |
| uncompressed | unchanged, or compressed to the best supported format when `COMPRESS_RAW` and the longest side >= 16 | |

ASTC block size is chosen by bits per pixel: `target_bpp <= source_bpp x tolerance` (QUALITY 2.0, BALANCED 1.1, MEMORY 0.6); PRIORITY_SPEED prefers ETC2 where it
exists. Small textures (< `min_recompress_dimension`) become RGBA8: not worth encoding, negligible memory. `max_dimension` drops top mips.

Memory per texel for the estimate (`nue_tex_memory_estimate`): BC1/ETC2 RGB 0.5 B, BC2/3/7/ETC2 RGBA/ASTC 4x4 1 B, ASTC 6x6 0.445 B, RGBA8 4 B,
RGB565 2 B, plus 1/3 for the mip chain. Savings versus today's RGBA8 fallback: DXT1 -> ETC2 8x, DXT5 -> ASTC 4x4 / ETC2 RGBA 4x, to ASTC 6x6 9x.

## 4. Integration with the Direct3D 9 layer (read-only analysis of `claude/d3d9-bgfx-d42a87`, no edits made)

What the layer does today (`nue_d3d9/src/layer/layer_resources.cpp`, `formats.cpp`):

* `create_texture` fixes the GPU format once: `gpu_format_for(info, caps)` (native if the GPU has it, else RGBA8) and creates **all mip levels up front**.
* The game fills levels with `Lock`/`Unlock` (`lock_texture` gives a staging buffer in the *D3D9* format; `unlock_texture` -> `upload_level_region`),
  or through `UpdateTexture` from a system-memory copy. Partial rectangles are possible. The layer keeps a copy of each level in the game's format
  (`retain_copy`, budgeted by `texture_copy_mb`) so a later Lock can read it back.
* `convert_pixels` converts per level / rectangle; DXT goes through the private `decode_dxt`.

Proposed patch for the layer owners (the texture module supplies the three calls in `nue/texture.h`, section *Direct3D 9 seam*):

1. Hold a `nue_tex_context` (created once from the `nue_gfx` caps via `nue_tex_caps_from_bgfx`).
2. In `create_texture` / `create_cube_texture`: replace `gpu_format_for` by `nue_tex_d3d9_texture_create(ctx, {d3d_format,w,h,levels,usage,pool,cube,srgb}, &tex, &info, &plan)`;
   create the bgfx texture with `info.gpu_format` (`nue_tex_format_to_bgfx`), `info.gpu_levels`, size `info.gpu_width x gpu_height`. Render targets and
   depth formats keep their own path (the call returns `UPLOAD_AS_IS`/`CONVERT_PIXELS` for them).
3. In `upload_level_region` replace `convert_pixels`/`decode_dxt` by `nue_tex_d3d9_convert_level(tex, level, face, rect, src, pitch, dst, cap, &pitch_out, &size)`.
   Sub-rectangles must be block aligned (as D3D9 itself requires for DXT locks); non-DXT formats convert any rectangle.
4. Slow encodes (`info.defer_to_worker`: ASTC/BC7 of a big level): upload a 1-colour placeholder (average colour) and replace it from the
   `nue_tex_request_async` callback; ETC2/BC1 encodes are fast enough (etcpak: hundreds of MPixel/s multithreaded) to stay inline.
5. `texture_report` gains the plan explanation per texture (`nue_tex_plan.explanation`) so the existing live audit shows which format each texture got.

Cost model to be measured in the next block (benchmarks in `nue_texconv bench`): per-texture inline cost = decode (bcdec ~ GPixel/s class) + encode
(etcpak ETC2 / BC1, ASTC at `FAST` preset); per-frame cost zero once uploaded. Run-time transcoding of a whole game is avoided by the first-run import:
all textures are converted once into the cache (KTX2, hash keyed), later runs only read them.

## 5. Format ids and mappings

`texture.h` defines ~130 formats with explicit numeric ids. Mapping tables to/from D3D9 (D3DFMT + FOURCC), DXGI, VkFormat (= KTX2), MTLPixelFormat,
OpenGL(ES) internal formats and bgfx::TextureFormat (105 ids, order verified against `bgfx.h` in `third_party/bgfx.cmake` and bimg) are specified in the
header; the numeric tables were written from the public headers and **must be cross-checked against the SDK headers on first use on each platform**
(Metal and GL numbers especially, until a platform session verifies them in its own build).

## 6. Status (honest)

| Item | State |
|---|---|
| Research of existing libraries, licences, speed, gaps | done (THIRD_PARTY_TEXTURE.md) |
| Public C API draft (`include/nue/texture.h`) | written; compiles clean as C99 and C++17 (`-Wall -Wextra -pedantic`, syntax check) |
| Third-party codecs vendored + pinned | done (copied; **not yet built** inside the module) |
| Format registry, mappings, caps tables, planner | not started (specified above) |
| Codec glue, mips, metrics, DDS/KTX2/PNG | not started |
| Cache, importer, async path, budget | not started |
| `nue_texconv`, tests, benchmarks | not started |
| D3D9 seam implementation | API drafted; analysis of the layer done |

Toolchain facts verified: clang 23 + Ninja build C++20 with `<bit>` (needed by etcpak's `bc7enc.cpp`); astc-encoder needs `-msse4.1 -mpopcnt` and the
`ASTCENC_*` defines listed in its `cmake_core.cmake` (copied into this module's CMake in the next block).

## 7. Requests to other owners

* **nue-core**: add `#include "nue/texture.h"` to `nue.h` and `src/texture` to the root CMake (`add_subdirectory(src/texture)`, target `nue_texture`, linked into
  `nue`); optional `include/nuepp/texture.hpp` will follow from this module.
* **d3d9 layer owners**: section 4 above.
* **Platform sessions (android, ios)**: fill `nue_tex_caps` from real Vulkan / GL / Metal probes (`nue_tex_caps_set_format`) and report device classes
  that do not match the presets in `nue_tex_device_class`.
