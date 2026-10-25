# Requests from the legacy_dsound session

The layer is described in [LEGACY_DSOUND.md](LEGACY_DSOUND.md). Branch of this session: `legacy-dsound` (it contains a merge of `audio` at `c0e2702`).

## To core (root files)

1. **Merge order.** `legacy-dsound` needs `src/audio` (`nue::audio`) from branch `audio`; the merge commit is already in this branch, so merging `audio` into
   `main` first (or together) is enough. No file of `audio` was changed here. The only conflict of that merge was the generated ABI reference version lines
   (`tests/abi/reference/*.txt`); it was resolved to `assets 2 / audio 1 / core 4099`. Please re-run `NUE_ABI_UPDATE=1 ctest -R nue_test_abi` on the merge
   result if `main` moved.
2. **Root `CMakeLists.txt`, two additions (done, minimal):** `option(NUE_WITH_LEGACY_DSOUND ... OFF)` next to the D3D9 option, and
   `if(NUE_WITH_LEGACY_DSOUND) add_subdirectory(src/legacy/dsound) endif()` after the D3D9 block. Nothing else outside this session's folders was touched.
3. **CI.** Add `-DNUE_WITH_LEGACY_DSOUND=ON` to the Windows x86, Windows x64 and Linux x64 jobs (the Level A library and its tests are portable; the COM
   facade, `dsound.dll` and their tests are Windows only and skipped elsewhere by the CMake file). The ctest names are `nue_dsound_device`, `nue_dsound_nue`
   (everywhere), `nue_dsound_com`, `nue_dsound_dll_test` (Windows). The differential test `nue_dsound_differential` is built on Windows but **not** in ctest
   (it runs in real time for about 3 s and needs an audio endpoint; `-DNUE_DSOUND_SYSTEM_TESTS=ON` registers it).
4. **Install/export.** `nue_legacy_dsound` is a static library with the alias `nue::legacy_dsound`; it is not in `nueTargets`. If the SDK package should carry
   it (component `legacy_dsound` as `docs/ABI.md` 3 says), add it to the export set; the headers `include/nue_legacy/dsound_*.hpp` are already covered by the
   `install(DIRECTORY include/nue_legacy ...)` rule. `dsound.dll` is a deployment artifact (copy next to a game), not an SDK file.
5. **Docs.** `docs/ROADMAP.md` lists `legacy_dsound` as "Planned": it can move to "In development". Add `LEGACY_DSOUND.md` and `THIRD_PARTY_DSOUND.md` to the installed
   docs list if wanted.
6. **ABI guard.** Nothing under `include/nue/` changed, so no version bump and no `API_CHANGES.md` line was needed. The new public headers
   (`include/nue_legacy/dsound_*.hpp`) are C++ and are not read by `tools/abi/gen_abi_probe.py`; they are the layer's own API (versioning below).

## To core (mixer: what would let the layer sit on `nue_mixer` voices instead of its own DirectSound mixer)

The layer renders DirectSound voices itself because `mixer.h` cannot express four DirectSound behaviours. None of them blocks anything; they are the list to
close if the layer should ever use `nue_mixer` directly.

| Gap | DirectSound behaviour | What `mixer.h` does |
|---|---|---|
| one-shot ring | `Play` without `DSBPLAY_LOOPING` stops at the end of the buffer, the position goes back to 0, a stop notification fires | a `NUE_VOICE_RING` voice always loops |
| pan law | `SetPan` attenuates only the far channel by `|pan|` mB; a centred sound is at unity in both channels | constant power: -3 dB per channel at the centre (the layer would lower every centred sound by 3 dB) |
| write cursor | at least about 30 ms ahead of the play cursor (measured on Windows 11) | play + pending block + device latency, which can be less |
| notifications | positions in bytes, plus `DSBPN_OFFSETSTOP` at stop | `nue_voice_ring_set_notifications` has no stop notification |

## To the audio session (`nue-audio`)

Sent as a message as well. Nothing is duplicated: the layer calls `nue_resampler`, `nue_chanmix`, `nue_spatial_*` and `nue_reverb_*`. Open items:

* **EAX 1/2/3 environment tables** (`EAX_ENVIRONMENT_*` ids to parameters) are missing in `i3dl2.cpp`; the layer's `eax2_to_environment()` takes the *listener
  properties* an application sends, not an environment id. If the table appears, an `IKsPropertySet` facade can map ids too.
* **Doppler convention:** the layer relies on `nue_spatial_evaluate` returning `(c + vl)/(c + vs)` with velocities projected on listener -> source (checked by a test
  in `ds_device_tests.cpp`: an approaching source at c/10 plays 1/0.9 faster, a receding one 1/1.1).
* **Stereo balance pan:** DirectSound's 3D default on stereo is a balance law (the near channel stays at unity). That law is a 20-line function inside
  `placement.cpp`; it is not DirectSound-specific and could become `NUE_SPATIAL_BALANCE` in `audio_spatial.h` if other layers want it. Until then it stays here.
* `nue_resampler` output position after priming is within one output frame of the ideal one (the layer absorbs it in its cursor bookkeeping); a
  "start at fractional offset" call would remove the last bit of guessing.

## To the platform sessions (windows, linux, android, ios)

* **Endpoint channel count and mask.** Measured on this machine (Windows 11, Realtek stereo endpoint, SDL3 WASAPI): a stream opened with 6 channels reports
  `device_spec.channels == 6` from `nue_audio_stream_get_spec` (the platform converts), so the *device* spec of a stream is the format the stream was opened
  with, not the endpoint's. `NueSink` therefore reads `nue_audio_get_device_info(0).preferred.channels` and never asks for more channels than that, so the
  layer's own ITU down-mix (`nue_chanmix`) and spatial stage run instead of the platform converter (`SinkDesc::match_device_channels`, default on). Please
  confirm `preferred` is the endpoint's mix format on every platform (a 5.1 endpoint must say 6), and the channel mask if it can be exposed.
* **Default device changes.** `docs`/`audio.h` say streams on device 0 follow the system default. The layer reads latency once at open (`apply_output`);
  if the new device has a different latency or rate it would like a notification (`NUE_EVENT_AUDIO_DEFAULT_CHANGED` is informational only). A cheap answer
  is fine: the engine could re-read `nue_audio_stream_get_latency` when it gets that event.
* **Spatial audio backends** (`nue_spatial_backend_ops`): the layer passes `SpatialOptions::backend` straight through. A Windows backend (ISpatialAudioClient /
  Windows Sonic) belongs to the windows session once the audio session confirms the entry points; not started here.
* **Exclusive / low latency** (`NUE_AUDIO_LOW_LATENCY`) is supported by `SinkDesc::low_latency` and untested on real devices.

## To the legacy-core work (planned; not started)

The guest-pointer facade of a recompiled 32-bit game needs five things from `legacy_core`; the exact interface is written in
[LEGACY_DSOUND.md](LEGACY_DSOUND.md), section "Guest facade seam": guest memory (translate, allocate), a handle/COM-object table with generation checks, call
tokens for the 75 vtable slots, `override_import(module, ordinal)`, and host services (event signalling, window validation). Nothing here depends on it.

## To the d3d9 / platform-api sessions

None. The layer does not touch the window, input or graphics. `SetCooperativeLevel(hwnd)` is validated with `IsWindow` in the Windows facade; the guest facade asks
`legacy_core` for the same check.
