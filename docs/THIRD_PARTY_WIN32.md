# Third-party sources in legacy_win32 / legacy_dinput

**No code was copied from another project.** Every module, test and table in `include/nue_legacy/win32`, `include/nue_legacy/dinput.hpp`, `src/legacy/win32` and
`src/legacy/dinput` is original. `src/legacy/win32/src/text_data.inc` is generated from the answers of the running Windows (its NLS functions) by
`tools/gen_text_data.cpp`: data about the system, not code of a project.

Read, then written independently (behaviour rules only, no text copied):

| Project | What was read | Where the behaviour went |
|---|---|---|
| wibo (MIT, https://github.com/decompals/wibo, commit 355e082) | path conversion rules, handle table shape | `win32/files`, `win32/handles` |

Behaviour was checked against the real Windows (kernel32, user32, advapi32, dinput8) by the `*_windows` tests, not against Wine's source. Projects whose licence
forbids copying into NUE (GPL: ReactOS, UnleashedRecomp, ssbu-recomp; no licence file: SR, uwin; LGPL: Wine) are listed in `docs/LEGACY_WIN32_RESEARCH.md`: do not copy from them.
