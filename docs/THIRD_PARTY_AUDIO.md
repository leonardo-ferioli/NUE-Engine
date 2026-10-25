# Third-party code used by the audio module

Vendored code lives in `src/audio/third_party/<name>/` with the upstream licence file next to it and an `UPSTREAM.txt` (source, commit, what was copied). It
is compiled into `nue_audio` through the `nue_fragment.cmake` of each folder and sits behind our own interfaces (`audio_codec.h`, `audio_dsp.h`), so each piece
can be replaced. Downloaded code was reviewed statically (no process / network calls beyond what a codec needs) and executed only by building and testing it.
Date of every copy: 2026-10-25. **The user reviews the licences afterwards.**

| Folder | Upstream | Pinned commit | Licence | Original path(s) | Changes | Used where |
|---|---|---|---|---|---|---|
| `stb_vorbis/` | https://github.com/nothings/stb | `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5` | MIT **or** public domain (Unlicense), `LICENSE` | `stb_vorbis.c` v1.22 | none; built with `STB_VORBIS_NO_STDIO`, `STB_VORBIS_NO_PUSHDATA_API` | `codecs/dec_vorbis.cpp` |
| `dr_flac/` | https://github.com/mackron/dr_libs | `ea33eedde32b3a1baa92bb78fc98c4711f890623` | Unlicense **or** MIT-0, `LICENSE` | `dr_flac.h` v0.13.4 | none; `dr_flac_impl.c` (3 lines, ours) defines the implementation without stdio / Ogg | `codecs/dec_flac.cpp` |
| `opus/` | https://github.com/xiph/opus tag v1.6.1 | `22244de5a79bd1d6d623c32e72bf1954b56235be` | BSD-3-Clause, `COPYING`; royalty-free patent grant | `include/`, `celt/`, `silk/`, `silk/float/`, `src/` (sources listed by upstream `opus_sources.mk`, `celt_sources.mk`, `silk_sources.mk`, float build), `COPYING`, `AUTHORS` | none to the files; `config.h` and `nue_fragment.cmake` are ours; no SIMD, no `dnn/` | `codecs/dec_opus.cpp` (decoder); the encoder is compiled in for tier 3 |
| `ebur128/` | https://github.com/jiixyj/libebur128 | `67b33abe1558160ed76ada1322329b0e9e058b02` | MIT (`COPYING`, (c) 2011 Jan Kokemuller); `queue/sys/queue.h` BSD-3-Clause (Regents of the University of California) | `ebur128/ebur128.c`, `ebur128.h`, `ebur128/queue/sys/queue.h` | none; built with `_USE_MATH_DEFINES` and `M_PI` defined (strict C99 hides it) | `dsp/loudness.cpp` |
| `vgmstream_notice/` | https://github.com/vgmstream/vgmstream | `d9875b1fff307e4759dfb85e49e3d5e6fb196f7c` | ISC-style, `COPYING` (lists portions by other authors in files not used) | `src/coding/{psx,ngc_dsp,ea_xa,ea_xas,ima}_decoder.c` read, **not copied** | - | `codecs/adpcm.cpp` is written against their behaviour; licence shipped as a precaution |

## Data tables typed in by hand

* `dsp/i3dl2.cpp`: the 30 I3DL2 environment presets (IA-SIG I3DL2 Level 2 guideline table, as exposed by DirectSound `DSFX_I3DL2_ENVIRONMENT_PRESET_*`).
  Cross-checked number by number against `dsound.h` of Wine as mirrored in mingw-w64 (commit `7ce22f6637cc93fd2eb37c8a645908c92bb80bbd`, **LGPL-2.1**).
  Nothing was copied from that file (no code, no comments, own struct layout), but the numbers are identical by nature. **Flagged for the user.**

## Consulted, not copied

| Project | Licence | Why it matters |
|---|---|---|
| libsamplerate (libsndfile), speexdsp resampler | BSD-2-Clause / BSD-3-Clause | design reference for the polyphase kernel |
| OpenAL Soft | LGPL-2 | reference for the EFX / EAX reverb model; **not used because LGPL** |
| libspatialaudio | LGPL-2.1+ (or commercial) | reference for ambisonic / binaural layout; **not used because LGPL** |
| Wine / mingw-w64 `dsound.h` | LGPL-2.1 | see the I3DL2 table above |

## Licence summary for the user

* Permissive (MIT, Unlicense / MIT-0, BSD-3, ISC): everything vendored. Keep the notices (they are in the folders).
* **LGPL / GPL: nothing is linked or copied.** Two LGPL projects (OpenAL Soft, libspatialaudio) and Wine's `dsound.h` were read for ideas and numbers only; the
  I3DL2 number table is the one place where LGPL material is *cross-checked*, not copied.
* **Unclear**: none of the vendored files. vgmstream's `COPYING` lists "portions" by other authors; none of those files were used.
* Patents: Opus has a royalty-free grant; AOM's IAMF (not started) comes with the AOM patent licence which must be read before shipping it.
