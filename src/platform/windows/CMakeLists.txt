# NUE Windows platform layer. Owned by the windows session. Included by the root CMakeLists.txt when the target OS is Windows.
#
# Targets defined here
#   nue_platform_obj      (defined by the root file) gets the modules and platform_windows.cpp: nue_platform_windows_get_ops() over them
#   nue_win_modules       STATIC  the nue::win modules alone (power, cpu, gpu, display, window, input, audio, storage, diag...), no core dependency: module tests, probe sample
#   nue_win_glue_obj      OBJECT  platform_windows.cpp compiled separately, linked by the glue test only
#
# For executables that want the discrete GPU on hybrid-graphics laptops: nue_win_request_discrete_gpu(<target>).

set(NUE_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Directory of the NUE Windows platform layer")

set(NUE_WIN_MODULE_SOURCES
    common.cpp power.cpp cpu.cpp gpu.cpp gamemode.cpp display.cpp window.cpp input.cpp audio.cpp audio_wasapi.cpp storage.cpp diag.cpp)
set(NUE_WIN_DEFINES UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
set(NUE_WIN_MODULES_ABS "")
foreach(f IN LISTS NUE_WIN_MODULE_SOURCES)
    list(APPEND NUE_WIN_MODULES_ABS "${CMAKE_CURRENT_SOURCE_DIR}/${f}")
endforeach()

# Into libnue: the sources are compiled by nue_platform_obj itself (objects of another object library are not forwarded by $<TARGET_OBJECTS:...>).
# The Win32 import libraries are recorded in the objects with #pragma comment(lib), so linking nue needs no extra list.
target_sources(nue_platform_obj PRIVATE ${NUE_WIN_MODULES_ABS} "${CMAKE_CURRENT_SOURCE_DIR}/platform_windows.cpp")
target_include_directories(nue_platform_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nue_platform_obj PRIVATE ${NUE_WIN_DEFINES})

# The same modules as an independent static library (no SDL3, no core): used by the module tests and the probe sample.
add_library(nue_win_modules STATIC ${NUE_WIN_MODULES_ABS})
target_include_directories(nue_win_modules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nue_win_modules PUBLIC ${NUE_WIN_DEFINES})
if(MSVC)
    target_compile_options(nue_win_modules PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(nue_win_modules PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set_target_properties(nue_win_modules PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(nue_win_modules PUBLIC kernel32 user32 advapi32 shell32 powrprof avrt winmm dxgi shcore comctl32 wtsapi32 ole32 pdh)

# The ops table compiled on its own, for the glue test (which supplies stubs of the core services).
add_library(nue_win_glue_obj OBJECT platform_windows.cpp)
target_link_libraries(nue_win_glue_obj PUBLIC nue_internal)
target_include_directories(nue_win_glue_obj PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
target_compile_definitions(nue_win_glue_obj PUBLIC ${NUE_WIN_DEFINES})
set_target_properties(nue_win_glue_obj PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

function(nue_win_request_discrete_gpu target)
    target_sources(${target} PRIVATE "${NUE_WIN_DIR}/gpu_exports.cpp")
endfunction()

if(NUE_BUILD_TESTS)
    enable_testing()   # the root file enables it only after the platform fragment, and tests registered below would be lost
    add_subdirectory(tests)
endif()
if(NUE_BUILD_SAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/windows_probe/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../samples/windows_probe" samples_windows_probe)
endif()
