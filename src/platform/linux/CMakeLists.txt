# NUE Linux platform backend (owned by the nue-linux session).
#
# Two ways to use this directory:
#   * Integrated: nue_engine/CMakeLists.txt add_subdirectory()s it when targeting Linux. The fragment then adds all
#     sources to the `nue_platform_obj` object library (contract in docs/PORTING.md) and defines nue_platform_linux_get_ops().
#   * Standalone (any host, e.g. the Windows development machine):  cmake -S nue_engine/src/platform/linux -B build
#     builds the portable logic, the unit tests (ctest) and the console sample, using stand-ins for the core.
#
# Targets
#   nue_linux_logic    portable logic: path case resolution, XDG, /proc+/sys parsers, session detection, GPU/renderer
#                      policy, audio/video/input policy, report. Builds on every host.
#   nue_linux_runtime  Linux-only system integration (GameMode, D-Bus, scheduling, timers, storage, crash handler,
#                      io_uring). Only when CMAKE_SYSTEM_NAME is Linux.
#   nue_linux_tests    ctest unit tests (no window, no sound, no GPU needed)
#   nue_linux_ops_tests the platform ops table against in-memory machines (standalone builds)
#   linux_probe        console sample that prints what the machine supports (and --root FIXTURE for captured machines)
#   make_fixture       writes a built-in machine fixture to disk for linux_probe --root
cmake_minimum_required(VERSION 3.20)

if(TARGET nue_platform_obj)
  set(NUE_LINUX_INTEGRATED ON)
else()
  set(NUE_LINUX_INTEGRATED OFF)
endif()
if(NOT NUE_LINUX_INTEGRATED AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(nue_platform_linux LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  include(CTest)
endif()

get_filename_component(NUE_LINUX_NUE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(NUE_LINUX_LOGIC_SOURCES
  lx_text.cpp lx_fs.cpp lx_pathcase.cpp lx_xdg.cpp lx_sysinfo.cpp lx_session.cpp
  lx_gpu.cpp lx_gpu_probe.cpp lx_policy.cpp lx_dl.cpp lx_diag.cpp lx_report.cpp lx_steamos.cpp)
set(NUE_LINUX_RUNTIME_SOURCES
  lx_dbus.cpp lx_gamemode.cpp lx_sched.cpp lx_storage.cpp lx_crash.cpp lx_runtime.cpp lx_steamos_runtime.cpp)

add_library(nue_linux_logic STATIC ${NUE_LINUX_LOGIC_SOURCES})
target_include_directories(nue_linux_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nue_linux_logic PUBLIC cxx_std_17)
set_target_properties(nue_linux_logic PROPERTIES POSITION_INDEPENDENT_CODE ON)
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

set(NUE_LINUX_TEST_SOURCES tests/test_main.cpp tests/test_pathcase.cpp tests/test_system.cpp tests/test_diag.cpp tests/test_steamos.cpp)
set(NUE_LINUX_EXTRA_LIBS)
set(NUE_LINUX_EXTRA_DEFS)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  add_library(nue_linux_runtime STATIC ${NUE_LINUX_RUNTIME_SOURCES})
  target_link_libraries(nue_linux_runtime PUBLIC nue_linux_logic Threads::Threads)
  target_compile_options(nue_linux_runtime PRIVATE -Wall -Wextra -Wno-unused-parameter)
  set_target_properties(nue_linux_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
  list(APPEND NUE_LINUX_TEST_SOURCES tests/test_runtime_linux.cpp)
  list(APPEND NUE_LINUX_EXTRA_LIBS nue_linux_runtime)
  list(APPEND NUE_LINUX_EXTRA_DEFS NUE_LINUX_RUNTIME)
endif()

# ---- the platform ops table + extension (needs the core's internal headers)
set(NUE_LINUX_OPS_INCLUDES
  ${NUE_LINUX_NUE_ROOT}/include ${NUE_LINUX_NUE_ROOT}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})

if(NUE_LINUX_INTEGRATED)
  target_sources(nue_platform_obj PRIVATE platform_linux.cpp ${NUE_LINUX_LOGIC_SOURCES} ${NUE_LINUX_RUNTIME_SOURCES})
  target_include_directories(nue_platform_obj PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_definitions(nue_platform_obj PRIVATE NUE_LINUX_RUNTIME)
  target_link_libraries(nue_platform_obj PUBLIC ${CMAKE_DL_LIBS})
  find_package(Threads REQUIRED)
  target_link_libraries(nue_platform_obj PUBLIC Threads::Threads)
else()
  add_library(nue_linux_ops STATIC platform_linux.cpp)
  target_include_directories(nue_linux_ops PUBLIC ${NUE_LINUX_OPS_INCLUDES})
  target_link_libraries(nue_linux_ops PUBLIC nue_linux_logic ${NUE_LINUX_EXTRA_LIBS})
  target_compile_definitions(nue_linux_ops PUBLIC ${NUE_LINUX_EXTRA_DEFS})
  if(MSVC)
    target_compile_options(nue_linux_ops PRIVATE /W4 /permissive-)
  else()
    target_compile_options(nue_linux_ops PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  if(WIN32)
    target_compile_definitions(nue_linux_ops PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  endif()

  add_executable(nue_linux_ops_tests tests/test_main.cpp tests/test_platform_ops.cpp tests/ops_stubs.cpp)
  target_link_libraries(nue_linux_ops_tests PRIVATE nue_linux_ops)
  target_include_directories(nue_linux_ops_tests PRIVATE tests)
  add_test(NAME nue_linux_ops_tests COMMAND nue_linux_ops_tests)
endif()

add_executable(nue_linux_tests ${NUE_LINUX_TEST_SOURCES})
target_link_libraries(nue_linux_tests PRIVATE nue_linux_logic ${NUE_LINUX_EXTRA_LIBS})
target_include_directories(nue_linux_tests PRIVATE tests)
add_test(NAME nue_linux_tests COMMAND nue_linux_tests)

add_executable(linux_probe tools/linux_probe.cpp)
target_link_libraries(linux_probe PRIVATE nue_linux_logic ${NUE_LINUX_EXTRA_LIBS})
target_compile_definitions(linux_probe PRIVATE ${NUE_LINUX_EXTRA_DEFS})

add_executable(make_fixture tools/make_fixture.cpp)
target_link_libraries(make_fixture PRIVATE nue_linux_logic)
target_include_directories(make_fixture PRIVATE tests)
