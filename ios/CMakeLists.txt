# NUE iOS backend: pure logic library (builds on any host and is what ctest runs), plus the Objective-C++ glue and the sample app when
# the target system is iOS. Usable standalone:
#   host tests:   cmake -S nue_engine/ios -B build_ios_host -G Ninja && cmake --build build_ios_host && ctest --test-dir build_ios_host
#   device build: cmake --preset ios-device   (see CMakePresets.json; needs macOS + Xcode)
cmake_minimum_required(VERSION 3.20)
project(nue_ios LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(NUE_IOS_SRC "${CMAKE_CURRENT_LIST_DIR}/../src/platform/ios" ABSOLUTE)

# ---- pure logic (no Apple headers) -------------------------------------------------------------------------------------------------------------
add_library(nue_ios_logic STATIC
    ${NUE_IOS_SRC}/logic/lifecycle.cpp
    ${NUE_IOS_SRC}/logic/power.cpp
    ${NUE_IOS_SRC}/logic/caps.cpp
    ${NUE_IOS_SRC}/logic/storage.cpp
    ${NUE_IOS_SRC}/logic/audio_policy.cpp
    ${NUE_IOS_SRC}/logic/input_logic.cpp
    ${NUE_IOS_SRC}/logic/nue_map.cpp)
# nue_map maps to the public NUE types, so the public headers are needed.
set(NUE_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include" CACHE PATH "Public NUE headers (default: this repository's include/)")
target_include_directories(nue_ios_logic PUBLIC ${NUE_IOS_SRC}/logic ${NUE_INCLUDE_DIR})
if(NOT MSVC)
    target_compile_options(nue_ios_logic PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- host tests --------------------------------------------------------------------------------------------------------------------------------
# Not built when cross-compiling for iOS (an iOS executable cannot run in ctest); the CI job builds them on the macOS host instead.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    include(CTest)
    add_executable(nue_ios_logic_tests tests/ios_logic_tests.cpp)
    target_link_libraries(nue_ios_logic_tests PRIVATE nue_ios_logic)
    add_test(NAME nue_ios_logic COMMAND nue_ios_logic_tests)
    add_executable(nue_ios_map_tests tests/nue_map_tests.cpp)
    target_link_libraries(nue_ios_map_tests PRIVATE nue_ios_logic)
    add_test(NAME nue_ios_map COMMAND nue_ios_map_tests)
    add_executable(nue_ios_plist_check tests/plist_check.cpp)
    add_test(NAME nue_ios_plist_check COMMAND nue_ios_plist_check ${CMAKE_CURRENT_LIST_DIR}/sample/Info.plist)
endif()

# ---- Apple glue + sample app -------------------------------------------------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/nue_ios_app.cmake)
endif()
