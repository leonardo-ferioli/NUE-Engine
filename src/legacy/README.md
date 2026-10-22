# src/legacy

Reserved for libnue_legacy: legacy_win32, legacy_d3d9 (the nue_d3d9 translator), legacy_dsound, legacy_dinput. Built on top of the native ABI only (never the reverse). See docs/ABI.md.
A CMakeLists.txt here must define the target `nue_legacy` (linking nue::nue) and the alias `nue::legacy`.
