# NUE Windows backend (own code beside SDL3). Owned by the nue-windows session.
#
# Standalone:   cmake -S nue_engine/src/platform/windows -B build -G Ninja && cmake --build build && ctest --test-dir build
# From core:    add_subdirectory(src/platform/windows)  ->  target nue_platform_windows (static library, x86 and x64)
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nue_platform_windows LANGUAGES CXX)
    set(NUE_WIN_STANDALONE ON)
else()
    set(NUE_WIN_STANDALONE OFF)
endif()
if(NOT WIN32)
    return()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nue_platform_windows STATIC
    common.cpp
    power.cpp
    cpu.cpp
    gpu.cpp
    gamemode.cpp
    display.cpp
    window.cpp
    input.cpp
    audio.cpp
    audio_wasapi.cpp
    storage.cpp
    diag.cpp
)
target_include_directories(nue_platform_windows PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(nue_platform_windows PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
    target_compile_options(nue_platform_windows PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(nue_platform_windows PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
target_link_libraries(nue_platform_windows PUBLIC kernel32 user32 advapi32 shell32 powrprof avrt winmm dxgi shcore comctl32 wtsapi32 ole32 pdh)

# An executable that wants the discrete GPU on hybrid-graphics laptops adds the two exported symbols the NVIDIA / AMD drivers look for
# in the main executable (a static library's exports would be dropped by the linker): nue_win_request_discrete_gpu(<exe target>).
function(nue_win_request_discrete_gpu target)
    target_sources(${target} PRIVATE "${NUE_WIN_DIR}/gpu_exports.cpp")
endfunction()
set(NUE_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Directory of the NUE Windows backend (visible to the callers of nue_win_request_discrete_gpu)")

if(NUE_WIN_STANDALONE OR NUE_WINDOWS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
