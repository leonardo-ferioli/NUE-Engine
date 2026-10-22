# Native part of the NUE engine AAR: SDL3 (shared) + the Android backend (libnue_android.so).
cmake_minimum_required(VERSION 3.22.1)
project(nue_engine_android LANGUAGES C CXX)

set(NUE_SDL3_SOURCE_DIR "" CACHE PATH "SDL3 source tree (release-3.4.x), see scripts/fetch_sdl3.ps1")
if(NOT EXISTS "${NUE_SDL3_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL3 sources not found at '${NUE_SDL3_SOURCE_DIR}'.\n"
        "Run nue_engine/android/scripts/fetch_sdl3.ps1 (or .sh), or pass -Pnue.sdl3.dir=<path> to Gradle.")
endif()

# SDL3: shared library, no tests / install rules.  16 KB alignment is added to SDL's own target as well.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${NUE_SDL3_SOURCE_DIR}" SDL3)
target_link_options(SDL3-shared PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)

# Android backend: <repo>/nue_engine/src/platform/android  (this file is nue_engine/android/nue-engine/src/main/cpp)
get_filename_component(NUE_ANDROID_BACKEND_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../src/platform/android" ABSOLUTE)
add_subdirectory("${NUE_ANDROID_BACKEND_DIR}" nue_android)
