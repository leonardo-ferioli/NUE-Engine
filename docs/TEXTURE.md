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

## 2b. Cold conversion (the rule): nothing is encoded while the game runs

Decision (user): conversion happens **cold**, ahead of play, and produces **all formats at once**. Run time only looks things up.

* **Cook.** `nue_tex_importer_set_classes(imp, NUE_TEX_CLASS_MASK_ALL)` + the import pipeline (or `nue_tex_cook_image`, or `nue_texconv cook <textures> <cache>`) converts each texture for every device class
  in one pass: the planner is run for each class, the distinct lossy targets are deduplicated, and each one is encoded once and stored. Example (test_cold, three textures): ETC2 RGB, ETC2 RGBA8, EAC RG11,
  ASTC 4x4 and 6x5 (linear and sRGB) and PVRTC1 4bpp. Desktop BC, as-is and plain-decode targets need no file (the source bytes are used or decoded cheaply).
  The planner is also run for the cases the D3D9 layer can ask for: alpha known or unknown, sRGB or linear (ETC2/BC/PVRTC twins share their bytes, ASTC is cooked for both).
* **Device independent, movable.** A cache entry is keyed by the source content and the target format, never by the device or a path: cook once on a PC (all classes), copy the folder to the phone, done
  (`a_cache_can_be_copied_to_another_machine`). Resumable: variants already present are skipped, so an interrupted cook continues. Safe to delete.
* **Lookup by content.** Every entry records the hash of the source level behind each stored level. The context indexes the folder when it is created. `nue_tex_d3d9_convert_level` hashes the bytes the game
  uploads (level format, size and data) and copies the cooked level: `NUE_OK`. Hash + copy costs a SHA-256 pass over the level (a few hundred MB/s) plus a memcpy: no codec runs.
* **Misses never encode.** A level that is not in the cache (a texture the game builds itself, a partial rectangle, a new asset not yet cooked) returns `NUE_OK_PARTIAL` with a zero-block placeholder and is counted
  (`nue_tex_runtime_stats.level_misses`, logged); `nue_tex_context_convert` / `nue_tex_request_async` return `NUE_ERR_NOT_FOUND`. `nue_tex_runtime_stats.runtime_encodes` stays 0.
  Plain decodes to RGBA8 (cheap, 100+ MPixel/s) remain allowed as a fallback when the device cannot use the source format.
* **Development switch.** `NUE_TEX_CONTEXT_ALLOW_RUNTIME_ENCODE` (context flag) restores on-the-spot encoding for tools and tests.
* **Cost model.** Cook time = the encode throughput of section 7 times the number of distinct targets (about 3-6 per texture). 1500 textures of 256x256 with mips (about 98 MPixel) cost roughly 2 s of ETC2 plus 15 s of ASTC fast on 16
  threads on the measured machine, times the number of variants; on a phone, cook on a PC instead. Disk: one KTX2 file per (texture, target); a variant is about as large as the DXT source (ETC2/ASTC 4x4) or smaller (ASTC 6x5).
* **Caveat for the integration.** The lookup works only if the level bytes the game uploads are the bytes that were cooked (DXT data coming straight from the archive). If the game decompresses or edits a texture
  before `UnlockRect`, that texture misses; `level_misses` and the log make this visible on the first run.

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

## 7b. Results on the real NFSU2 textures (2026-10-22)

Source: 6119 DDS files (164 MB) extracted by the assets session from every `.BUN/.LZC` pack (GLOBAL 503, FRONTEND 499, NIS 183, TRACKS 4934; the duplicate GlobalB/InGameCommon folders excluded). **They are every texture of the
game, not one scene.** Nothing of the game is stored in the repository; the files stay local. Formats found: DXT1 (BC1) 4544, DXT3 (BC2) 1460, BGRA8 115 (+ expanded palettised), no DXT5, no normal maps (BC4/BC5).
Machine: Ryzen 7, 16 threads, SSE4.1, Release.

**Memory if every texture were resident** (the real per-scene working set is smaller; this is the ratio that matters):

| Device class | Shipped | Planned | Current D3D9 layer (RGBA8 fallback) |
|---|---|---|---|
| desktop BC / Apple silicon / Android with BC | 164.1 MB | 164.1 MB (as is) | 1001.7 MB |
| Android ASTC / Apple ASTC | 164.1 MB | 161.3 MB | 1001.7 MB |
| Android ETC2 only | 164.1 MB | 167.8 MB | 1001.7 MB |
| Apple PVRTC only (A7, square power-of-two only) | 164.1 MB | 479.0 MB | 1001.7 MB |
| no compressed format (software) | 164.1 MB | 1001.7 MB | 1001.7 MB |

Android ASTC with `COMPRESS_RAW` (the 115 BGRA8 textures, 30 MB, get compressed): 138.1 MB. With `PRIORITY_MEMORY` (ASTC 6x5 for DXT3, 0.6x tolerance): 98.6 MB. The RGBA8 fallback of today is **6.1x** the shipped size.
Apple PVRTC is the weak class: 2164 DXT1 and 969 DXT3 textures are not square power-of-two and PVRTC1 cannot hold them (they fall back to RGBA8, +300 MB); a padded or 16-bit fallback would fix it (A7 devices are very old; not done).

**Plans** (Android ASTC): DXT1 opaque 4026 -> ETC2 RGB8 (101 MB, same size); DXT1 with 1-bit alpha 506 -> ASTC 6x5 (7.5 -> 8.3 MB); DXT3 1195 -> ASTC 4x4 (17.7 MB, same size); DXT3 whose alpha is all 255, 256 textures -> ETC2 RGB8 (7.5 -> 3.8 MB, half);
12 + 9 textures under 16 texels stay RGBA8. On ETC2-only devices: DXT1 with alpha -> ETC2 RGBA8 (7.5 -> 15.0 MB), DXT3 -> ETC2 RGBA8.

**Quality of the re-encode against the decoded DXT** (PSNR / SSIM / CIEDE2000):

| Android ASTC | Textures | PSNR mean | Worst | Below 30 dB | SSIM | dE |
|---|---|---|---|---|---|---|
| DXT1 -> ETC2 RGB8 (all packs) | 4026 | 36.8 dB | 14.4 dB | 127 | 0.98 | 2.6 |
| DXT1 + alpha -> ASTC 6x5 | 506 | 39-58 dB | 27.2 dB | 15 | 0.99 | 1.2 |
| DXT3 -> ASTC 4x4 | 1195 | 47-68 dB | 33.0 dB | 0 | 0.99-1.00 | 0.1-0.7 |
| DXT3 opaque -> ETC2 RGB8 | 256 | 36-40 dB | 27.4 dB | 19 | 0.94 | 4.1 |

On ETC2-only devices: DXT3 -> ETC2 RGBA8 mean 36-47 dB (26 below 30 dB in TRACKS), DXT1+alpha -> ETC2 RGBA8 32-41 dB. Reading: ASTC reproduces DXT almost exactly; ETC2 is acceptable (mean 36-38 dB) but weak on sharp two-colour patterns
(the 14 dB cases are 32x32/64x64 test patterns and a few track decals): for those, `PRIORITY_QUALITY` chooses ASTC 4x4 (twice the memory) on ASTC devices; on ETC2-only devices there is no better target.

**Cold conversion cost** (`nue_texconv cook --classes all`, every variant of every texture): GLOBAL 1.6 s, FRONTEND 1.4 s, NIS 1.6 s, TRACKS 44.3 s on 16 threads, i.e. **about 49 s for the whole game**; the cache folder is 259.5 MB (10526 files)
for all nine device classes together (about 2.5 variants per texture; a single device needs roughly its 160 MB share). Opening that cache at startup (indexing 41595 source levels) takes 1.7 s.
**Run time:** replaying every texture through the Direct3D 9 seam on a context that cannot encode: **100 % of the levels served from the cache** (23094 levels in TRACKS, 0 placeholders, 0 encodes) for Android ASTC, Android ETC2 and Apple ASTC, with the alpha hint the layer knows and
with "unknown"; 50-150 us per level (hash + copy). Caveat: this replay uses the same bytes that were cooked; the real test is the dump of what the game uploads during a race (pending from the legacy-d3d9 session).

**Encoders not needed by these numbers:** BC2 (the game has 1460 DXT3 textures but the conversion path decodes them, it never encodes BC2); only a modding tool that replaces a DXT3 texture would need it (the assets session asked for it as a nicety, today it falls back to BC3).
No BC4/BC5/DXT5/BC7/HDR source exists in the game; PVRTC2, ATC and ETC2-A1 are irrelevant for it.

## 7c. Quality regression, NEON, speed and budget on the real textures (2026-10-25)

**Quality regression** (`test_real_quality`, opt-in: `NUE_TEX_REAL_DIR=<folder with the extracted DDS>`; without it the test reports "skipped" and passes; nothing is committed). For the device classes `android_astc` and `android_etc2` every texture is planned and
converted exactly as the cook does and compared with the decoded source; the per `source -> target` averages (PSNR, SSIM, CIEDE2000, textures below 30 dB) must stay inside the baseline table in the test (margins: PSNR -1.0 dB, SSIM -0.01, dE +15 % + 0.25, share below 30 dB +3 points; groups of fewer than 40 textures are printed but not judged, so `NUE_TEX_REAL_STRIDE` can be used for a quick run). `NUE_TEX_REAL_PRINT_BASELINE=1` prints a new table to accept a deliberate codec change. Run on every 10th texture of the dump (612 textures, 40 s): all eight rows inside the margins
(e.g. DXT3 -> ASTC 4x4 58.0 dB / SSIM 0.999; DXT1 -> ETC2 RGB8 36.8 dB / 0.985; DXT1+alpha -> ETC2 RGBA8 36.3 dB / 0.878). Conversions that are bit exact (RGBA8 fallbacks) report 99 dB and are not in the baseline.

**ARM NEON.** The compile-time ISA is chosen with `NUE_TEXTURE_SIMD=auto|none|sse2|sse41|avx2|neon`. What could be checked on the host (Windows x64) is the equivalence of the code paths: `test_simd` decodes a fixed set of blocks of every decoder and
encodes a fixed synthetic image with every encoder, and compares with a golden file (`tests/texture/data/simd_golden.bin`, produced by the scalar build). It passes for `none`, `sse2`, `sse41` (default) and `avx2`, and for the 32-bit x86 build:
decoders are **bit exact across all instruction sets**; astc-encoder, PVRTC and BC4/BC5 encoders produce identical bytes in every set; the etcpak based encoders (BC1, BC3, BC7, ETC, EAC) differ between their scalar/SSE/AVX2 code paths by up to 1.5 dB PSNR
(scalar BC1 +0.7 dB, AVX2 BC7 -1.2 dB, AVX2 ETC2 +1.5 dB against SSE4.1), so the test accepts 2.5 dB for them. Consequence for the cache: a cook made with another instruction set is a *different but equivalent* result, never a cache-key issue (the key does not contain the ISA).
The NEON build was **compiled** with Android NDK 28.2 for arm64-v8a (standalone module, tests and `nue_texconv`), but **not run**: no emulator or device was available. `tests/texture/run_on_android.sh` pushes the test executables and the golden file with `adb` and runs them;
it is ready for the emulator nue-android is preparing. Until then NEON is "compiled, reviewed, not executed".

**Speed** (cook of `TRACKS/STREAML4RB`, 473 textures, every device class, one thread, SSE4.1, Release, `nue_texconv cook --profile`): **29-31 s -> 19.3 s** after the hotspot work below. Per target format, thread time of the encoder:

| Target | Encoder | MPixel/s per thread |
|---|---|---|
| ETC2 RGB8 / RGBA8 | etcpak | 29.6 / 30.0 (AVX2 build: ~91) |
| PVRTC1 4bpp | Javelin | 8.2 |
| ASTC 6x5 / 4x4 | astc-encoder (quality 60 / 98 mapping) | 1.55 / 1.18 (AVX2: +5 %) |

The decode (0.2 s) and the level hashing (6 ms) are noise (1-2 %); **ASTC takes 75 % of the cook** (11.5 s + 1.1 s of 15.3 s encode); that is the price of ASTC quality and the knob is the effort (`nue_tex_policy.quality`, mapped to astc-encoder presets); a faster preset was not adopted without
a quality decision. Removed hotspots (measured, in order): (1) the *sRGB ASTC variants* were cooked for every texture although the game never asks for them; now they exist only when the request carries `NUE_TEX_REQ_SRGB` (`cook_image`);
(2) SHA-256 of every source level (the cold lookup key) replaced by a 128-bit non-cryptographic multiply-fold hash (`fast_hash128`; the cache is local, not a security boundary) and the source digest is computed once per image; the cache folder shrank from 223 to 178 MB for the same textures.
Remaining: writing the cache files (KTX2 + atomic rename) is 16 % of the thread time, ASTC is the rest.
**Opening the cache** at start-up was 1.7-5 s for 31892 indexed levels because every `.ktx2` was opened; the index is now persisted (`<cache>/index.nuei`, CRC protected, codec-version stamped, paths relative so the cache stays movable, rebuilt by a scan when absent or damaged):
**4.96 s (first open, builds it) -> 0.09 s**. A cold lookup at run time now reads only the bytes of the requested level (`ktx2_read_level`) instead of loading the file; the replay through the D3D9 seam is 100 % served from the cache at 270-300 us per level, dominated by opening one file per lookup.

**Texture budget with `nue_pack convert` (nue-assets) on real packs, for an Android-class and a desktop-class device.** Memory per pack (`nue_pack convert --target ...`; source = DXT as shipped; RGBA8 = what the current D3D9 layer fallback would use):

| Pack | Textures | Source MB | Android ASTC MB | Android ETC2 MB | Desktop BC MB | RGBA8 MB |
|---|---|---|---|---|---|---|
| FrontB (menus) | 499 | 4.4 | 4.4 | 4.6 | 4.4 | 18.3 |
| GlobalB | 211 | 6.7 | 7.3 | 7.6 | 7.3 | 11.9 |
| InGameRace | 43 | 0.25 | 0.25 | 0.26 | 0.25 | 1.1 |
| IntroNis01 | 6 | 0.33 | 0.37 | 0.67 | 0.33 | 2.7 |
| STREAML4RC | 266 | 6.9 | 10.3 | 15.0 | 9.7 | 48.7 |
| STREAML4RA | 1836 | 42.9 | 50.0 | 81.0 | 46.2 | 310.7 |
| STREAML4RD | 1916 | 44.6 | (budget rejected, see below) | (rejected) | 47.8 | 323.8 |

Findings: (a) `nue_pack`'s variant choice is its own fixed mapping and **does not call the planner**: DXT1 goes to ETC2 RGBA8 on ETC2 devices (the planner picks ETC2 RGB8 for opaque DXT1, half the memory: STREAML4RA 81 MB instead of ~44-52 MB) and to ASTC 6x5 for every DXT1 on ASTC devices
(the planner uses ETC2 RGB8 for the 4026 opaque ones). The planner exists in this module (`nue_tex_make_plan`); nue-assets should call it or `nue_tex_cook_image`. (b) For STREAML4RD the Android variants were rejected because the engine's `assets` budget refused a 134 MB build: this is the budget doing its job, with a limit that the assets session configured; it is not a texture-module limit.
(c) `nue_texconv budget-sim <dump> --device CLASS --available-mb N --preset P` plays every scene of the dump in order through the texture budget with LRU eviction (evict callback frees real sizes). Totals, planned by this module: all scenes together 153.0 MB (Android ASTC), 155.8 MB (desktop BC) against 985.6 MB as RGBA8.
Android ASTC, `low_memory_mobile` on a 1 GB device (limit 122.9 MB): the global/front-end/NIS packs stay resident (~22 MB, pinned), tracks A, B, C, D load (peak 123.0 MB) and the budget evicts 11.6 + 11.8 + 7.9 MB = **31.3 MB in 8 eviction rounds** while D, F, G stream in; resident at the end 121.7 MB, nothing rejected.
Android ASTC, `mobile` on 4 GB (limit 819 MB): all 153.0 MB resident, no eviction. Desktop BC, `desktop` on 16 GB (limit 5734 MB): 155.8 MB resident, no eviction. (The simulation loads each pack as a whole; a real race needs one track plus the shared packs, ~65 MB.)

## 8. Memory (budget API, model, D3D9 layer numbers)

* **No built-in limit.** `nue_tex_budget_policy`: unlimited | bytes | fraction of the available memory the host reports | preset (`LOW_MEMORY_MOBILE` 12 % with a 64 MiB floor, `MOBILE` 20 %,
  `DESKTOP` 35 %, `HIGH_END` 50 %), min/max clamps, changeable at run time; eviction callback receives LRU-ordered candidates (pinned resources never); statistics. A game that wants a fixed ceiling configures that number.
  The engine-wide manager (`nue_budget_*`, `memory.h`, core session) owns the policy defaults per platform. `nue_tex_budget_attach_engine(budget, "texture", &id)` (engine builds) registers a `NUE_BUDGET_TEXTURE`
  budget and puts this module's LRU accounting behind it: the limit then comes from the engine policy / `budget.texture.*` config keys / pressure events; `nue_tex_budget_add` goes through `nue_budget_request`
  (`NUE_ERR_OVERFLOW` when it does not fit even after eviction), frees are reported, and the engine's eviction hook calls the owner's evict callback with LRU candidates (tested in `test_engine_budget`, 32 checks, including OS pressure through `nue_budget_trim`).
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
4. No encoding at run time (section 2b): `nue_tex_d3d9_convert_level` is a lookup by content in the cooked cache; `NUE_OK_PARTIAL` means a placeholder was written for a level the cache does not know (`info.defer_to_worker` is only a development hint now). Cook with `nue_texconv cook` or the import pipeline before play.
5. `alpha_hint`: DXT1 with unknown alpha plans an alpha-capable target (ASTC 6x5), DXT1 known opaque plans ETC2 RGB8 (half the size of the alternative). The layer knows more than the module: pass what it saw.
6. `plan.explanation` per texture belongs in the layer's texture report.

Seam cost at play time: SHA-256 of the level plus a copy (test_cold); the encode cost is paid once, cold, at the throughput of section 7.

## 10. Status

| Item | State |
|---|---|
| Format registry (130 formats), geometry, 8 mapping tables, D3D9 table as data | implemented, tested for internal consistency; the **numeric ids of Metal/GL/Vulkan were written from public headers and are not verified against the SDKs** |
| Caps model, 9 device-class presets, bgfx and engine GPU-probe adapters (`NUE_FMTSUP_EMULATED` -> `NUE_TEX_FCAP_EMULATED`) | implemented, tested |
| Planner | implemented, 134 checks over desktop/Android/Apple/software classes |
| Codecs | BC1/2/3/4/5/6H/7 decode; ETC1/ETC2 RGB/RGBA8/EAC R11/RG11 decode; ASTC LDR/HDR (all 14 block sizes) decode + encode; PVRTC1 4bpp decode + encode (square power-of-two only); encoders BC1/3/4/5/7, ETC1/ETC2/EAC R/RG |
| **Gaps** | **decoders missing for ETC2 RGB-A1, PVRTC1 2bpp, PVRTC2, ATC (registry flags say so; the planner never plans to them); encoders missing for BC2, ETC2 RGB-A1, PVRTC 2bpp/PVRTC2 (no open encoder found); KTX2 supercompression (zstd/BasisLZ) refused, not misread** |
| DDS, KTX2, PNG read, PNG write | implemented, round trips for mips/cube/array/volume, truncation fuzz, hand-built legacy D3D9 headers |
| Mip generation | box (exact), triangle, Kaiser, Lanczos; linear-light; normal renorm; alpha coverage; wrap; premultiplied; 3D |
| Cold conversion (cook all device classes, index by content, no run-time encode, movable cache) | implemented; `test_cold` (80 checks); `nue_texconv cook` run on a folder and re-run (everything already cooked) |
| Cache | hit/miss/corrupt/trim/clear tested; LRU by file time; atomic writes. Not tested: concurrent processes sharing one directory |
| Import pipeline, async requests | tested (resume via cache, pause, cancel, failures) |
| Budget | tested (LRU, pinned, run-time policy change, presets); bridge to the engine budget manager tested in the engine build |
| D3D9 seam | tested against synthetic DXT data; **not wired into nue_d3d9** (other owner) |
| nue_texconv | info/convert/compare/plan/scan/cook/bench/budget-sim/formats/classes/caps work, run on the real textures |
| Real game textures | tested on the 6119 extracted textures (section 7b, 7c): scan, quality regression (opt-in test), cook, replay through the D3D9 seam, budget simulation. **Not seen:** the dump of what the game really uploads during a race (pending from the legacy-d3d9 session) |
| Other platforms | run on Windows x64 and 32-bit x86 (all suites), SIMD sets none/sse2/sse41/avx2 equivalent (`test_simd`); Android arm64 (NEON) **compiled with the NDK, not run** (no emulator yet, `tests/texture/run_on_android.sh` ready); Linux/macOS/iOS builds are CI's job |

### 10b. Backlog (decided not to build now, with the reason)

Decision of the main session (2026-10-25), after the real-texture numbers: do not write these until a real need appears.

* **BC2 (DXT3) encoder**: the game ships 1460 DXT3 textures but every plan decodes them and encodes to ASTC/ETC2/BC3; BC2 would only be written by a modding tool that replaces a DXT3 texture (BC3 is the fallback today).
* **PVRTC 2bpp and PVRTC2 encoders and decoders, ATC decoder/encoder, ETC2 RGB-A1 decoder/encoder**: no source texture in the game uses them and no device class in the presets needs them (Apple PVRTC-only A7 is the single PVRTC consumer and it is served by PVRTC1 4bpp);
  no open encoder exists for PVRTC2/2bpp (PVRTexLib is proprietary); the decoders can be ported from bimg (BSD-2) when a texture arrives in those formats.
* **KTX2 supercompression (zstd/BasisLZ)**: refused with an error, not misread.
* **Apple PVRTC non-square**: falls back to RGBA8 (+300 MB for the game); a padded or 16-bit fallback would fix it.
* **ASTC effort**: a faster astc-encoder preset (medium instead of thorough for 6x5) would cut the cook by a large share (ASTC is 75 % of it) at a quality cost; needs a decision; the level-3 run was not completed.
* **NEON on a device**: the arm64 build and the golden test exist, they have not been executed.

## 11. Requests to other owners

* **Legacy D3D9 owners**: section 9.
* **Platform sessions**: fill `nue_gpu_caps.format[]` honestly and set `NUE_FMTSUP_EMULATED` (core added it) for formats the driver decompresses at upload, so the planner does not count them as savings.
* **Core**: nothing pending; the root contract (`nue_texture_obj`) is used as documented in `docs/REPLIES_core.md`.
