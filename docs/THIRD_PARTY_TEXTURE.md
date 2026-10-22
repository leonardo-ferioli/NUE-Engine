# Third-party code used by the texture module

Everything below is **vendored** (copied, original headers and licence files kept) under `src/texture/third_party/<name>/`, one folder each.
Downloaded code is untrusted data: each file set was reviewed statically (no process/network/file-system calls beyond what a codec needs;
`grep` for `system(`, `popen`, `CreateProcess`, sockets found nothing), nothing from them was executed except by building the module.
Pinned by upstream commit. Update by re-copying from a newer commit and bumping `NUE_TEX_CODEC_VERSION` (cache keys change).

| Folder | Upstream | Pinned | Licence | Used for | What we took |
|---|---|---|---|---|---|
| `astc-encoder/` | https://github.com/ARM-software/astc-encoder | `d0f91386350222fcbf6da533c0f7a3f9b5c6b783` (5.7.0) | Apache-2.0 (`LICENSE.txt`) | ASTC LDR/HDR encode and decode, all 14 2D block sizes, SSE2/SSE4.1/AVX2/NEON/SVE/none builds | `Source/astcenc*.cpp|h` (library only; the command line tool `astcenccli_*` was not copied) |
| `etcpak/` | https://github.com/wolfpld/etcpak | `3d716e1550023dc5ff8b602636af8b76584e0b66` | BSD-3-Clause (`LICENSE.txt`, `AUTHORS.txt`) | ETC1 / ETC2 RGB / ETC2 RGBA8 (EAC) / EAC R11 / EAC RG11 encoders; BC1, BC3, BC4, BC5, BC7 encoders; ETC1/2/EAC decoders | `ProcessRGB`, `ProcessDxtc`, `Decode`, `Tables`, `Dither`, `Math`, `Vector`, `ProcessCommon`, `ForceInline`, `bc7enc` (Richard Geldreich's BC7 encoder, MIT or public domain, bundled by etcpak). Not copied: CLI, file readers, task dispatch, `bcdec.h` copy (we use the newer standalone one) |
| `bcdec/` | https://github.com/iOrange/bcdec | `80859ed3b7afb1c527a2a99d70c61457bea72d0c` (0.985) | MIT or Unlicense (`LICENSE`) | BC1, BC2, BC3, BC4, BC5, BC6H (half and float), BC7 decoders (single header) | `bcdec.h` |
| `pvrtc_javelin/` | PvrTcCompressor by Jeffrey Lim, as redistributed in https://github.com/bkaradzic/bimg `3rdparty/pvrtc` | bimg `9bfbd6493b48a15238f2d1374a718ec026524515` | BSD-3-Clause style (`LICENSE.TXT`) | PVRTC1 4 bpp RGB/RGBA encoder and decoder | whole folder (`PvrTc*`, `Bitmap`, helpers) |
| `stb/` | https://github.com/nothings/stb `stb_image.h` v2.30 | master at `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` (2026-10-22) | MIT or public domain (end of the file) | PNG reading for `nue_texconv` and tests (`STBI_ONLY_PNG`) | `stb_image.h` |

## Evaluated and not vendored (and why)

| Candidate | What it provides today | Decision |
|---|---|---|
| **bimg** (bkaradzic, BSD-2) | BC1-7 / ETC1/2 / EAC / ASTC / PVRTC / ATC decoders, encoders through astc-encoder, etcpak (ETC only, older), libsquish, nvtt, rg_etc1, PvrTc; KTX/DDS/PVR readers, mip generation | Not used wholesale: it needs `bx`, its API is one monolithic image library, its encoders are the same upstream projects at older pins and slower settings. **Used as a source**: the PVRTC code, and its ATC / ETC2 A1 / PVRTC decoders are the reference for the decoders we still have to add (BSD-2, attribution will be kept in the file that ports them). Its TextureFormat enum order defines the bgfx mapping. |
| **etcpak** | see above | **Used.** Fastest ETC/BC encoder available; has SSE4.1/AVX2/NEON paths and scalar fallback. No ETC2 RGB-A1 encoder (so BC1-with-alpha goes to ETC2 RGBA8 or ASTC). |
| **astc-encoder** | see above | **Used.** The reference encoder/decoder; four preset speeds. |
| **bc7enc / rgbcx / bc7enc_rdo** (Geldreich, MIT/PD) | BC1-5 (`rgbcx.h`, better than etcpak's BC1 at higher levels), BC7, RDO | BC7 comes through etcpak's copy. `rgbcx.h` is a candidate upgrade for "high quality BC1/BC3" if measurements show etcpak's BC1 too weak; the repo clone is 54 MB (RDO tools), so only that header would be taken. |
| **Compressonator** (AMD, MIT) | BC1-7, ETC, ASTC, ATC, GPU paths | Not used: very large, many dependencies (OpenCV-style helper libs, Qt tools), slow build; nothing it does is missing from the chosen set except ATC *encoding*, which no target needs. |
| **Intel ISPC Texture Compressor** (MIT) | BC1-7, ETC1, ASTC (ISPC) | Not used: needs the ISPC compiler in every build (4 platforms) and its ASTC/ETC are weaker than astc-encoder/etcpak. |
| **Basis Universal** (Apache-2.0) | UASTC / ETC1S supercompressed intermediate and transcoders to BC/ETC/ASTC/PVRTC | Not used now: it solves "one file, many targets" for content we author; our sources are already DXT. A basis/KTX2-BasisLZ *reader* could be added later (the KTX2 reader reports supercompression != none as unsupported). |
| **KTX-Software / libktx** (Apache-2.0) | KTX2 read/write, Basis, zstd | Not used: heavy dependency tree for what is a small container (header, level index, DFD, key/values). We write KTX2 ourselves; Khronos' format spec is the reference. |
| **libsquish** (MIT) | BC1-3 | Not used: slower and lower quality than etcpak/rgbcx. |
| **crunch** (public domain / zlib) | DXT/ETC1 + its own container | Obsolete (replaced by Basis); not used. |
| **PVRTexLib** (Imagination) | PVRTC1/2 encode, ETC, ASTC | Proprietary: avoided as requested. Open PVRTC1 4 bpp (Javelin) is used; PVRTC 2 bpp and PVRTC2 have **no open encoder** found (decoders only, still to be written from the PowerVR spec / bimg); the planner never targets them except on devices whose only compressed format is PVRTC. |
| **DirectXTex** (MIT) | DDS reader/writer, BC encoders on CPU/GPU | Not vendored: the DDS format is small; we follow its header handling rules (legacy bitmask / FOURCC / DX10) from the public spec. |
| **zstd / miniz** | KTX2 supercompression | Not needed: GPU block formats barely compress and the cache wants zero-copy mapping. Reading zstd KTX2 can be added when needed. |

## Licence check for the public repository (2026-10-22)

All vendored folders keep their original licence text next to the code (`astc-encoder/LICENSE.txt` Apache-2.0, `etcpak/LICENSE.txt` + `AUTHORS.txt` BSD-3-Clause,
`bcdec/LICENSE` MIT or Unlicense, `pvrtc_javelin/LICENSE.TXT` BSD-3-Clause style, `stb_image.h` MIT or public domain at the end of the file, `bc7enc` MIT or public domain in its header).
All are permissive: source and binary redistribution is allowed with the notices kept. A shipped binary (SDK package, game) must reproduce the notices of the BSD and Apache
components: the packaging step should collect `src/texture/third_party/*/LICENSE*` into the SDK's notices file. Notes: the Javelin folder also holds helper files contributed by Brendan Duncan
(bitmap and colour classes, see its README) under the same folder licence as redistributed by bimg; no GPL/LGPL/proprietary code and no game data are in the tree; no personal paths appear in the texture sources or docs
(checked with a search for user names and drive paths).
