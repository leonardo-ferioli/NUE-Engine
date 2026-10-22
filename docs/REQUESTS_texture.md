# Requests from the texture session

## To nue-platform sessions (windows, linux, android, ios)

* **Emulation flag.** `nue_gpu_caps.format[]` cannot say that a family is only *emulated* (the driver decompresses at upload: ASTC/ETC2 on desktop GL, BC on some
  GLES drivers). The planner treats emulated formats as unusable because they save no memory. Please add a `NUE_FMTSUP_EMULATED` bit to `gfx.h` (core) and set it from the
  probe where it is known (`bgfx::Caps::formats[...] & BGFX_CAPS_FORMAT_TEXTURE_2D_EMULATED`, Vulkan `VK_FORMAT_FEATURE_*` + vendor knowledge, GL extensions).
* Report device classes that do not fit `nue_tex_device_class` (the presets in `src/texture/caps.cpp` are assumptions, not measurements).

## To the legacy D3D9 layer owners

* See `docs/TEXTURE.md` section 9 (patch proposal). The three calls to wire: `nue_tex_d3d9_texture_create`, `nue_tex_d3d9_convert_level`, `nue_tex_d3d9_texture_destroy`; the format table
  `nue_tex_d3d9_format_table()` replaces the layer's own D3DFMT knowledge.
* A DDS dump (or a list of extracted textures) of the real game would let `nue_texconv scan <dir>` and `nue_texture_test_real_textures <dir>` report real memory and quality numbers.

## To core

* Nothing pending. `include/nue/nue.h` already includes `nue/texture.h`; the root contract (`nue_texture_obj`) works as documented (13/13 tests pass in the engine build).
