# Graphics module (nue_gfx): the bgfx integration, compiled into libnue with -DNUE_WITH_GFX=ON. Owner: the gfx session (docs/GFX.md).
#
#   git submodule update --init --depth 1 third_party/bgfx.cmake && git -C third_party/bgfx.cmake submodule update --init --recursive --depth 1
#
# What it defines
#   nue_gfx_obj          object library with src/gfx/*.cpp (linked into nue like the other modules)
#   bgfx, bx, bimg       from third_party/bgfx.cmake (static libraries; folded into libnue, or installed next to a static SDK and found by nueConfig.cmake)
#   NUE_HAS_GFX=1        compile definition on nue_internal (core.h nue_build_info.has_gfx)
#
# The handle limits are bgfx compile-time constants. The Direct3D 9 layer (src/legacy/d3d9) needs 16384 textures/buffers; it reuses these targets when it
# finds them, so the numbers here are the ones both modules get. Override with -DNUE_GFX_MAX_TEXTURES=... (cache variables).

set(NUE_BGFX_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bgfx.cmake" CACHE PATH "bgfx.cmake checkout (bgfx, bx, bimg) used by the graphics module")
if(NOT EXISTS "${NUE_BGFX_CMAKE_DIR}/bgfx/src/bgfx.cpp")
    message(FATAL_ERROR "NUE_WITH_GFX: ${NUE_BGFX_CMAKE_DIR} is empty. Run: git submodule update --init --depth 1 third_party/bgfx.cmake && "
                        "git -C third_party/bgfx.cmake submodule update --init --recursive --depth 1")
endif()

set(NUE_GFX_MAX_TEXTURES 16384 CACHE STRING "bgfx BGFX_CONFIG_MAX_TEXTURES (handles alive at once)")
set(NUE_GFX_MAX_VERTEX_BUFFERS 16384 CACHE STRING "bgfx BGFX_CONFIG_MAX_VERTEX_BUFFERS")
set(NUE_GFX_MAX_INDEX_BUFFERS 16384 CACHE STRING "bgfx BGFX_CONFIG_MAX_INDEX_BUFFERS")
set(NUE_GFX_MAX_FRAME_BUFFERS 1024 CACHE STRING "bgfx BGFX_CONFIG_MAX_FRAME_BUFFERS")
set(NUE_GFX_MAX_VERTEX_LAYOUTS 1024 CACHE STRING "bgfx BGFX_CONFIG_MAX_VERTEX_LAYOUTS")

if(NOT TARGET bgfx)
    set(BGFX_CONFIG_MAX_TEXTURES ${NUE_GFX_MAX_TEXTURES} CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_VERTEX_BUFFERS ${NUE_GFX_MAX_VERTEX_BUFFERS} CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_INDEX_BUFFERS ${NUE_GFX_MAX_INDEX_BUFFERS} CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_FRAME_BUFFERS ${NUE_GFX_MAX_FRAME_BUFFERS} CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_VERTEX_LAYOUTS ${NUE_GFX_MAX_VERTEX_LAYOUTS} CACHE STRING "" FORCE)
    set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)             # shaderc/texturec/geometryc are build-time tools; shaders ship precompiled (docs/GFX.md)
    set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_RENDERER_WEBGPU OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
    set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)                 # bgfx.cmake's own install would also want bimg_decode/encode; the root CMakeLists installs bgfx, bx, bimg into the nue export set instead
    add_subdirectory(${NUE_BGFX_CMAKE_DIR} ${CMAKE_BINARY_DIR}/bgfx_build EXCLUDE_FROM_ALL)
endif()
# bimg ships its own copy of ARM's astc-encoder (an older version), and the texture module (src/texture) links the current one into the same library: two copies of the
# same global symbols in one process. bimg only uses it to decode ASTC textures that the GPU cannot sample (software emulation); NUE does that in nue_texture
# (transcoding at import, docs/TEXTURE.md), so bimg is built without ASTC decoding and the driver's ASTC support is reported as it is, not as "emulated".
# bimg_decode / bimg_encode (stb, tinyexr, libsquish, nvtt, dav1d...) are not used by nue_gfx and are not installed: keep them out of the default build
foreach(_t bimg_decode bimg_encode)
    if(TARGET ${_t})
        set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL ON)
    endif()
endforeach()
get_target_property(_bimg_sources bimg SOURCES)
list(FILTER _bimg_sources EXCLUDE REGEX "/astc-encoder/")
set_property(TARGET bimg PROPERTY SOURCES ${_bimg_sources})
target_compile_definitions(bimg PRIVATE BIMG_CONFIG_DECODE_ASTC=0)
set(NUE_GFX_BGFX_TARGETS bgfx bx bimg)
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    # bx's SIMD layer uses SSE4.1 intrinsics (_mm_testz_si128, _mm_round_ps): the 32-bit x86 baseline of clang does not enable them
    foreach(_t ${NUE_GFX_BGFX_TARGETS})
        target_compile_options(${_t} PRIVATE -msse4.2)
    endforeach()
endif()
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT NUE_WITH_LEGACY_D3D9)
    # 32-bit Windows needs two workarounds in bgfx (see src/gfx/x86/*.h). When the Direct3D 9 layer is built in the same tree its CMake applies equivalent ones to the
    # same targets (src/legacy/d3d9/CMakeLists.txt), so they are not doubled here.
    foreach(_t ${NUE_GFX_BGFX_TARGETS})
        target_compile_options(${_t} PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/x86/x86_stdcall_fix.h")
    endforeach()
    set_source_files_properties("${NUE_BGFX_CMAKE_DIR}/bx/src/allocator.cpp" TARGET_DIRECTORY bx PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/x86/x86_aligned_alloc.h")
    set_source_files_properties("${NUE_BGFX_CMAKE_DIR}/bgfx/src/bgfx.cpp" TARGET_DIRECTORY bgfx PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/x86/x86_aligned_alloc.h")
endif()
foreach(_t ${NUE_GFX_BGFX_TARGETS})
    set_target_properties(${_t} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

file(GLOB NUE_GFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/*.cpp")
add_library(nue_gfx_obj OBJECT ${NUE_GFX_SOURCES})
target_link_libraries(nue_gfx_obj PUBLIC nue_internal)
target_include_directories(nue_gfx_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/gfx")
# bgfx's public headers (bgfx/bgfx.h, bgfx/platform.h, bx/...) without linking: the libraries are linked into nue itself (see CMakeLists.txt)
target_include_directories(nue_gfx_obj PRIVATE
    "$<TARGET_PROPERTY:bgfx,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:bx,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:bimg,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(nue_internal INTERFACE NUE_HAS_GFX=1)
if(MSVC)
    target_compile_definitions(nue_gfx_obj PRIVATE __STDC_LIMIT_MACROS __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS)
endif()
