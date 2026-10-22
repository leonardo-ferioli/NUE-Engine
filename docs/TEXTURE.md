# NUE texture module (`nue/texture.h`, library `nue_texture`)

Status: **implemented and tested on Windows x64 (clang, SSE4.1), console only.** Nothing has run on Android, iOS, Linux or a real GPU yet; the
numbers below were measured on one machine (AMD Ryzen 7 "Surface Edition", 8 cores / 16 threads). Section 9 lists exactly what is verified and what is not.

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
roughly 70 MB x 4..8 = **280-560 MB for the menus alone** instead of about 70 MB. The layer's DXT decoder is also a private 3-format implementation;
this module replaces it.

## 2. Architecture and why

```
 source bytes (DXT in D3D9 locks | DDS | KTX2 | PNG)         nue_tex_caps (device)      nue_tex_policy
        |                                                           |                       |
        v                                                           v                       v
  format registry --(nue_tex_make_plan)--> PLAN: action, target format, shader swizzle, skip_mips, flags, explanation
        |                                       |
        |        cache hit? (SHA-256: source + plan + params + codec version) --yes--> KTX2 entry --> GPU
        |                                       | no
        v                                       v
  decoders: bcdec | etcpak | astcenc | Javelin -> RGBA8 / RGBA32F -> mips (only where missing) -> encoders: astcenc | etcpak | bc7enc | Javelin
                                                          (all on the job executor)           -> KTX2 cache entry (atomic write) + GPU upload
```

1. **Own thin facade over existing codecs; no monolithic library.** Every codec is a small permissively licensed, SIMD-optimised project
   (`THIRD_PARTY_TEXTURE.md` has the evaluation of 12 candidates). Written here: registry and API mappings, capability model, planner, level/image/mip API,
   DDS and KTX2, cache, importer, budget, metrics, D3D9 seam.
2. **Decode to RGBA, encode from RGBA.** Direct block-to-block transcoding exists only for a few pairs and loses more quality than a proper
   decode + encode (ETC2/ASTC represent more than the 4-colour palette of a DXT block). Measured: BC7 -> ASTC 6x5 31.1 dB, BC7 -> BC3 26.9 dB, BC7 -> ETC2 RGBA 27.0 dB
   against the original on the test image (`test_pipeline`).
3. **The planner is pure logic** and deterministic; its explanation is a text log, so every texture can say why it got its format.
4. **Capabilities are data**: `nue_tex_caps` (per-format flags: sample, filter, render, sRGB, **emulated**; limits). Filled from bgfx (`nue_tex_caps_from_bgfx`),
   from the engine GPU probe (`nue_tex_caps_from_gpu_caps`, `gfx.h`), from Vulkan/Metal/GL probes (`nue_tex_caps_set_format`), or from the fallback table
   per device class. A format the driver emulates (decompresses at upload) is not trusted: it saves no memory.
5. **Executor with priorities, host-pluggable** (`nue_tex_executor`): own `std::thread` workers or `desc.submit` into the engine job system.
   Nested use is safe: a band of block rows is claimed by whoever is free, so a worker that calls the codec never deadlocks.
6. **KTX2 as cache container**: vkFormat is authoritative, our key and payload CRC live in the key/value block, entries are written to a temp file and renamed.
7. **SIMD** is chosen at build time (`NUE_TEXTURE_SIMD=auto|none|sse2|sse41|avx2|neon`, default SSE4.1 on x86, NEON on arm64): etcpak and astc-encoder select it at compile time.
   Desktop GPUs sample BC directly, so the lowest common denominator costs nothing where speed matters.

## 3. Build and layout

* `src/texture/*.cpp` (module), `src/texture/third_party/<name>/` (vendored codecs, own folder and licence each), `tests/texture/`, `tools/nue_texconv/`.
* Standalone (no SDL, no engine): `cmake -S src/texture -B build_tex -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build build_tex && ctest --test-dir build_tex`.
* Inside the engine the same file follows the root contract (`nue_texture_obj` sources; the vendored codec libraries are forwarded to `libnue`).

## 4. Public API in one page (`include/nue/texture.h`, 108 functions)

| Group | Functions |
|---|---|
| formats | `nue_tex_get_format_info`, `_format_name/_from_name/_list/_is_compressed/_to_srgb/_to_linear`; geometry `_row_pitch/_rows/_level_size/_chain_size/_mip_count/_mip_extent`; mappings `_format_from/to_d3d9/dxgi/vk/metal/gl/bgfx`, `nue_tex_d3d9_format_table` (D3DFMT/FOURCC as data) |
| capabilities | `nue_tex_caps_for_class`, `_clear/_set_format/_get_format/_can_use/_from_bgfx/_from_gpu_caps/_to_string` |
| planner | `nue_tex_make_plan(caps, request, plan)` + `nue_tex_policy` |
| images/files | `nue_tex_image_create/_get_level/_clone/_convert/_drop_top_mips`, `nue_tex_generate_mips`, `_image_load/save_memory/file` for DDS, KTX2, PNG |
| codecs | `nue_tex_decode_level`, `_encode_level`, `_convert_level` (any format to any format, one level), `_unpack/_pack_pixels`, `_scan_level` |
| metrics | `nue_tex_compare_level/_images`: PSNR (RGB, alpha), SSIM, CIEDE2000, max error |
| execution | `nue_tex_executor_*` (priorities LOW..CRITICAL, host submit hook) |
| cache/import | `nue_tex_context_*`, `nue_tex_context_convert`, `nue_tex_request_async/_cancel`, `nue_tex_cache_*`, `nue_tex_importer_*` |
| budget | `nue_tex_budget_*` (policy, LRU eviction hook, statistics), `nue_tex_memory_estimate` |
| D3D9 seam | `nue_tex_d3d9_texture_create/_convert_level/_level_gpu_size/_texture_info_get/_texture_destroy` |

## 5. Rules that matter

**Colour space.** 8-bit LDR conversion never applies a curve: `convert_level` treats the bytes as already encoded for the destination (BC1_UNORM bytes into
BC1_SRGB are the same bytes). The planner selects the sRGB twin only when the content is sRGB (D3D9 `SRGBTEXTURE` seen, or the source format is sRGB) *and* the
device samples it (`NUE_TEX_FCAP_SRGB_SAMPLE`); otherwise it picks the linear format and sets `NUE_TEX_PLAN_SRGB_IN_SHADER`. Float intermediates are linear; sRGB sources are linearised
when decoded to RGBA32F and re-encoded when packed to sRGB. Mip filtering in linear light is opt-in (`NUE_TEX_MIP_SRGB`).

**Signed data** (V8U8, BC4/BC5 SNORM, EAC SNORM) is stored biased (`s*0.5+0.5`) in the RGBA8 intermediate. **Alpha**: DXT2/DXT4 (premultiplied) keep their data;
the alpha mode is recorded in the registry.

**Normal maps** (`NUE_TEX_USAGE_NORMAL_MAP`): BC5 -> EAC RG11 (no shader change). ASTC is only used with the swizzle `rrrg` (the plan reports it and sets `NUE_TEX_PLAN_NEEDS_SWIZZLE`);
`NUE_TEX_POLICY_NO_SHADER_SWIZZLE` forbids it.

## 6. Planner rules (all covered by `test_planner`)

Hard rules: render targets and dynamic textures never get a compressed format; a format that the GPU samples and filters and does not emulate is uploaded as is.
Otherwise the candidates per source family, first one the caps allow and an encoder exists for:

| Source | BALANCED | Notes |
|---|---|---|
| BC1 opaque | ETC2 RGB8 (4 bpp) > BC1 > ASTC <= 1.1x bpp > PVRTC1 4bpp | alpha scan decides opaque |
| BC1 with 1-bit alpha | ASTC (6x5, 4.27 bpp) > BC7 > ETC2 RGBA8 (8 bpp) > BC3 | etcpak has no ETC2 RGB-A1 encoder |
| BC2/BC3/BC7 | ASTC 4x4 > BC7 > ETC2 RGBA8 > BC3 | scan says opaque => treated as BC1 (half the memory) |
| BC4 / BC5 | EAC R11 / EAC RG11 > BC4 / BC5 > ASTC | |
| HDR (BC6H, ASTC HDR, floats) | ASTC HDR (if caps) > RG11B10F > RGB9E5 > RGBA16F > RGBA8 | |
| ETC/ASTC/PVRTC on a BC device | BC7 (QUALITY) / BC1 / BC3 | mobile content on desktop |
| uncompressed | unchanged; compressed only with `COMPRESS_RAW` | |

Priorities: QUALITY allows 2x the memory (ASTC 4x4 for DXT1), MEMORY 0.6x, SPEED prefers the cheap encoder. `max_dimension` drops top mips. Textures under 16 texels become RGBA8.
If nothing fits, the fallback is uncompressed (RGBA8, or RG11B10F for HDR), flagged `NUE_TEX_PLAN_FALLBACK`.

## 7. Measured performance (Release, clang 23, SSE4.1, 1024x1024 synthetic image, one machine)

Encode, MPixel/s by thread count (`nue_texconv bench`, includes the calling thread). PSNR is on this smooth synthetic image (real textures give lower numbers):

| Encoder | PSNR | 1 | 2 | 4 | 8 | 16 |
|---|---|---|---|---|---|---|
| BC1 | 38.1 | 251 | 293 | 320 | 335 | 314 |
| BC3 | 38.0 | 210 | 242 | 380 | 349 | 319 |
| BC4 / BC5 | 49 | 259 / 218 | 258 / 223 | 341 / 318 | 373 / 341 | 303 / 315 |
| BC7 fastest / normal | 47.4 / 47.6 | 9.6 / 1.6 | 18 / 3.0 | 31 / 5.7 | 48 / 9.2 | 61 / 13.5 |
| ETC1 | 39.0 | 84 | 136 | 193 | 238 | 241 |
| ETC2 RGB fast / normal | 33.9 / 38.8 | 130 / 50 | 189 / 88 | 256 / 139 | 277 / 181 | 270 / 197 |
| ETC2 RGBA normal | 38.8 | 35 | 61 | 100 | 126 | 153 |
| EAC R11 / RG11 | 50 / 50 | 63 / 38 | 108 / 60 | 161 / 105 | 189 / 129 | 216 / 160 |
| ASTC 4x4 fastest / fast / normal (30) | 47 | 7.2 / 6.3 / 2.1 | 14 / 11 / - | 25 / 21 / - | 35 / 28 / - | 42 / 37 / - |
| ASTC 6x6 fast / 8x8 fast | 39 / 35 | 3.8 / 8.2 | 6.9 / 16 | 12 / 25 | 17 / 36 | 23 / 47 |
| PVRTC1 4bpp | 32.1 | 11 (single thread codec) | | | | |

Decode, one thread, MPixel/s: BC1 245, BC3 188, BC4 137, BC5 119, BC7 58, ETC1/2 ~130, ASTC 33-42 (4x4), 42-51 (6x6/8x8), PVRTC 25.

Consequences: a DXT1/DXT3/DXT5 -> ETC2 transcode costs ~(1/245 + 1/50) s per MPixel single threaded (about 25 ms for a 512x512 level), the ASTC 4x4 "fast" path ~170 ms per
512x512 level on one core. A scene of 1500 textures averaging 256x256 (98 MPixel with mips) needs about 2 s on 16 threads for ETC2 and about 15 s for ASTC fast, which is why the first-run
import and the cache exist: nothing of that happens during a race. On a phone core expect 3-6x slower than this desktop core (**not measured**).

## 8. Memory (budget API, model, D3D9 layer numbers)

* **No built-in limit.** `nue_tex_budget_policy`: unlimited | bytes | fraction of the available memory the host reports | preset (`LOW_MEMORY_MOBILE` 12 % with a 64 MiB floor, `MOBILE` 20 %,
  `DESKTOP` 35 %, `HIGH_END` 50 %), min/max clamps, changeable at run time; eviction callback receives LRU-ordered candidates (pinned resources never); statistics. A game that wants a fixed ceiling configures that number.
  The engine-wide manager (`nue_budget_*`, `memory.h`, core session) owns the policy defaults per platform; this module's budget is the texture-local LRU accounting that can sit behind it.
* Model (`nue_tex_memory_estimate`, `test_pipeline`): 100 DXT1 512x512 + 200 DXT5 256x256 textures with full mips = 33.3 MB as shipped; 200.0 MB as RGBA8 (6.0x, the current D3D9 layer
  fallback); 22.8 MB as ASTC 6x6. Per texel: BC1/ETC2 RGB 0.5 B, BC2/3/7/ETC2 RGBA/ASTC 4x4 1 B, ASTC 6x6 0.45 B, RGBA8 4 B, RGB565 2 B, plus one third for the mip chain.
  The real texture mix per scene of the game is **not known to this session** (the layer's audit says 800-1600 textures, 17-70 MB in menus; races unknown); `nue_texconv scan <dir>` prints the totals for a directory of DDS/KTX2/PNG files once
  the textures are extracted.

## 9. Integration with the Direct3D 9 layer (read-only analysis of `claude/d3d9-bgfx-d42a87`, no edits made)

What the layer does today (`nue_d3d9/src/layer/layer_resources.cpp`, `formats.cpp`): `create_texture` fixes the GPU format once (`gpu_format_for`) and creates all mip levels up front; the game fills levels with
`Lock`/`Unlock` (`unlock_texture` -> `upload_level_region`) or `UpdateTexture`; the layer keeps a copy per level for later reads.

Patch proposal (the legacy layer owners apply it; the data table `nue_tex_d3d9_format_table` removes the layer's own format knowledge):

1. One `nue_tex_context` from the live `nue_gpu_caps` (`nue_tex_caps_from_gpu_caps`) or bgfx caps (`nue_tex_caps_from_bgfx`).
2. `create_texture`/`create_cube_texture`: `nue_tex_d3d9_texture_create(ctx, {d3d_format, w, h, levels, usage, pool, cube, srgb, content, alpha_hint}, &tex, &info, &plan)`; create the bgfx texture with
   `info.gpu_format` (`nue_tex_format_to_bgfx`, srgb flag), `info.gpu_levels`, `info.gpu_width x gpu_height`. Render targets, depth and system-memory textures come back as `UPLOAD_AS_IS` (the layer keeps its path).
3. `upload_level_region`: replace `convert_pixels`/`decode_dxt` with `nue_tex_d3d9_convert_level(tex, level, face, rect, src, pitch, dst, capacity, &pitch_out, &size)`. Source rectangles must be block aligned (as D3D9 itself requires for DXT locks);
   a partial rectangle that does not start on a *destination* block boundary returns `NUE_ERR_UNSUPPORTED` (send the whole level). Levels below `info.skip_mips` return `NUE_OK_NOOP`.
4. `info.defer_to_worker`: slow encodes (ASTC/BC7 of large levels): upload a placeholder and replace it from `nue_tex_request_async`; ETC2/BC1 are fast enough inline.
5. `alpha_hint`: DXT1 with unknown alpha plans an alpha-capable target (ASTC 6x5), DXT1 known opaque plans ETC2 RGB8 (half the size of the alternative). The layer knows more than the module: pass what it saw.
6. `plan.explanation` per texture belongs in the layer's texture report.

Measured seam cost (`test_pipeline`, Release, one 128x128 DXT1 level -> ETC2): well under a millisecond; a whole-game cost is the import throughput in section 7.

## 10. Status

| Item | State |
|---|---|
| Format registry (130 formats), geometry, 8 mapping tables, D3D9 table as data | implemented, tested for internal consistency; the **numeric ids of Metal/GL/Vulkan were written from public headers and are not verified against the SDKs** |
| Caps model, 9 device-class presets, bgfx and engine GPU-probe adapters | implemented, tested |
| Planner | implemented, 134 checks over desktop/Android/Apple/software classes |
| Codecs | BC1/2/3/4/5/6H/7 decode; ETC1/ETC2 RGB/RGBA8/EAC R11/RG11 decode; ASTC LDR/HDR (all 14 block sizes) decode + encode; PVRTC1 4bpp decode + encode (square power-of-two only); encoders BC1/3/4/5/7, ETC1/ETC2/EAC R/RG |
| **Gaps** | **decoders missing for ETC2 RGB-A1, PVRTC1 2bpp, PVRTC2, ATC (registry flags say so; the planner never plans to them); encoders missing for BC2, ETC2 RGB-A1, PVRTC 2bpp/PVRTC2 (no open encoder found); KTX2 supercompression (zstd/BasisLZ) refused, not misread** |
| DDS, KTX2, PNG read, PNG write | implemented, round trips for mips/cube/array/volume, truncation fuzz, hand-built legacy D3D9 headers |
| Mip generation | box (exact), triangle, Kaiser, Lanczos; linear-light; normal renorm; alpha coverage; wrap; premultiplied; 3D |
| Cache | hit/miss/corrupt/trim/clear tested; LRU by file time; atomic writes. Not tested: concurrent processes sharing one directory |
| Import pipeline, async requests | tested (resume via cache, pause, cancel, failures) |
| Budget | tested (LRU, pinned, run-time policy change, presets) |
| D3D9 seam | tested against synthetic DXT data; **not wired into nue_d3d9** (other owner) |
| nue_texconv | info/convert/compare/plan/scan/bench/formats/caps work; `scan` has not seen real game textures |
| Real game textures | **not tested**: the game's textures live in its own archives; an extraction or a DDS dump from the d3d9 layer is needed (`nue_texconv scan <dir>` then runs the planner and the totals) |
| Other platforms | only built and run on Windows x64 here; Linux/Android/iOS builds are CI's job, NEON path unexercised |

## 11. Requests to other owners

* **Legacy D3D9 owners**: section 9.
* **Platform sessions**: fill `nue_gpu_caps.format[]` honestly (also whether a family is merely emulated, which the probe struct cannot say: a `NUE_FMTSUP_EMULATED` bit would let the planner avoid fake support).
* **Core**: nothing pending; the root contract (`nue_texture_obj`) is used as documented in `docs/REPLIES_core.md`.
