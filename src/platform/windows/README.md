# NUE platform layer: windows

Owned by the `windows` session. Contract and status: `docs/PLATFORM_WINDOWS.md` (what is implemented, tested and only written), `docs/PORTING.md`.

- `nue_win/*.hpp` + `*.cpp`: the `nue::win` modules (power, cpu, gpu, game mode, display, window attachment, input, audio, storage, diagnostics). No core dependency.
- `platform_windows.cpp`: `nue_platform_windows_get_ops()`.
- `tests/`: console tests (`build_and_test.ps1` builds the repository without SDL3 for x64/x86 with clang and MSVC).
- `../../../samples/windows_probe`: console probe of what the machine supports.
