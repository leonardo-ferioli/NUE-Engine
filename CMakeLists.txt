# NUE engine: cross-platform runtime + SDK + C API (SDL3 base, own code where SDL3 is not enough).
#
#   cmake --preset windows-x64            (see CMakePresets.json: windows-x86, windows-x64, linux-x64, android-arm64, ios-arm64)
#   cmake --build --preset windows-x64
#   ctest --preset windows-x64
#
# Targets
#   nue::nue      the library (C API + C++ wrapper headers), static or shared (-DNUE_BUILD_SHARED=ON)
#   nue::entry    optional entry-point stub for apps that define `int nue_main(int, char**)` (see include/nue/entry.h)
#   nue_sample_*  samples, nue_test_* tests (ctest)
#
# Ownership: this file and src/core, src/platform/sdl3, include, tests, samples, docs, cmake are the CORE session's.
# src/platform/<os>/ belongs to the platform sessions; see docs/PORTING.md for the contract of its CMakeLists.txt.
cmake_minimum_required(VERSION 3.21)

project(nue VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(NUE_IS_TOP_LEVEL ON)
else()
    set(NUE_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------------------------------- options
option(NUE_BUILD_SHARED "Build libnue as a shared library (nue.dll / libnue.so)" OFF)
option(NUE_BUILD_TESTS "Build the ctest suite (console only, null backend / SDL dummy drivers)" ${NUE_IS_TOP_LEVEL})
option(NUE_BUILD_SAMPLES "Build the samples (they open windows when run: never run them from CI)" ${NUE_IS_TOP_LEVEL})
option(NUE_BACKEND_SDL3 "Compile the SDL3 backend (needs third_party/SDL, see docs/PORTING.md)" ON)
option(NUE_BUILD_LEGACY "Build the legacy ABI layer (src/legacy -> libnue_legacy) when it exists; native games never link it" ON)
option(NUE_WITH_GFX "Compile the bgfx graphics module (nue_gfx); needs bgfx.cmake" OFF)
option(NUE_WITH_LEGACY_D3D9 "Build the legacy Direct3D 9 layer (src/legacy/d3d9: translator, bgfx adapter, drop-in d3d9.dll on Windows); needs third_party/bgfx.cmake" OFF)
option(NUE_STATIC_CRT "MSVC/clang-cl/Windows: link the C runtime statically (no redistributable next to the game)" OFF)
option(NUE_INSTALL "Generate install and package-config rules" ${NUE_IS_TOP_LEVEL})
option(NUE_WARNINGS_AS_ERRORS "Treat warnings as errors in the engine's own sources" OFF)
set(NUE_GIT_COMMIT "" CACHE STRING "Commit id compiled into nue_get_build_info (auto-detected when empty)")

if(NUE_STATIC_CRT AND WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------------------------------- platform
if(ANDROID)
    set(NUE_OS android)
elseif(IOS OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(NUE_OS ios)
elseif(WIN32)
    set(NUE_OS windows)
elseif(APPLE)
    set(NUE_OS macos)
elseif(UNIX)
    set(NUE_OS linux)
else()
    message(FATAL_ERROR "NUE: unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()
message(STATUS "NUE: target OS ${NUE_OS}, pointer size ${CMAKE_SIZEOF_VOID_P}, build type ${CMAKE_BUILD_TYPE}")

# Apple platform layers are Objective-C++: enable the languages when the toolchain has them (the iOS/macOS fragment may rely on it).
if(APPLE)
    include(CheckLanguage)
    check_language(OBJC)
    if(CMAKE_OBJC_COMPILER)
        enable_language(OBJC)
    endif()
    check_language(OBJCXX)
    if(CMAKE_OBJCXX_COMPILER)
        enable_language(OBJCXX)
    endif()
endif()

if(NOT NUE_GIT_COMMIT)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
                        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                        OUTPUT_VARIABLE NUE_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(NOT NUE_GIT_COMMIT)
        set(NUE_GIT_COMMIT "unknown")
    endif()
endif()

# ---------------------------------------------------------------------------------------------------- SDL3 (pinned submodule, static)
set(NUE_HAVE_SDL3 OFF)
if(NUE_BACKEND_SDL3)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt")
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL ${NUE_INSTALL} CACHE BOOL "" FORCE)   # a static libnue needs SDL3 in the SDK: it is installed next to it and found with find_dependency
        set(SDL_DISABLE_INSTALL_DOCS ON CACHE BOOL "" FORCE)
        if(NUE_INSTALL)
            add_subdirectory(third_party/SDL)               # its install rules are part of the SDK (static libnue needs it)
        else()
            add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
        endif()
        set(NUE_HAVE_SDL3 ON)
    else()
        message(STATUS "NUE: third_party/SDL is empty -> SDL3 backend disabled (git submodule update --init --depth 1 nue_engine/third_party/SDL)")
    endif()
endif()

# ---------------------------------------------------------------------------------------------------- shared include/compile settings
add_library(nue_internal INTERFACE)
target_include_directories(nue_internal INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core")
if(NUE_BUILD_SHARED)
    target_compile_definitions(nue_internal INTERFACE NUE_BUILD_SHARED)
endif()
if(NUE_HAVE_SDL3)
    target_compile_definitions(nue_internal INTERFACE NUE_HAS_SDL3=1)
endif()
if(MSVC)
    target_compile_options(nue_internal INTERFACE /W4 /permissive- /utf-8)
    target_compile_definitions(nue_internal INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(NUE_WARNINGS_AS_ERRORS)
        target_compile_options(nue_internal INTERFACE /WX)
    endif()
else()
    target_compile_options(nue_internal INTERFACE -Wall -Wextra -Wno-unused-parameter)
    if(WIN32)
        target_compile_definitions(nue_internal INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(NUE_WARNINGS_AS_ERRORS)
        target_compile_options(nue_internal INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------------------------------- core (platform independent) + null backend
file(GLOB NUE_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c")
add_library(nue_core_obj OBJECT ${NUE_CORE_SOURCES})
target_link_libraries(nue_core_obj PUBLIC nue_internal)
target_compile_definitions(nue_core_obj PRIVATE
    NUE_GIT_COMMIT="${NUE_GIT_COMMIT}"
    NUE_BUILD_TYPE_STRING="$<CONFIG>"
    NUE_VERSION_STRING_LITERAL="${PROJECT_VERSION}")

# ctest registration must be enabled before any fragment (platform layers register their own tests with add_test)
if(NUE_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------------------------------- platform layer (owned by the platform sessions)
add_library(nue_platform_obj OBJECT)
target_link_libraries(nue_platform_obj PUBLIC nue_internal)
string(TOUPPER "${NUE_OS}" NUE_OS_UPPER)
set(NUE_HAVE_PLATFORM_LAYER OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${NUE_OS}/CMakeLists.txt")
    # Contract: the fragment adds its sources with target_sources(nue_platform_obj PRIVATE ...) and may link extra libraries
    # with target_link_libraries(nue_platform_obj PUBLIC ...). It must define nue_platform_<os>_get_ops().
    add_subdirectory(src/platform/${NUE_OS})
    target_compile_definitions(nue_internal INTERFACE NUE_HAS_PLATFORM_${NUE_OS_UPPER}=1)
    set(NUE_HAVE_PLATFORM_LAYER ON)
endif()
get_target_property(_nue_platform_sources nue_platform_obj SOURCES)
if(NOT _nue_platform_sources)
    # An OBJECT library needs at least one source on every generator.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nue_platform_empty.cpp" "// no platform layer sources for ${NUE_OS}\nnamespace nue_empty_platform {}\n")
    target_sources(nue_platform_obj PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nue_platform_empty.cpp")
endif()

# ---------------------------------------------------------------------------------------------------- texture module (owned by the nue-texture session)
# Contract: src/texture/CMakeLists.txt adds its sources with target_sources(nue_texture_obj PRIVATE ...) and may link libraries with
# target_link_libraries(nue_texture_obj PUBLIC ...). Its public header is include/nue/texture.h (included by nue.h when present).
add_library(nue_texture_obj OBJECT)
target_link_libraries(nue_texture_obj PUBLIC nue_internal)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/texture/CMakeLists.txt")
    add_subdirectory(src/texture)
    target_compile_definitions(nue_internal INTERFACE NUE_HAS_TEXTURE=1)
endif()
get_target_property(_nue_texture_sources nue_texture_obj SOURCES)
if(NOT _nue_texture_sources)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nue_texture_empty.cpp" "// texture module not present\nnamespace nue_empty_texture {}\n")
    target_sources(nue_texture_obj PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nue_texture_empty.cpp")
endif()

# ---------------------------------------------------------------------------------------------------- SDL3 backend (shared by all platforms)
add_library(nue_backend_sdl3_obj OBJECT)
target_link_libraries(nue_backend_sdl3_obj PUBLIC nue_internal)
if(NUE_HAVE_SDL3)
    file(GLOB NUE_SDL3_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/sdl3/*.cpp")
    target_sources(nue_backend_sdl3_obj PRIVATE ${NUE_SDL3_SOURCES})
    target_link_libraries(nue_backend_sdl3_obj PUBLIC SDL3::SDL3-static)
    if(NOT NUE_SDL3_SOURCES)
        # An OBJECT library needs at least one source on every generator, also while the backend sources are still being written.
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nue_sdl3_empty.cpp" "// SDL3 backend sources not present yet: the entry point exists and reports no backend\n#include \"internal/backend.h\"\nconst nue_backend* nue_backend_sdl3_get(void) { return nullptr; }\n")
        target_sources(nue_backend_sdl3_obj PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nue_sdl3_empty.cpp")
    endif()
else()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nue_sdl3_empty.cpp" "// SDL3 backend not built\nnamespace nue_empty_sdl3 {}\n")
    target_sources(nue_backend_sdl3_obj PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nue_sdl3_empty.cpp")
endif()

# ---------------------------------------------------------------------------------------------------- graphics module (bgfx)
if(NUE_WITH_GFX)
    include(NueGfx)   # cmake/NueGfx.cmake: defines nue_gfx_obj (see docs/GFX_D3D9_SEAM.md)
endif()

# ---------------------------------------------------------------------------------------------------- the library
if(NUE_BUILD_SHARED)
    add_library(nue SHARED)
else()
    add_library(nue STATIC)
endif()
add_library(nue::nue ALIAS nue)
target_sources(nue PRIVATE
    $<TARGET_OBJECTS:nue_core_obj>
    $<TARGET_OBJECTS:nue_platform_obj>
    $<TARGET_OBJECTS:nue_texture_obj>
    $<TARGET_OBJECTS:nue_backend_sdl3_obj>)
if(TARGET nue_gfx_obj)
    target_sources(nue PRIVATE $<TARGET_OBJECTS:nue_gfx_obj>)
endif()
target_include_directories(nue PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>")
target_compile_features(nue PUBLIC cxx_std_17)
if(NUE_BUILD_SHARED)
    target_compile_definitions(nue PRIVATE NUE_BUILD_SHARED)
    target_compile_definitions(nue INTERFACE NUE_USE_SHARED)
endif()
set_target_properties(nue PROPERTIES OUTPUT_NAME nue VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
                      POSITION_INDEPENDENT_CODE ON)
# Link order: objects of every module need the OS libraries and (static) SDL3.
target_link_libraries(nue PRIVATE "$<BUILD_INTERFACE:nue_internal>")
if(NUE_HAVE_SDL3)
    if(NUE_BUILD_SHARED)
        target_link_libraries(nue PRIVATE SDL3::SDL3-static)   # folded into the shared library
    else()
        target_link_libraries(nue PRIVATE "$<BUILD_INTERFACE:SDL3::SDL3-static>" "$<INSTALL_INTERFACE:SDL3::SDL3-static>")
    endif()
endif()
if(TARGET nue_gfx_obj)
    # bgfx, bx, bimg (static): folded into a shared libnue; a static SDK installs them next to it in the nue export set (nue::bgfx, nue::bx, nue::bimg)
    target_link_libraries(nue PRIVATE bgfx bx bimg)
endif()
# Libraries/frameworks the platform and texture fragments asked for: libraries linked PUBLIC to an OBJECT library do not travel with
# $<TARGET_OBJECTS>, so they are forwarded to the final library here (iOS frameworks, Android liblog, Linux libs...).
foreach(_obj nue_platform_obj nue_texture_obj)
    get_target_property(_deps ${_obj} INTERFACE_LINK_LIBRARIES)
    if(_deps)
        list(FILTER _deps EXCLUDE REGEX "nue_internal")
        if(_deps)
            target_link_libraries(nue PRIVATE ${_deps})
        endif()
    endif()
endforeach()
if(WIN32)
    target_link_libraries(nue PRIVATE winmm ole32 advapi32 shell32 user32 dbghelp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(nue PRIVATE pthread dl m)
elseif(ANDROID)
    target_link_libraries(nue PRIVATE log android dl m)
endif()

# ---------------------------------------------------------------------------------------------------- entry stub (nue::entry)
add_library(nue_entry STATIC)
add_library(nue::entry ALIAS nue_entry)
file(GLOB NUE_ENTRY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/sdl3/entry/*.cpp")
if(NUE_ENTRY_SOURCES AND NUE_HAVE_SDL3)
    target_sources(nue_entry PRIVATE ${NUE_ENTRY_SOURCES})
else()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nue_entry_empty.cpp" "// entry stub not built (needs SDL3)\nnamespace nue_empty_entry {}\n")
    target_sources(nue_entry PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/nue_entry_empty.cpp")
endif()
target_link_libraries(nue_entry PUBLIC nue::nue PRIVATE "$<BUILD_INTERFACE:nue_internal>")
if(NUE_ENTRY_SOURCES AND NUE_HAVE_SDL3)
    # the stub includes <SDL3/SDL_main.h>; SDL_main's platform glue (WinMain, SDL_RunApp...) comes from SDL itself
    target_link_libraries(nue_entry PRIVATE "$<BUILD_INTERFACE:SDL3::SDL3-static>")
    if(NUE_BUILD_SHARED)
        # SDL is folded into the DLL with hidden symbols; the stub still needs SDL_RunApp (the stateless main glue), so it carries its own copy
        target_link_libraries(nue_entry PRIVATE "$<INSTALL_INTERFACE:SDL3::SDL3-static>")
    endif()
endif()

# ---------------------------------------------------------------------------------------------------- asset module (owned by the assets session)
# Contract: src/assets/CMakeLists.txt defines the static library nue_assets (alias nue::assets) on top of nue::nue, plus tools/nue_pack and tests/assets.
# Public header include/nue/assets.h (not included by nue.h: it needs the separate library).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/assets/CMakeLists.txt")
    add_subdirectory(src/assets)
endif()

# ---------------------------------------------------------------------------------------------------- audio module (owned by the audio session)
# Contract: src/audio/CMakeLists.txt defines the static library nue_audio (alias nue::audio) on top of nue::nue, plus tools/nue_audio and tests/audio.
# Public headers include/nue/audio_*.h (not included by nue.h: they need the separate library).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/CMakeLists.txt")
    add_subdirectory(src/audio)
endif()

# ---------------------------------------------------------------------------------------------------- legacy ABI layer (libnue_legacy)
# Contract: src/legacy/CMakeLists.txt defines the target `nue_legacy` (linking nue::nue) and an alias nue::legacy. Native games never link it.
if(NUE_BUILD_LEGACY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/legacy/CMakeLists.txt")
    add_subdirectory(src/legacy)
endif()

# ---------------------------------------------------------------------------------------------------- tests, samples
if(NUE_BUILD_TESTS)
    add_subdirectory(tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx/CMakeLists.txt")
        add_subdirectory(tests/gfx)   # owner: gfx session
    endif()
endif()
if(NUE_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()

# ---------------------------------------------------------------------------------------------------- legacy ABI: Direct3D 9 layer (owner: legacy-d3d9)
if(NUE_WITH_LEGACY_D3D9)
    add_subdirectory(src/legacy/d3d9)
endif()

# ---------------------------------------------------------------------------------------------------- install / package config (find_package(nue))
# `cmake --install <build> --prefix <dir>` writes the SDK:
#   include/nue, include/nuepp (+ include/nue_legacy when built)   headers
#   lib/ nue, nue_entry, nue_assets, SDL3-static (static SDK) or bin/+lib/ nue (shared SDK)
#   lib/cmake/nue/nueConfig.cmake                                    find_package(nue CONFIG): nue::nue, nue::entry, nue::assets, nue::legacy
#   bin/ nue_pack, nue_texconv                                       command line tools (desktop builds)
#   share/doc/nue, licenses                                          documentation, third party licences
# samples/consume_sdk is an external project that uses it; ctest runs it against a fresh install (tests/sdk_install.cmake).
if(NUE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # the exported names match the build tree aliases (nue::entry, nue::assets, nue::legacy)
    if(TARGET nue_entry)
        set_target_properties(nue_entry PROPERTIES EXPORT_NAME entry)
    endif()
    if(TARGET nue_assets)
        set_target_properties(nue_assets PROPERTIES EXPORT_NAME assets)
    endif()
    if(TARGET nue_legacy)
        set_target_properties(nue_legacy PROPERTIES EXPORT_NAME legacy)
    endif()

    install(TARGETS nue nue_entry EXPORT nueTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/nue include/nuepp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    if(TARGET nue_legacy)
        install(TARGETS nue_legacy EXPORT nueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(DIRECTORY include/nue_legacy DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    endif()
    # Extension headers published by platform layers (optional): src/platform/<os>/include/nue_ext_*.h
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/${NUE_OS}/include")
        install(DIRECTORY src/platform/${NUE_OS}/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nue/ext
                FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
    endif()
    # Command line tools (they exist on desktop builds only)
    foreach(_tool nue_pack nue_texconv)
        if(TARGET ${_tool})
            install(TARGETS ${_tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    endforeach()
    if(TARGET nue_gfx_obj AND NOT NUE_BUILD_SHARED)
        install(TARGETS bgfx bx bimg EXPORT nueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(DIRECTORY "${NUE_BGFX_CMAKE_DIR}/bgfx/include/bgfx" "${NUE_BGFX_CMAKE_DIR}/bx/include/bx" "${NUE_BGFX_CMAKE_DIR}/bimg/include/bimg"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(FILES "${NUE_BGFX_CMAKE_DIR}/bgfx/LICENSE" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/bgfx)
        install(FILES "${NUE_BGFX_CMAKE_DIR}/bx/LICENSE" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/bx)
        install(FILES "${NUE_BGFX_CMAKE_DIR}/bimg/LICENSE" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/bimg)
    endif()
    install(EXPORT nueTargets NAMESPACE nue:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nue)

    # facts a consumer must match (see cmake/nueConfig.cmake.in)
    set(NUE_CONFIG_SHARED ${NUE_BUILD_SHARED})
    set(NUE_CONFIG_STATIC_CRT ${NUE_STATIC_CRT})
    if(TARGET nue_gfx_obj)
        set(NUE_CONFIG_HAS_GFX ON)
    else()
        set(NUE_CONFIG_HAS_GFX OFF)
    endif()
    configure_package_config_file(cmake/nueConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/nueConfig.cmake"
                                  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nue)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/nueConfigVersion.cmake"
                                     VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nueConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/nueConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nue)
    install(FILES docs/API.md docs/API_CHANGES.md docs/ABI.md docs/ARCHITECTURE.md docs/PORTING.md docs/STATUS.md docs/NUEPACK.md docs/TEXTURE.md
                  docs/ASSET_FORMATS.md docs/GFX_D3D9_SEAM.md docs/GFX.md docs/THIRD_PARTY_GFX.md docs/LEGACY_LAYERS.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(DIRECTORY samples/consume_sdk DESTINATION ${CMAKE_INSTALL_DOCDIR}/samples)   # the minimal external project, ready to copy
endif()
