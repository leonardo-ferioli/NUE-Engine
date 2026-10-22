cmake_minimum_required(VERSION 3.22.1)
project(nue_sample LANGUAGES C CXX)

# SDL3 and the Android backend come from the nue-engine AAR through Prefab.
find_package(SDL3 REQUIRED CONFIG)
find_package(nue_android REQUIRED CONFIG)

# SDLActivity loads the last library of NueActivity.getLibraries() and calls SDL_main in it: it must be named "main".
add_library(main SHARED sample_main.cpp)
target_compile_features(main PRIVATE cxx_std_17)
target_compile_options(main PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(main PRIVATE SDL3::SDL3 nue_android::nue_android log android)
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
