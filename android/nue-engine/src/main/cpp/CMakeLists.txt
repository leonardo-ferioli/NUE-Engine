# Native part of the NUE engine AAR = libnue.so built by the repository's root CMakeLists.txt (core + sdl3 backend + the
# Android platform layer, SDL3 linked statically).  This file only sets the options of an Android SDK build.
cmake_minimum_required(VERSION 3.22.1)
project(nue_engine_android LANGUAGES C CXX)

# <repo>/android/nue-engine/src/main/cpp -> five levels up is the repository root
get_filename_component(NUE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)

if(NOT EXISTS "${NUE_ROOT}/third_party/SDL/CMakeLists.txt")
    message(FATAL_ERROR
        "SDL3 submodule missing.  Run:  git submodule update --init --depth 1 third_party/SDL")
endif()

set(NUE_BUILD_SHARED ON CACHE BOOL "" FORCE)    # libnue.so: the Java side loads it by name, JNI entry points live in it
set(NUE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NUE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(NUE_INSTALL OFF CACHE BOOL "" FORCE)
set(NUE_BACKEND_SDL3 ON CACHE BOOL "" FORCE)
add_subdirectory("${NUE_ROOT}" nue)

# 16 KB page size (Android 15+ devices): ELF segments aligned to 16384.
target_link_options(nue PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,--build-id=sha1)
