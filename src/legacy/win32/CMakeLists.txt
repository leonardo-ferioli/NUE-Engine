# legacy_win32: the Win32 subset of a translated 32-bit game as Level A libraries (plain handle-based C++, no Windows header, no guest pointer, no COM object).
# Added from the root CMakeLists.txt by -DNUE_WITH_LEGACY_WIN32=ON. See docs/LEGACY_WIN32.md.
#   nue_legacy_platform   the small interfaces the library is written against (Clock, Threads, Storage, Config, System, Input, Window, Display), shaped like the
#                         native headers, with std implementations (the default on every host) and null ones (deterministic: virtual clock, in-memory files,
#                         scripted input) that are the test fixtures. NUE-backed implementations live in nue_backend/.
#   nue_legacy_win32      time, text, registry, sync (threads, timers, waits), files, memory (address space, heaps), process, window, seh, guest threads
#   nue_legacy_win32_host the real Windows behind a Level A interface (registry, address space, polling input); Windows only
#   nue_legacy_dinput     DirectInput 8 device model (src/legacy/dinput)
# Needs no graphics module: it builds and tests with NUE_WITH_LEGACY_D3D9=OFF.
get_filename_component(NUE_WIN32_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

add_library(nue_legacy_platform STATIC
    platform/storage_path.cpp platform/std_clock.cpp platform/std_threads.cpp platform/std_storage.cpp platform/std_config_system.cpp
    platform/null_services.cpp platform/null_window.cpp)
add_library(nue::legacy_platform ALIAS nue_legacy_platform)
target_include_directories(nue_legacy_platform PUBLIC "$<BUILD_INTERFACE:${NUE_WIN32_ROOT}/include>")
target_compile_features(nue_legacy_platform PUBLIC cxx_std_20)
target_link_libraries(nue_legacy_platform PUBLIC nue::nue)
set_target_properties(nue_legacy_platform PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB NUE_WIN32_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
add_library(nue_legacy_win32 STATIC ${NUE_WIN32_SOURCES})
add_library(nue::legacy_win32 ALIAS nue_legacy_win32)
target_include_directories(nue_legacy_win32 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_link_libraries(nue_legacy_win32 PUBLIC nue_legacy_platform)
set_target_properties(nue_legacy_win32 PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    file(GLOB NUE_WIN32_HOST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/host/*.cpp")
    add_library(nue_legacy_win32_host STATIC ${NUE_WIN32_HOST_SOURCES})
    target_link_libraries(nue_legacy_win32_host PUBLIC nue_legacy_win32 advapi32 user32)
endif()

# The same interfaces implemented over the native API (nue_*): clock, threads, mutexes, conditions, semaphores, TLS, timers, system. Needs an initialised engine.
add_library(nue_legacy_win32_nue STATIC nue_backend/nue_platform.cpp)
add_library(nue::legacy_win32_nue ALIAS nue_legacy_win32_nue)
target_link_libraries(nue_legacy_win32_nue PUBLIC nue_legacy_platform nue::nue)
set_target_properties(nue_legacy_win32_nue PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../dinput" "${CMAKE_CURRENT_BINARY_DIR}/dinput")

if(NUE_INSTALL)
    include(GNUInstallDirs)
    set_target_properties(nue_legacy_platform PROPERTIES EXPORT_NAME legacy_platform)
    set_target_properties(nue_legacy_win32 PROPERTIES EXPORT_NAME legacy_win32)
    set_target_properties(nue_legacy_win32_nue PROPERTIES EXPORT_NAME legacy_win32_nue)
    install(TARGETS nue_legacy_platform nue_legacy_win32 nue_legacy_win32_nue EXPORT nueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(NUE_BUILD_TESTS AND EXISTS "${NUE_WIN32_ROOT}/tests/legacy/win32/CMakeLists.txt")
    add_subdirectory("${NUE_WIN32_ROOT}/tests/legacy/win32" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
