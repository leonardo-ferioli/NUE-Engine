# NUE Windows platform layer. Owned by the windows session. Included by the root CMakeLists.txt when the target OS is Windows.
#
# Targets defined here
#   nue_win_modules_obj   OBJECT  the nue::win modules (power, cpu, gpu, display, window, input, audio, storage, diag...). No dependency on the core.
#   nue_win_modules       STATIC  the same objects as a library, used by the module tests and the probe sample (no SDL3, no core needed)
#   nue_win_glue_obj      OBJECT  platform_windows.cpp: nue_platform_windows_get_ops() over the modules
# Both object sets are added to nue_platform_obj, hence to libnue.
#
# For executables that want the discrete GPU on hybrid-graphics laptops: nue_win_request_discrete_gpu(<target>).

set(NUE_WIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Directory of the NUE Windows platform layer")

set(NUE_WIN_MODULE_SOURCES
    common.cpp power.cpp cpu.cpp gpu.cpp gamemode.cpp display.cpp window.cpp input.cpp audio.cpp audio_wasapi.cpp storage.cpp diag.cpp)

add_library(nue_win_modules_obj OBJECT ${NUE_WIN_MODULE_SOURCES})
target_include_directories(nue_win_modules_obj PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nue_win_modules_obj PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
    target_compile_options(nue_win_modules_obj PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(nue_win_modules_obj PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set_target_properties(nue_win_modules_obj PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

add_library(nue_win_modules STATIC $<TARGET_OBJECTS:nue_win_modules_obj>)
target_include_directories(nue_win_modules PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(nue_win_modules PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(nue_win_modules PUBLIC kernel32 user32 advapi32 shell32 powrprof avrt winmm dxgi shcore comctl32 wtsapi32 ole32 pdh)

add_library(nue_win_glue_obj OBJECT platform_windows.cpp)
target_link_libraries(nue_win_glue_obj PUBLIC nue_internal)
target_include_directories(nue_win_glue_obj PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../core")
target_compile_definitions(nue_win_glue_obj PUBLIC UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
set_target_properties(nue_win_glue_obj PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Into libnue. (The Win32 import libraries are recorded in the objects with #pragma comment(lib), so linking nue needs no extra list.)
target_sources(nue_platform_obj PRIVATE $<TARGET_OBJECTS:nue_win_modules_obj> $<TARGET_OBJECTS:nue_win_glue_obj>)

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
