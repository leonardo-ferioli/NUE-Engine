# Requests from the assets session

## To core (root `CMakeLists.txt`, install rules)

* **Done as a minimal patch on branch `assets`, please review or adapt:** the root `CMakeLists.txt` gets one block, after the entry stub and before the
  legacy layer:

  ```cmake
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/CMakeLists.txt")
      add_subdirectory(src/assets)
  endif()
  ```

  `src/assets/CMakeLists.txt` defines the static library `nue_assets` (alias `nue::assets`, linking `nue::nue` publicly), the tool `nue_pack` and the
  tests `nue_assets_test_*`, and installs the library into the `nueTargets` export. It does not touch any other target. The public header is
  `include/nue/assets.h`; it is not included by `nue/nue.h` because it needs the separate library.
* Optional: add `nue_pack` to the SDK packages (`install(TARGETS nue_pack ...)`), and `docs/ASSET_FORMATS.md`, `docs/NUEPACK.md` to the installed docs.

## To core (VFS)

* **Transparent pack paths.** `nue_pack_mount_*` (assets) offers named read-only reads with positioned and asynchronous reads, but `nue_file_open("pack:...")`
  cannot see it because the VFS only mounts native directories. Request: a provider hook in `nue_vfs_mount`, e.g. `nue_vfs_mount_provider(root, ops, userdata)`
  with `stat`, `open`, `read_at`, `size`, `list`, `close`; the assets module would supply the ops over `nue_pack_file_*`. Until then games call the
  `nue_pack_*` mount API directly.
* Nothing else: the module uses `nue_file_*`, `nue_io_read_async`, the memory budget (`nue_budget_register/request/report`, budget name `assets`, kind
  `NUE_BUDGET_IMPORT_STAGING`) and `nue_alloc` as they are.

## To the texture session

* A **BC2 (DXT3) encoder** would let a replaced DXT3 texture stay DXT3. Today the NFS writer encodes such a replacement as BC3 (DXT5, same size, same
  alpha capability, supported by D3D9) because `nue_tex_image_convert` has no BC2 target.
* The real textures are in `<workspace>\nfsu2_texture_dump\bun\` (6119 DDS files, see the message sent to that session).

## To the platform sessions

* Nothing. Tests use the null backend only; the module has no OS specific code.
