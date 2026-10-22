# Android platform layer of the NUE engine.  Included by the root CMakeLists.txt when NUE_OS is "android"
# (contract: docs/PORTING.md): adds its sources to nue_platform_obj and must define nue_platform_android_get_ops().
#
#   logic/   plain C++17 without platform headers (lifecycle, memory, display, thermal, GPU caps, input tables, audio
#            policy, storage paths + import engine, crash report text, event queue).  Unit tested on any host, see tests/.
#   jni/     JNI glue (exported Java_org_nue_engine_NueBridge_* entry points), ADPF, GPU probe, crash handler, trace.
#   platform_android.cpp   the nue_platform_ops table.
#   include/ extension headers (nue_ext_android.h, nue_android.h), installed by the root file as nue/ext/*.

set(NUE_ANDROID_DIR "${CMAKE_CURRENT_LIST_DIR}")

target_sources(nue_platform_obj PRIVATE
    ${NUE_ANDROID_DIR}/platform_android.cpp
    ${NUE_ANDROID_DIR}/jni/android_state.cpp
    ${NUE_ANDROID_DIR}/jni/android_jni.cpp
    ${NUE_ANDROID_DIR}/jni/android_api.cpp
    ${NUE_ANDROID_DIR}/jni/android_adpf.cpp
    ${NUE_ANDROID_DIR}/jni/android_gpu_probe.cpp
    ${NUE_ANDROID_DIR}/jni/android_import.cpp
    ${NUE_ANDROID_DIR}/jni/android_crash.cpp
    ${NUE_ANDROID_DIR}/jni/android_trace.cpp
    ${NUE_ANDROID_DIR}/logic/audio_policy.cpp
    ${NUE_ANDROID_DIR}/logic/cpu_topology.cpp
    ${NUE_ANDROID_DIR}/logic/crash_report.cpp
    ${NUE_ANDROID_DIR}/logic/crc32.cpp
    ${NUE_ANDROID_DIR}/logic/display_policy.cpp
    ${NUE_ANDROID_DIR}/logic/event_queue.cpp
    ${NUE_ANDROID_DIR}/logic/gpu_caps.cpp
    ${NUE_ANDROID_DIR}/logic/import_engine.cpp
    ${NUE_ANDROID_DIR}/logic/import_fs.cpp
    ${NUE_ANDROID_DIR}/logic/input_classify.cpp
    ${NUE_ANDROID_DIR}/logic/lifecycle_model.cpp
    ${NUE_ANDROID_DIR}/logic/memory_budget.cpp
    ${NUE_ANDROID_DIR}/logic/storage_paths.cpp
    ${NUE_ANDROID_DIR}/logic/thermal_policy.cpp)

target_include_directories(nue_platform_obj PRIVATE
    ${NUE_ANDROID_DIR}/include ${NUE_ANDROID_DIR}/logic ${NUE_ANDROID_DIR}/jni)
target_compile_options(nue_platform_obj PRIVATE -fno-exceptions)
# log / android (ANativeWindow, AChoreographer, ATrace, APerformanceHint via dlsym), EGL + GLESv3 for the GPU probe;
# libvulkan is opened with dlopen, so devices without a Vulkan driver still load the library.
target_link_libraries(nue_platform_obj PUBLIC log android EGL GLESv3)
# 16 KB page size devices (Android 15+): the final library must be linked with 16 KB segment alignment.  NDK r28+ does it
# by default; the flags below also cover older NDKs.  (They are link options of the final target, see android/.)
