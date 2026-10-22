# NUE platform layer: linux

Owned by the `nue-linux` session. The core session creates only this stub.

Contract (details: `docs/PORTING.md`, `src/core/internal/platform_ops.h`):

- Add `CMakeLists.txt` here. It is included with `add_subdirectory` when the target OS is linux; it adds sources with
  `target_sources(nue_platform_obj PRIVATE ...)` and may link libraries with `target_link_libraries(nue_platform_obj PUBLIC ...)`.
- Implement `const nue_platform_ops* nue_platform_linux_get_ops(void)` returning a static `nue_platform_ops` table.
  Leave an entry NULL when the OS offers nothing; never fake a capability.
- Optional OS-only API: publish named extensions with `nue_internal_register_extension("nue.linux.v1", table)` and put the header in
  `include/nue_ext_linux.h` (installed under `nue/ext/`).
- Keep `docs/STATUS.md` rows for linux honest: ask the core session to update them or send the text.
