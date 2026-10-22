cmake_minimum_required(VERSION 3.20)

# Android backend of the NUE engine.
#   * nue_android_logic  (static, plain C++17, no platform headers): lifecycle, memory, display, thermal, GPU caps,
#                        input tables, audio policy, storage import, crash report, event queue.  Builds and is tested on
#                        any host:  cmake -S nue_engine/src/platform/android -B build_android_host -G Ninja
#                                   cmake --build build_android_host && ctest --test-dir build_android_host
#   * nue_android        (shared libnue_android.so, only when cross compiling for Android): JNI glue, ADPF, GPU probe,
#                        crash handler, trace markers, public C API of include/nue_android.h.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(nue_android LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(NUE_ANDROID_STANDALONE ON)
else()
    set(NUE_ANDROID_STANDALONE OFF)
endif()

option(NUE_ANDROID_BUILD_TESTS "Build the host tests of the Android logic" ${NUE_ANDROID_STANDALONE})

set(NUE_ANDROID_LOGIC_SOURCES
    logic/audio_policy.cpp
    logic/crash_report.cpp
    logic/crc32.cpp
    logic/display_policy.cpp
    logic/event_queue.cpp
    logic/gpu_caps.cpp
    logic/import_engine.cpp
    logic/import_fs.cpp
    logic/input_classify.cpp
    logic/lifecycle_model.cpp
    logic/memory_budget.cpp
    logic/storage_paths.cpp
    logic/thermal_policy.cpp)

add_library(nue_android_logic STATIC ${NUE_ANDROID_LOGIC_SOURCES})
target_include_directories(nue_android_logic PUBLIC logic include)
target_compile_features(nue_android_logic PUBLIC cxx_std_17)
set_target_properties(nue_android_logic PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(nue_android_logic PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(NOT MSVC)
    target_compile_options(nue_android_logic PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(nue_android_logic PUBLIC Threads::Threads)
endif()

if(NUE_ANDROID_BUILD_TESTS)
    enable_testing()
    add_executable(nue_android_tests
        tests/test_main.cpp tests/test_storage.cpp tests/test_import.cpp tests/test_logic.cpp)
    target_link_libraries(nue_android_tests PRIVATE nue_android_logic)
    if(NOT MSVC)
        target_compile_options(nue_android_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    foreach(suite storage import lifecycle memory display thermal gpu input audio crash events)
        add_test(NAME android_${suite} COMMAND nue_android_tests ${suite})
    endforeach()
endif()

if(ANDROID)
    set(NUE_ANDROID_NATIVE_SOURCES
        jni/android_state.cpp
        jni/android_jni.cpp
        jni/android_api.cpp
        jni/android_adpf.cpp
        jni/android_gpu_probe.cpp
        jni/android_import.cpp
        jni/android_crash.cpp
        jni/android_trace.cpp)
    add_library(nue_android SHARED ${NUE_ANDROID_NATIVE_SOURCES})
    target_include_directories(nue_android PUBLIC include PRIVATE logic jni)
    target_compile_features(nue_android PUBLIC cxx_std_17)
    target_compile_options(nue_android PRIVATE -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fno-exceptions)
    target_link_libraries(nue_android PRIVATE nue_android_logic log android EGL GLESv3)
    # 16 KB page size devices (Android 15+): segments must be aligned to 16384.  NDK r28+ does this by default, the
    # flags make older NDKs produce the same layout.  Also keep a build id for symbolication of crash reports.
    target_link_options(nue_android PRIVATE -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,--build-id=sha1)
    # Vulkan headers come from the NDK sysroot; libvulkan is opened with dlopen so the library also loads on devices
    # without a Vulkan driver.
endif()
