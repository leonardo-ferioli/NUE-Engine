# NUE Linux platform backend (owned by the nue-linux session).
#
# Targets
#   nue_linux_logic    portable logic: path case resolution, XDG, /proc+/sys parsers, session
#                      detection, GPU/renderer policy, audio/video/input policy. Builds on every
#                      host, which is how it is unit-tested on a Windows development machine.
#   nue_linux_runtime  Linux-only system integration (GameMode, D-Bus, scheduling, timers, storage,
#                      crash handler, io_uring). Added only when CMAKE_SYSTEM_NAME is Linux.
#   nue_linux_tests    ctest unit tests (no window, no sound, no GPU needed)
#   linux_probe        console sample that prints what the machine supports
#
# Usable standalone (cmake -S nue_engine/src/platform/linux) or through add_subdirectory().
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(nue_platform_linux LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  include(CTest)
endif()

set(NUE_LINUX_LOGIC_SOURCES
  lx_text.cpp lx_fs.cpp lx_pathcase.cpp lx_xdg.cpp lx_sysinfo.cpp lx_session.cpp
  lx_gpu.cpp lx_gpu_probe.cpp lx_policy.cpp lx_dl.cpp)

add_library(nue_linux_logic STATIC ${NUE_LINUX_LOGIC_SOURCES})
target_include_directories(nue_linux_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nue_linux_logic PUBLIC cxx_std_17)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(nue_linux_logic PUBLIC ${CMAKE_DL_LIBS})
endif()
if(WIN32)
  target_compile_definitions(nue_linux_logic PRIVATE _CRT_SECURE_NO_WARNINGS)  # host-side unit tests only
endif()
if(MSVC)
  target_compile_options(nue_linux_logic PRIVATE /W4 /permissive-)
else()
  target_compile_options(nue_linux_logic PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

set(NUE_LINUX_TEST_SOURCES tests/test_main.cpp tests/test_pathcase.cpp tests/test_system.cpp)
set(NUE_LINUX_PROBE_SOURCES tools/linux_probe.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(NUE_LINUX_RUNTIME_SOURCES)
  add_library(nue_linux_runtime STATIC ${NUE_LINUX_RUNTIME_SOURCES})
  target_link_libraries(nue_linux_runtime PUBLIC nue_linux_logic)
  list(APPEND NUE_LINUX_TEST_SOURCES)
endif()

add_executable(nue_linux_tests ${NUE_LINUX_TEST_SOURCES})
target_link_libraries(nue_linux_tests PRIVATE nue_linux_logic)
target_include_directories(nue_linux_tests PRIVATE tests)
add_test(NAME nue_linux_tests COMMAND nue_linux_tests)

add_executable(linux_probe ${NUE_LINUX_PROBE_SOURCES})
target_link_libraries(linux_probe PRIVATE nue_linux_logic)
