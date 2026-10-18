# nue_d3d9: a general Direct3D 9 -> bgfx translator library (no game, no guest, no Windows types in its public headers).
#   cmake -S nue_d3d9 -B build_nue -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
#   cmake --build build_nue
cmake_minimum_required(VERSION 3.21)
project(nue_d3d9 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
option(NUE_WITH_BGFX "Build the bgfx adapter and the tools that draw" ON)

# SPIRV-Cross (Apache-2.0), vendored by bgfx: SPIR-V -> GLSL ES / GLSL / MSL / HLSL.
set(BGFX_ROOT "${REPO}/third_party/bgfx.cmake")
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

if(NUE_WITH_BGFX)
    set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BGFX_INSTALL OFF CACHE BOOL "" FORCE)
    set(BGFX_CUSTOM_TARGETS OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_RENDERER_WEBGPU OFF CACHE BOOL "" FORCE)
    set(BGFX_CONFIG_VIDEO OFF CACHE BOOL "" FORCE)
    add_subdirectory(${BGFX_ROOT} ${CMAKE_BINARY_DIR}/bgfx_build EXCLUDE_FROM_ALL)
    file(GLOB NUE_BGFX_SOURCES CONFIGURE_DEPENDS bgfx/*.cpp)
    add_library(nue_d3d9_bgfx STATIC ${NUE_BGFX_SOURCES})
    target_include_directories(nue_d3d9_bgfx PUBLIC include PRIVATE src)
    target_link_libraries(nue_d3d9_bgfx PUBLIC nue_d3d9 bgfx bx bimg)
    target_compile_definitions(nue_d3d9_bgfx PRIVATE _CRT_SECURE_NO_WARNINGS)

endif()

add_executable(nue_replay tools/replay.cpp)
target_link_libraries(nue_replay PRIVATE nue_d3d9)
target_compile_definitions(nue_replay PRIVATE _CRT_SECURE_NO_WARNINGS)
if(NUE_WITH_BGFX)
    target_sources(nue_replay PRIVATE tools/replay_bgfx.cpp)
    target_link_libraries(nue_replay PRIVATE nue_d3d9_bgfx)
    target_compile_definitions(nue_replay PRIVATE NUE_HAVE_BGFX=1)
    if(WIN32)
        target_link_libraries(nue_replay PRIVATE user32 gdi32)
    endif()
endif()

enable_testing()
add_executable(nue_tests tests/tests.cpp)
target_link_libraries(nue_tests PRIVATE nue_d3d9)
target_compile_definitions(nue_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME nue_tests COMMAND nue_tests)
