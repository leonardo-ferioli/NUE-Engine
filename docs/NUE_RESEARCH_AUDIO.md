# Audio research: what exists, what was reused

Written 2026-10-25 by the audio session. "Licence" means what was read in the repository's own licence file or file header on that date, not a
search summary (the GitHub API reports `NOASSERTION` for several of these). Commit hashes are in [THIRD_PARTY_AUDIO.md](THIRD_PARTY_AUDIO.md) for what
is vendored. **Decision** says what the engine does today.

## Tier 1

| Need | Candidates looked at | Licence (verified) | Decision |
|---|---|---|---|
| Game ADPCM (IMA, MS, Xbox, PSX VAG, GC/Wii DSP, EA XAS, EA-XA) | **vgmstream** (hundreds of game formats) | ISC-style, `COPYING`, with a list of "portions" by other authors (Marko Kreen, jagarl, Nullsoft, Paul Hsieh, Entis...) that apply to files outside the ones used here | **Source of format knowledge, no file copied.** The decoders are about 50 lines each; they were written in `src/audio/codecs/adpcm.cpp` against the behaviour of vgmstream's `psx`, `ngc_dsp`, `ea_xa`, `ea_xas`, `ima` decoders (edge cases: PSX filter > 4 and shift > 12, flag 7 silence; DSP coefficient index clamp; XAS group layout and float truncation; Xbox 64-sample blocks; EA-XA v1/v2 rounding). Outputs are checked by hash against an independent Python transcription of those decoders (`tests/audio/gen_adpcm_vectors.py`). vgmstream's `COPYING` is shipped in `src/audio/third_party/vgmstream_notice/`. Not linked: its container/meta layer (~1000 formats) is the job of nue_assets. |
| Opus | **libopus** (xiph/opus) | BSD-3-Clause (`COPYING`) + royalty-free patent grant | **Vendored v1.6.1**, float build, no SIMD, no DNN extensions; decoder used, encoder compiled for tier 3 voice chat. Ogg page layer is our own (`dec_opus.cpp`). |
| Vorbis | **stb_vorbis** (nothings/stb), libvorbis (xiph) | stb: MIT or public domain; libvorbis: BSD-3 | **stb_vorbis vendored** (one C file, memory based, sample exact seek). libvorbis would need libogg and is bigger for no gain at this scale. |
| FLAC | **dr_flac** (mackron/dr_libs), libFLAC (xiph) | dr_flac: Unlicense or MIT-0; libFLAC: BSD-3 for the code, GFDL for the docs (GitHub shows the docs licence) | **dr_flac vendored** (single header, streaming, seek table). |
| Resampler | **libsamplerate** (libsndfile), **speexdsp** `resample.c` (xiph), soxr | libsamplerate 0.2.x: BSD-2-Clause; speexdsp: BSD-3-Clause (header read); soxr: LGPL-2.1 | **Studied, not copied**: all three use a windowed-sinc polyphase kernel; ours is a Kaiser design written for four quality levels, fixed-point read position, time-varying ratio. Measured: 77 / 96 / 124 / 133 dB SNR on a 1 kHz tone 48 -> 44.1 kHz, against 55 dB for the engine's linear converter. soxr is LGPL and was not considered further. |
| Loudness (EBU R128 / BS.1770) | **libebur128** (jiixyj) | MIT (`COPYING`); `queue.h` BSD-3 (Regents of the University of California) | **Vendored** unchanged (last upstream commit 2021 is still the reference implementation). Wrapped in `nue_loudness_*`. |
| Dynamics | nothing vendored | - | Own code: Giannoulis/Massberg/Reiss gain computer; limiter by sliding minimum + moving average (ceiling holds by construction, true-peak detection with 16-tap Kaiser sinc interpolators). |
| Reverb | **OpenAL Soft** (EAX/EFX reverb), Freeverb, Dattorro, libspatialaudio | OpenAL Soft: LGPL-2 (`COPYING` is the GNU Library GPL v2); libspatialaudio: LGPL-2.1+ or commercial; Freeverb: public domain | **LGPL code not copied** (a static link of LGPL code into a public SDK that games embed is a decision for the user). Own FDN reverb (Jot structure, Hadamard feedback, per-line RT60 and HF damping design) with the **EFX parameter model**; measured RT60 = 0.50 / 1.50 / 2.99 s for 0.5 / 1.5 / 3.0 requested. |
| Convolution reverb | pffft (BSD-like), kissfft (BSD-3), pocketfft | permissive | Own radix-2 FFT (power of two partitions only), no dependency. Matches direct convolution to 1.5e-6 on a 3000 tap IR. |
| EAX / I3DL2 presets | Microsoft `dsound.h` (`DSFX_I3DL2_ENVIRONMENT_PRESET_*`), Wine / mingw-w64 `dsound.h`, OpenAL Soft `efx-presets.h` (EAX 1/2 environments converted to EFX) | The numbers are the IA-SIG I3DL2 Level 2 guideline table. Wine's header is LGPL-2.1; `efx-presets.h` carries no notice of its own inside an LGPL project | **I3DL2 table typed in by hand** (30 presets), cross-checked against Wine's header, nothing copied. **EAX 1.0 / 2.0 environment tables are NOT included**: they could not be verified from a source with a clear licence in this session. `nue_reverb_params_from_eax` converts any EAX 2/3 property set, so the DirectSound layer can feed the properties the game sends; the 26 EAX 1 environment ids still need a table (open item, see AUDIO.md). |

## Tier 2

| Need | Candidates | Licence (verified) | Decision |
|---|---|---|---|
| Panning | VBAP (Pulkki 1997), OpenAL Soft, libspatialaudio | - / LGPL | Own 2D VBAP and sine law. |
| HRTF binaural | **Steam Audio** (Valve), **libmysofa** (Symonics), OpenAL Soft (MHR), libspatialaudio | Steam Audio: Apache-2.0; libmysofa: BSD-3-Clause (header read; needs zlib and a SOFA file); OpenAL Soft: LGPL | Own spherical head model (Woodworth delay + head shadow) as the always-available fallback; **measured HRTFs plug in through `nue_spatial_set_hrtf`**. The SOFA reader (libmysofa + zlib, or a small netCDF/HDF5 subset) is the next step and belongs in `tools/nue_audio` so the runtime needs no HDF5. Steam Audio is Apache-2.0 and ships the SOFA/HRTF pipeline, occlusion, propagation and reflection simulation; it is a large C++ library with a prebuilt binary distribution, so it is **an optional backend candidate**, not vendored. |
| Ambisonics | **Resonance Audio** (resonance-audio), libspatialaudio | Resonance Audio: Apache-2.0; libspatialaudio: LGPL-2.1 | Own encode (ACN/SN3D orders 1..3) and a regularised mode-matching decoder with max-rE weights; Resonance Audio is the reference for a future higher-order/room-aware path (Apache-2.0, can be vendored). Limitation: on 5.1 the decoder is approximate (horizontal harmonics only). |
| Platform spatializers | Windows Sonic / Dolby Atmos for Headphones (`ISpatialAudioClient`, Windows 10+, Windows Spatial Audio API), Android 13 `android.media.Spatializer`, Apple PHASE / `AVAudioEnvironmentNode` | OS APIs | **Hook only**: `nue_spatial_backend_ops` receives evaluated sources and writes the mix; a failing backend falls back to the built-in renderer. Entry points are to be confirmed against Microsoft Learn / Android / Apple documentation by the platform sessions before implementation (requests will be written when they start; none of these APIs was called in this session). |
| Object audio: ADM | **EBU ADM Renderer** (ebu_adm_renderer, Python reference, BSD-3-Clause-Clear), **libear** (ebu/libear, Apache-2.0), **BEAR** (ebu/bear, Apache-2.0) | as listed (LICENSE files read) | Not started. libear is the C++ renderer to vendor when a game needs ADM; the hook above is where it connects. |
| IAMF / Eclipsa | **libiamf** (AOMediaCodec) | BSD-3-Clause-Clear (+ AOM patent licence: read it before shipping) | Not started. |
| Dolby Atmos | proprietary | - | **Not reimplemented.** Use the OS output (Windows Spatial Audio / Atmos for Headphones, Atmos on Xbox, Apple Spatial Audio) through the backend hook. |

## Tier 3 (not started)

| Need | Candidate | Licence (verified) | Note |
|---|---|---|---|
| Time stretch / pitch | **signalsmith-stretch** | MIT | header only, C++11; first candidate. |
| Voice chat | libopus encoder (already compiled in), WebRTC audio processing (BSD-3), **RNNoise** | RNNoise BSD-3-Clause | |
| LE Audio | **liblc3** (Google) | Apache-2.0 | |
| Bluetooth latency | none needed beyond `nue_audio_stream_get_latency` | - | |
| Engine sound synthesis, haptics from audio | none found worth vendoring | - | own work |
