# Console sample: prints what the machine supports. Standalone build:
#   cmake -S samples/windows_probe -B build/probe -G Ninja && cmake --build build/probe && build/probe/windows_probe.exe
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(windows_probe LANGUAGES CXX)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform/windows" nue_platform_windows)
endif()
if(NOT WIN32)
    return()
endif()
add_executable(windows_probe main.cpp)
target_link_libraries(windows_probe PRIVATE nue_platform_windows)
set_target_properties(windows_probe PROPERTIES CXX_STANDARD 17)
nue_win_request_discrete_gpu(windows_probe)
