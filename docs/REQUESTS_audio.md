# Requests from the audio session

The audio module (`src/audio`, library `nue::audio`, headers `include/nue/audio_*.h`) builds on top of `audio.h` and `mixer.h` and changes neither.
See [AUDIO.md](AUDIO.md) for what it contains.

## To core (root `CMakeLists.txt`, install rules)

* **Done as a minimal patch on branch `audio`, please review or adapt:** the root `CMakeLists.txt` gets one block, after the asset module and before the
  legacy layer (same pattern as `src/assets`):

  ```cmake
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/CMakeLists.txt")
      add_subdirectory(src/audio)
  endif()
  ```

  `src/audio/CMakeLists.txt` defines the static library `nue_audio` (alias `nue::audio`, linking `nue::nue` publicly), builds the vendored third-party C code
  (`src/audio/third_party/*`) into an object library with its own warning level, registers `tests/audio` (ctest names `nue_audio_test_*`) and the tool
  `tools/nue_audio`, and installs the library into the `nueTargets` export. Public headers: `include/nue/audio_codec.h`, `audio_dsp.h`, `audio_spatial.h`;
  none is included by `nue/nue.h` because they need the separate library.
* Optional: add `docs/AUDIO.md` to the installed docs.

## To core (mixer)

* **Nothing blocking.** Everything plugs in through the public API: DSP processors expose `nue_dsp_callback` adapters (`nue_reverb_dsp`, `nue_compressor_dsp`,
  `nue_limiter_dsp`, ...) for `NUE_EFFECT_CALLBACK` bus slots; codecs feed `NUE_VOICE_CALLBACK` voices through `nue_adec_pull_s16/f32` (or buffers through
  `nue_adec_decode_all_*`).
* Observed (low priority): a `NUE_VOICE_CALLBACK` voice whose source ends does not render its last source frame at 1:1 pitch, because the linear interpolator
  needs one frame of look-ahead (`tests/audio/test_codec.cpp`, `decoder_feeds_a_callback_voice`, compares all frames but the last).
* Later (tier 2, spatial rendering): a bus-level hook that lets a spatial stage see the per-voice 3D parameters before the voices are summed (today a
  `nue_dsp_callback` only sees the mixed bus). Proposal will follow with `audio_spatial.h`; until then the spatial stage works on voices the application
  routes itself (see AUDIO.md).

## To the platform sessions

* Nothing yet. When the spatial module lands, optional backends for the OS spatializers (Windows Sonic / Dolby Atmos for Headphones, Android 13
  `Spatializer`, Apple PHASE) will be requested here with the exact entry points.

## To the nue-assets session

* The decoders are ready for conversion tools: `nue_adec_open_raw_memory` with `nue_adec_raw_desc` (codec, channels, rate, interleave, loop points, DSP
  coefficients) and `nue_adec_decode_all_s16`. EA container parsing (SCHl / ABK) stays in nue_assets; the ADPCM frame decoders it needs are
  `nue_adpcm_ea_xa_decode_*` and `nue_adpcm_ea_xas_decode_block`.
