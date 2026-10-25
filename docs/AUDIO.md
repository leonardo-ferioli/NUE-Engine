# NUE audio module

Library `nue::audio` (`src/audio`, static, on top of `nue::nue`). It adds codecs, signal processing and spatial rendering **on top of** the native audio API
and the mixer; it changes neither `audio.h` nor `mixer.h`. Headers (not included by `nue/nue.h`):

| Header | What |
|---|---|
| `nue/audio_codec.h` | game ADPCM block decoders, streaming decoder `nue_adec` (WAV, FLAC, Ogg Vorbis, Ogg Opus, raw PSX / DSP / XAS / EA-XA / IMA / MS / Xbox), prefetch stream |
| `nue/audio_dsp.h` | resampler, channel mixer, HQ converter, compressor, limiter, dynamic range modes, EBU R128 loudness, FDN reverb with EFX / EAX / I3DL2 model, convolver |
| `nue/audio_spatial.h` | scene maths, panning, binaural, ambisonics, HRTF hook, platform / object-audio backend hook |

Research and licences: [NUE_RESEARCH_AUDIO.md](NUE_RESEARCH_AUDIO.md), [THIRD_PARTY_AUDIO.md](THIRD_PARTY_AUDIO.md). Requests to other areas: [REQUESTS_audio.md](REQUESTS_audio.md).

## How it plugs into the mixer

```
 file / memory / pack entry -> nue_adec  --pull--> NUE_VOICE_CALLBACK voice   (nue_adec_pull_s16 / _f32 are nue_voice_desc.pull)
                                         \-decode_all-> nue_buffer (static voice)
 bus -> NUE_EFFECT_CALLBACK slot: callback = nue_reverb_dsp / nue_compressor_dsp / nue_limiter_dsp / nue_drc_dsp / nue_convolver_dsp, userdata = handle
 master chain: nue_drc (compressor + look-ahead limiter), nue_loudness meter on the mixer output
 3D: nue_spatial_evaluate on the mixer's own 3D parameters, or nue_spatial_render_source for sources the application routes itself
```

Reverb and convolver effects return the **wet signal only**; set the effect's `wet_dry` or use a send bus. Everything is tested on the mixer in manual mode
(`tests/audio/test_dsp.cpp`: reverb as bus effect, bit-identical twice; `test_codec.cpp`: a decoder driving a callback voice).

## Codecs (`audio_codec.h`)

| Format | Block / frame API | Streaming (`nue_adec`) | Tested against |
|---|---|---|---|
| PCM u8 / s8 / s16 le/be / s24 / s32 / f32 | - | WAV and raw | exact values |
| IMA ADPCM (WAV 0x11) | `nue_adpcm_ima_wav_decode_block` | WAV, raw | hand-computed steps + hash of a Python transcription of the reference decoder (mono, stereo, 4 ch) |
| Microsoft ADPCM (0x02) | `nue_adpcm_ms_decode_block` | WAV (coefficients from the header), raw | hand-computed block + hash |
| Xbox ADPCM (0x69) | `nue_adpcm_xbox_decode_block` | WAV, raw | hash (mono, stereo) |
| PlayStation SPU / VAG | `nue_adpcm_psx_decode_frame` | raw, interleave, checkpoints for seek | hash incl. bad filter / shift / flag 7 frames |
| GameCube / Wii DSP ADPCM | `nue_adpcm_dsp_decode_frame` (+ nibble helpers) | raw with per-channel coefficients | hash |
| EA XAS v1 | `nue_adpcm_ea_xas_decode_block` | raw | hash (stereo) |
| EA-XA v1 / v2, stereo v1 | `nue_adpcm_ea_xa_decode_frame`, `_stereo_frame` | raw (mono) | hash |
| FLAC | - | yes | **bit exact** against embedded PCM, seek exact |
| Ogg Vorbis | - | yes (whole compressed file is read into memory) | tone frequency / level per channel, 5.1 channel order, seek |
| Ogg Opus | - | yes (own Ogg reader, 200 ms pre-roll on seek) | tone level, length, seek error < 0.005, tags with loop points |

Loop points come from WAV `smpl`, Vorbis comments (`LOOPSTART`, `LOOPEND` or `LOOPLENGTH`, FLAC / Vorbis / Opus) or the raw description; `nue_adec_set_loop(dec, n)`
wraps inside the decoder, so a voice never sees the seam. Seeking is sample exact for every codec; ADPCM streams keep a state checkpoint every 256 codec frames.
Memory sources never allocate in `read`; use `nue_adec_stream` (prefetch thread + lock-free ring) for file sources on the audio thread.

Known gaps: EA container parsing (SCHl, ABK) belongs to nue_assets; EA-XA "PCM frames" (0xEE) belong to that parser; only mono EA-XA is streamed; Opus mapping family 255
(ambisonics) and chained Ogg streams are rejected; no MP3 / AAC / ATRAC (no vendored decoder yet: minimp3 is public domain and the next candidate).

NFSU2 itself does its own decoding in game code and uses DirectSound as a single streaming ring buffer, so these decoders serve the console layers and the asset tools first.

## DSP (`audio_dsp.h`)

* **Resampler** (`nue_resampler_*`, `nue_resample_buffer`): Kaiser windowed sinc, 4 qualities (36 / 68 / 128 / 260 taps, 60 / 80 / 100 / 120 dB), streaming, ratio can
  change while running, deterministic regardless of how input is chunked (tested bit for bit). Measured on a 1 kHz tone 48 -> 44.1 kHz: 77 / 96 / 124 / 133 dB
  SNR against 55 dB for `nue_audio_convert`. Latency = half the kernel in input frames.
* **Channel mixer** (`nue_chanmix_*`, `nue_audio_convert_hq`): layouts by WAVE speaker mask, BS.775-style down mixes (center and surrounds at -3 dB), optional
  normalisation, LFE inclusion, surround fill for up mixes. Format conversion is identical to `nue_audio_convert`; mono -> stereo agrees; the 5.1 -> stereo gains
  differ on purpose (the engine converter halves the sum).
* **Dynamics**: `nue_compressor` (soft knee, linked detector, auto make-up), `nue_limiter` (look-ahead, sliding-minimum gain so the ceiling holds by construction;
  true-peak mode keeps the reconstructed signal within 0.3 dB of the ceiling), `nue_drc` with modes speakers / headphones / TV / night (32 dB input range becomes
  32 / 28 / 31 / 18 / 5 dB for off / speakers / headphones / TV / night in the test).
* **Loudness**: `nue_loudness` meter (momentary, short term, integrated, LRA, sample / true peak; libebur128), `nue_loudness_analyze`, `nue_loudness_normalization_gain`
  with a true-peak ceiling. 1 kHz stereo -20 dBFS peak reads -19.99 LUFS (expected -20.0).
* **Reverb**: `nue_reverb` is an 8 line FDN (Hadamard feedback, per-line RT60 and HF damping, optional modulation and echo) with early reflections, diffusion and
  tone shelves; parameters are the EFX model. Measured RT60 follows `decay_time` within 1 % (0.50 / 1.50 / 2.99 s); `decay_hf_ratio` darkens the tail
  (high-frequency share 2.0 -> 0.007 at ratio 0.3). `nue_reverb_params_from_i3dl2` / `_from_eax` take the DirectSound / EAX units (mB, percent); 30 I3DL2 presets by
  index or name. `nue_convolver` is a uniformly partitioned FFT convolver (latency = block size) with IR resampling; `nue_ir_generate_room` makes a test IR.
  Limits: `decay_hf_ratio > 1` and `decay_lf_ratio` are accepted but not modelled in the loop (HF cannot decay slower than the mids here); the pan vectors use only
  their left/right part; early reflections are a fixed 8 tap pattern, not a room simulation.

**EAX presets**: the 26 EAX 1.0 environments and the EAX 2.0 preset tables are *not* in the repository (see NUE_RESEARCH_AUDIO.md). A DirectSound layer that receives EAX
property sets converts them with `nue_reverb_params_from_eax`; a table for the environment ids can be added to `i3dl2.cpp` once its numbers come from a source we can name.

## Spatial (`audio_spatial.h`)

`nue_spatial_evaluate` is the scene maths alone (distance models as in `mixer.h`, cone in dB, occlusion, Doppler `(c + vl) / (c + vs)`), usable by the DirectSound layer on
the mixer's own voices. The renderer has three built-in modes and a backend hook: sine-law stereo and pairwise 2D VBAP for any mask (constant power, tested on stereo,
5.1, 7.1), a spherical-head binaural model (Woodworth interaural delay: 32 samples at 90 degrees, head shadow, rear cue) replaced by a measured HRTF set through
`nue_spatial_set_hrtf`, and ambisonic encode (ACN / SN3D, orders 1-3) with a regularised mode-matching decoder. Elevation is encoded in ambisonics only; PAN and the
head model are horizontal. Gains and delays glide between blocks (no clicks). `nue_spatial_backend_ops` is where Windows Sonic / Dolby Atmos for Headphones, the Android
13 `Spatializer`, Apple PHASE, ADM or IAMF renderers attach: they receive the evaluated sources of the block, and if one fails the built-in renderer takes over for
that block. **No platform spatializer is implemented yet**; each needs a request to its platform session once the API surface is confirmed from official documentation.

## Tests

`ctest --preset <name>` runs `nue_audio_test_codec`, `_formats`, `_dsp`, `_spatial`: null backend only, no device, deterministic. Fixtures: `tests/audio/gen_adpcm_vectors.py`
(independent reference decoders, run with plain Python) and `gen_fixtures.py` (needs `pip install soundfile numpy`; Vorbis / Opus / FLAC files made with libsndfile).

## Status

| Item | State |
|---|---|
| 1 codecs and streaming | implemented and tested (Windows x64 and x86 run; Linux x64 not run locally, left to CI) |
| 2 resampler / converter | implemented and tested |
| 3 dynamics, loudness, DRC modes | implemented and tested |
| 4 reverb (FDN, convolution), EFX / I3DL2 / EAX model | implemented and tested; EAX 1/2 preset tables missing |
| 5 spatial: pan, binaural model, ambisonics, hooks | implemented and tested; SOFA loader and platform backends not written |
| 6 ADM, IAMF | not started (candidates in the research document) |
| 7 tier 3 (time stretch, voice chat, LC3, engine sound, haptics) | not started |
| `tools/nue_audio` | `info`, `decode`, `loudness`, `raw` (headerless ADPCM / PCM to WAV); no DSP commands yet |
