# Third-party code and data in the DirectSound layer (`legacy_dsound`)

Rule: every piece of external code or data that enters the tree is listed here with source URL, commit, original path, licence, date and what changed. **The
user reviews licences afterwards; GPL, LGPL and unclear terms are flagged in bold.** Date of every row: 2026-10-25.

## Copied into the repository

**Nothing.** No file, function, table or data set of another project was copied or adapted verbatim into `src/legacy/dsound`, `tests/legacy/dsound`,
`include/nue_legacy/dsound_*` or `docs/*DSOUND*`.

## Origin of the code that is ours

| What | Origin | Licence | Note |
|---|---|---|---|
| `include/nue_legacy/dsound_*.hpp`, `src/legacy/dsound/src/*` (Level A: device, mixer, engine, placement, native sink) | Ported from the private NFS Underground 2 port, branch `port-dsound`, commit `729631c` (`platform/audio/ds_device.*`, `audio_engine.*`, `audio_types.hpp`, `sink.hpp`, `null_sink.*`, `reverb.cpp` EAX 2 mapping), **the user's own work**, rewritten on `nue::audio`. The port's own spatial renderers, FDN reverb, bed matrix and 16-tap resampler are **not** carried over: `nue_audio` provides them. | the repository's licence | game-specific parts (guest memory, launcher glue, import overrides) stay in the private repository |
| `src/legacy/dsound/dll/*` (COM facade, `dsound.dll`) | New code, written against the public `dsound.h` of the Windows SDK and the behaviour measured on the system DirectSound. The semantics come from the port's guest facade (`dsound_facade.*`, same author). | the repository's licence | |
| `tests/legacy/dsound/*` | Ported from the port's `audio_tests.cpp`, `dsound_facade_tests.cpp`, `dsound_differential_tests.cpp` (same author), re-expressed on the real COM interfaces and on `nue::audio`; new: native-stream, DLL and determinism tests. | the repository's licence | |
| `src/legacy/dsound/tools/dsound_inventory.py` | Ported unchanged except for the usage line (same author). Needs `capstone` (not vendored; `pip install capstone`, BSD-3-Clause, only when the tool is run). | the repository's licence | |

## Used from the Windows SDK (system component, not copied, not redistributed)

| Component | Use | Licence |
|---|---|---|
| `dsound.h`, `mmreg.h`, `ksmedia.h`, `dxguid.lib`, `uuid.lib` of the Windows SDK | the interface declarations (vtable order, structures, GUIDs) of the COM facade, compiled into `dsound.dll` | Microsoft SDK licence: headers/import libraries are build-time inputs; nothing from them is shipped in the repository |
| the system `dsound.dll` | loaded at run time by the differential test only, to compare answers; never copied, never distributed, plays only zero-filled buffers | OS component |

## Consulted for behaviour, nothing taken (as reported by the port; re-listed for the review)

| Source | URL | Commit read | Licence | What was read | Effect |
|---|---|---|---|---|---|
| Wine `dlls/dsound` | https://github.com/wine-mirror/wine | `e3c3d348676128bc745c40aa6397b51951ef1cd6` | **LGPL-2.1-or-later** | return codes and argument validation, notification ordering | semantics only; every one of them was re-measured on the real Windows DirectSound by the differential test |
| kcat/dsoal | https://github.com/kcat/dsoal | `e11128250439d586483ee6598d1670ab52ab5e37` | **LGPL-2.1** | DS3D and EAX concepts, EAX to EFX mapping idea | parameter ranges only |
| OpenAL Soft | https://github.com/kcat/openal-soft | not pinned | **LGPL-2 (library GPL v2 + Apache/BSD parts)** | EFX model, size to density idea in `eax2_to_environment` | idea only; the formula is typed from the EAX documentation, not copied |
| libspatialaudio | https://github.com/videolabs/libspatialaudio | not pinned | **LGPL-2.1+ (also commercial)** | binaural/ambisonic layout | nothing used (`nue_audio` has its own) |
| UnleashedRecomp, ReactOS tree | https://github.com/hedge-dev/UnleashedRecomp, https://github.com/reactos/reactos | not pinned | **GPL-3.0 / GPL-2.0** | nothing read for code | do not copy from them |
| codepdbh/nfsu2-recompiled, TalhaMudo/NFSU2Recomp, yugecin/nfsu2-re | github.com | see the port's `THIRD_PARTY_SOURCES.md` | **no licence / proprietary / none stated** | facts about one game's ring behaviour, re-verified in the executable | nothing copied; cannot be |
| dxwrapper | https://github.com/elishacloud/dxwrapper | not pinned | **unclear ("multiple licenses")** | not used | - |
| Microsoft Learn: DirectSound reference | https://learn.microsoft.com/previous-versions/windows/desktop/ee416960 | - | documentation | constants and the cursor definitions | the numbers of `DSERR_*`, `DSBCAPS_*` are the published API values |

## Dependencies this layer adds

None of its own. It links `nue::audio` (branch `audio`: resampler, channel mixer, spatial, reverb; its third-party code is listed in
`docs/THIRD_PARTY_AUDIO.md`) and `nue::nue`. The Windows DLL additionally links `dxguid` and `uuid` from the Windows SDK.

## To flag for the review

* Nothing in this layer is LGPL/GPL code. The **only** non-obvious point: `nue_audio`'s I3DL2 preset table, which `Environment`/`eax2_to_environment`
  feeds, is listed there as cross-checked against an LGPL header (see `THIRD_PARTY_AUDIO.md`); that is the audio session's flag, not ours.
* The semantics the layer reproduces (return codes, cursor margins) are facts about Microsoft's DirectSound measured on Windows 11 through the public API;
  they are not copyrightable code, but the choice of "what to match" was made by observing the real `dsound.dll`'s outputs, which the user may want to
  note for the licence review of the repository.
