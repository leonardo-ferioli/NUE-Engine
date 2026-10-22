cmake_minimum_required(VERSION 3.22.1)
project(nue_sample LANGUAGES C CXX)

# libnue.so (core + Android platform layer, SDL3 linked statically inside) comes from the nue-engine AAR through Prefab.
find_package(nue REQUIRED CONFIG)

# This sample talks to SDL3 directly (the SDL3 backend of the engine is not part of this repository yet): the headers come
# from the pinned submodule (third_party/SDL/include), the symbols from libnue.so, whose static SDL3 exports its API.
set(NUE_SDL3_INCLUDE "" CACHE PATH "SDL3 include directory (third_party/SDL/include)")
set(NUE_ANDROID_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../../../src/platform/android/include")

# SDLActivity loads the last library of NueActivity.getLibraries() and calls SDL_main in it: it must be named "main".
add_library(main SHARED sample_main.cpp)
target_compile_features(main PRIVATE cxx_std_17)
target_compile_options(main PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_include_directories(main PRIVATE "${NUE_SDL3_INCLUDE}" "${NUE_ANDROID_INCLUDE}")
target_link_libraries(main PRIVATE nue::nue log android)
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
