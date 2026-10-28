# Third-party code used by the graphics module (`nue_gfx`)

Rule: nothing here is copied into the repository tree except where a row says "copied". Everything else is a pinned git submodule (read-only for us) and keeps
its own licence files. The user reviews licences afterwards; this file is the list. Dates are the day the row was written (2026-10-25).

## Linked into `libnue` when `-DNUE_WITH_GFX=ON`

| Component | Where | URL | Pinned commit | Licence | What we use | Changes |
|---|---|---|---|---|---|---|
| bgfx.cmake (CMake wrapper) | `third_party/bgfx.cmake` | https://github.com/bkaradzic/bgfx.cmake | `8d950d6dc3df1d763e7d3655987c89009767a07e` | CC0 1.0 (`LICENSE`) | `add_subdirectory`: builds the three libraries below | none (we set cache variables: handle limits, no tools, no examples, no WebGPU/video) |
| bgfx | `third_party/bgfx.cmake/bgfx` (nested submodule) | https://github.com/bkaradzic/bgfx | `fd541f05865f6e7fbfc302f81f5a71d2b05794eb` | BSD-2-Clause (`bgfx/LICENSE`) | the renderer: Direct3D 11/12, Vulkan, Metal, OpenGL, OpenGL ES, Noop | none |
| bx | `third_party/bgfx.cmake/bx` | https://github.com/bkaradzic/bx | `d4e6ed3cf4f75c355bb04088bc1c13cd919aafa6` | BSD-2-Clause (`bx/LICENSE`) | base library of bgfx (allocators, strings, math, threads) | none |
| bimg | `third_party/bgfx.cmake/bimg` | https://github.com/bkaradzic/bimg | `8e665781eff8e963a04924f1acc1d74324149bcb` | BSD-2-Clause (`bimg/LICENSE`) | image/texture format tables and the decoders bgfx uses for formats a GPU cannot sample | **built without its ASTC decoder** (`BIMG_CONFIG_DECODE_ASTC=0`, its bundled astc-encoder sources removed from the target in `cmake/NueGfx.cmake`): `src/texture` links the current astc-encoder (5.7.0) into the same library and two copies of the same global symbols cannot coexist |
| SDL3 | `third_party/SDL` | https://github.com/libsdl-org/SDL | `829a65d769d935c4852f8159e964312c0957260a` | zlib | window handles (already a dependency of the engine, not new) | none |

Header-only third-party code that comes with bgfx and is compiled into bgfx (not separate libraries): `bgfx/3rdparty/directx-headers` (MIT, Microsoft),
`bgfx/3rdparty/khronos` (EGL/GLES/GL/Vulkan headers, Apache-2.0 OR MIT), `bgfx/3rdparty/renderdoc/renderdoc_app.h` (MIT). Each keeps its licence header or
`LICENSE` file inside the submodule. The bgfx tools (shaderc, texturec, geometryc) are not built (`BGFX_BUILD_TOOLS=OFF`), so glslang, spirv-tools, spirv-cross,
dear-imgui, meshoptimizer, etc. are not compiled into anything.

## Used by the sample and the local test (referenced in place, not copied)

| File | Path in the submodule | Licence | Use |
|---|---|---|---|
| `vs_ocornut_imgui.bin.h`, `fs_ocornut_imgui.bin.h` | `third_party/bgfx.cmake/bgfx/examples/common/imgui/` | BSD-2-Clause (bgfx) | pre-compiled vertex + fragment shader (binaries for DXBC, DXIL, SPIR-V, Metal, GLSL, ESSL, WGSL): `samples/hello_gfx/draw.hpp` includes them from the submodule, so `hello_gfx` needs no shader compiler and runs on every renderer. The shader sources (`.sc`) next to them document what the binaries do. |

## Ideas taken from, not code copied

* bgfx's own `examples/common/entry` (SDL window glue): the way a native handle goes into `bgfx::Init::swapChain.nwh/ndt` and `platformData.type` (Wayland) is the same
  as there; the code in `src/gfx/gfx_bgfx.cpp` is written for this engine.
* bgfx's `CallbackI` stub (`bgfx/src/...`) as the template for the callback class (log, fatal, cache, screenshot).

## Not used (evaluated)

* `bgfx::createEmbeddedShader` tables other than the imgui pair, nanovg shaders: not needed.
* `bimg_encode` / `bimg_decode` (stb, libsquish, nvtt, tinyexr, lodepng): not linked; image work belongs to `nue_texture`.
