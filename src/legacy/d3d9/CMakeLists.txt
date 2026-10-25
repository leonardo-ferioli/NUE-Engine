# nue_d3d9 (the engine's legacy_d3d9 module): a general Direct3D 9 -> bgfx translator library (no game, no guest, no Windows types in its
# public headers), the bgfx adapter, the drop-in d3d9.dll (Windows) and their tools.
#   From the engine:  cmake --preset windows-x64 -DNUE_WITH_LEGACY_D3D9=ON   (windows-x86 for the 32-bit d3d9.dll of the game)
#   On its own:       cmake -S src/legacy/d3d9 -B build_d3d9 -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
# Needs the bgfx.cmake submodule: git submodule update --init --recursive --depth 1 third_party/bgfx.cmake
cmake_minimum_required(VERSION 3.21)
project(nue_d3d9 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(NUE_D3D9_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)   # src/legacy/d3d9 -> repository root
set(NUE_BGFX_CMAKE_DIR "${NUE_D3D9_REPO_ROOT}/third_party/bgfx.cmake" CACHE PATH "bgfx.cmake checkout (bgfx, bx, bimg) used by the Direct3D 9 layer")
option(NUE_WITH_BGFX "Build the bgfx adapter and the tools that draw" ON)
option(NUE_STATIC_CRT "Link the C/C++ runtime statically (use it for the 32-bit d3d9.dll so the game folder needs no redistributable)" OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # 32-bit x86: the ASTC decoder of bimg wants the 64-bit popcount intrinsic, which does not exist there; SSE 4.2 for the 32-bit popcount
    add_compile_options("-D_mm_popcnt_u64(v)=(_mm_popcnt_u32((unsigned)(v))+_mm_popcnt_u32((unsigned)((unsigned long long)(v)>>32)))" -msse4.2)
endif()
if(NUE_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # /MT: no Visual C++ redistributable needed next to the game
endif()

# SPIRV-Cross (Apache-2.0), vendored by bgfx: SPIR-V -> GLSL ES / GLSL / MSL / HLSL.
set(BGFX_ROOT "${NUE_BGFX_CMAKE_DIR}")
if(NOT EXISTS "${BGFX_ROOT}/bgfx/3rdparty/spirv-cross/spirv_cross.cpp")
    message(FATAL_ERROR "legacy_d3d9: ${BGFX_ROOT} is empty; run: git submodule update --init --recursive --depth 1 third_party/bgfx.cmake")
endif()
set(SPIRV_CROSS "${BGFX_ROOT}/bgfx/3rdparty/spirv-cross")
add_library(spirv_cross STATIC
    ${SPIRV_CROSS}/spirv_cfg.cpp ${SPIRV_CROSS}/spirv_cpp.cpp ${SPIRV_CROSS}/spirv_cross.cpp ${SPIRV_CROSS}/spirv_cross_parsed_ir.cpp
    ${SPIRV_CROSS}/spirv_cross_util.cpp ${SPIRV_CROSS}/spirv_glsl.cpp ${SPIRV_CROSS}/spirv_hlsl.cpp ${SPIRV_CROSS}/spirv_msl.cpp
    ${SPIRV_CROSS}/spirv_parser.cpp ${SPIRV_CROSS}/spirv_reflect.cpp)
target_include_directories(spirv_cross PUBLIC ${SPIRV_CROSS})
target_compile_options(spirv_cross PRIVATE -O2 -w)

# Core: Direct3D 9 semantics (layer), shader translator, trace reader/replay, null Gpu. Portable C++20.
file(GLOB NUE_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp src/shader/*.cpp src/layer/*.cpp)
add_library(nue_d3d9 STATIC ${NUE_CORE_SOURCES})
target_include_directories(nue_d3d9 PUBLIC include PRIVATE src)
target_link_libraries(nue_d3d9 PUBLIC spirv_cross)
target_compile_definitions(nue_d3d9 PRIVATE _CRT_SECURE_NO_WARNINGS)
if(WIN32)
    target_compile_options(nue_d3d9 PRIVATE -g -Xclang -gcodeview)   # symbols for the sampling profiler of nue_replay --bench --sample
endif()

if(NUE_WITH_BGFX)
    # Handle limits of bgfx (compile-time): the defaults of 4096 textures / vertex buffers / index buffers and 128 frame buffers / 64 vertex layouts are
    # too small for a race of this game (thousands of textures and buffers alive at once); a handle past the limit is invalid and the object is silently missing.
    set(BGFX_CONFIG_MAX_TEXTURES 16384 CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_VERTEX_BUFFERS 16384 CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_INDEX_BUFFERS 16384 CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_FRAME_BUFFERS 1024 CACHE STRING "" FORCE)
    set(BGFX_CONFIG_MAX_VERTEX_LAYOUTS 1024 CACHE STRING "" FORCE)
    set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
    set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_RENDERER_WEBGPU OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
    if(NOT TARGET bgfx)   # another module of the engine may have added bgfx already
        add_subdirectory(${BGFX_ROOT} ${CMAKE_BINARY_DIR}/bgfx_build EXCLUDE_FROM_ALL)
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        foreach(t bx bimg bgfx)
            target_compile_options(${t} PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/dll/x86_stdcall_fix.h)
        endforeach()
    endif()
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        # bgfx's default allocator must hand out 16-byte aligned memory on 32-bit x86 (see dll/x86_aligned_alloc.h)
        set_source_files_properties(${BGFX_ROOT}/bx/src/allocator.cpp TARGET_DIRECTORY bx PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/dll/x86_aligned_alloc.h")
        set_source_files_properties(${BGFX_ROOT}/bgfx/src/bgfx.cpp TARGET_DIRECTORY bgfx PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/dll/x86_aligned_alloc.h")
    endif()
    file(GLOB NUE_BGFX_SOURCES CONFIGURE_DEPENDS bgfx/*.cpp)
    add_library(nue_d3d9_bgfx STATIC ${NUE_BGFX_SOURCES})
    target_include_directories(nue_d3d9_bgfx PUBLIC include PRIVATE src)
    target_link_libraries(nue_d3d9_bgfx PUBLIC nue_d3d9 bgfx bx bimg)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_options(nue_d3d9_bgfx PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/dll/x86_stdcall_fix.h)
    endif()
    target_compile_definitions(nue_d3d9_bgfx PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(WIN32)
        target_compile_options(nue_d3d9_bgfx PRIVATE -g -Xclang -gcodeview)
    endif()

endif()

# The drop-in d3d9.dll (Windows): the COM objects of Direct3D 9 on the layer + bgfx. Copy it next to the game's executable.
if(WIN32 AND NUE_WITH_BGFX)
    add_library(nue_d3d9_dll SHARED dll/main.cpp dll/device.cpp dll/resources.cpp dll/threaded.cpp dll/spikes.cpp)
    set_target_properties(nue_d3d9_dll PROPERTIES OUTPUT_NAME d3d9 PREFIX "" ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/dll_import)
    target_include_directories(nue_d3d9_dll PRIVATE dll src)
    target_link_libraries(nue_d3d9_dll PRIVATE nue_d3d9_bgfx user32 gdi32 dxguid)
    target_compile_definitions(nue_d3d9_dll PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(nue_d3d9_dll PRIVATE -Wno-microsoft-exception-spec)
    # window behaviour of the dll with windows that are never shown: nue_window_test <path to d3d9.dll>
    add_executable(nue_window_test tools/window_test.cpp)
    target_link_libraries(nue_window_test PRIVATE user32)
    target_include_directories(nue_window_test PRIVATE include)
    target_compile_definitions(nue_window_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    # every texture format and mip level through the dll and through the system runtime, compared: nue_format_test <path to d3d9.dll>
    add_executable(nue_format_test tools/format_test.cpp)
    target_link_libraries(nue_format_test PRIVATE user32)
    target_compile_definitions(nue_format_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    # render states and fixed-function texture stages through the dll and the system runtime, compared: nue_state_test <path to d3d9.dll> [filter]
    add_executable(nue_state_test tools/state_test.cpp)
    target_link_libraries(nue_state_test PRIVATE user32)
    target_compile_definitions(nue_state_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    # device state right after CreateDevice / Reset, real runtime against the dll: nue_defaults_test <path to d3d9.dll>
    add_executable(nue_defaults_test tools/defaults_test.cpp)
    target_link_libraries(nue_defaults_test PRIVATE user32)
    target_compile_definitions(nue_defaults_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(nue_replay tools/replay.cpp)
target_link_libraries(nue_replay PRIVATE nue_d3d9)
target_compile_definitions(nue_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
if(NUE_WITH_BGFX)
    target_sources(nue_replay PRIVATE tools/replay_bgfx.cpp)
    target_link_libraries(nue_replay PRIVATE nue_d3d9_bgfx)
    target_compile_definitions(nue_replay PRIVATE NUE_HAVE_BGFX=1)
    if(WIN32)
        target_sources(nue_replay PRIVATE tools/native_d3d9.cpp)
        target_link_libraries(nue_replay PRIVATE user32 gdi32 d3d9 dbghelp winmm)
        target_compile_options(nue_replay PRIVATE -g -Xclang -gcodeview)
        target_link_options(nue_replay PRIVATE -Wl,/DEBUG:FULL)
    endif()
endif()

# console-only unit tests (no window, no GPU): part of ctest. The tools above (nue_state_test and the others) need a GPU and the system
# Direct3D 9 and open hidden windows: they are run by hand with the path of d3d9.dll, never from CI.
enable_testing()
add_executable(nue_tests tests/tests.cpp)
target_link_libraries(nue_tests PRIVATE nue_d3d9)
target_include_directories(nue_tests PRIVATE src)
target_compile_definitions(nue_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME nue_tests COMMAND nue_tests)
if(NUE_WITH_BGFX)
    # the bgfx adapter on bgfx's Noop renderer (no window, no GPU): external_init (bgfx owned by the engine) and the adapter's own init
    add_executable(nue_bgfx_tests tests/bgfx_tests.cpp)
    target_link_libraries(nue_bgfx_tests PRIVATE nue_d3d9_bgfx)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        target_compile_options(nue_bgfx_tests PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/dll/x86_stdcall_fix.h)
    endif()
    add_test(NAME nue_bgfx_tests COMMAND nue_bgfx_tests)
endif()
