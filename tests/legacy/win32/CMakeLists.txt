# Headless, deterministic tests of legacy_win32 and legacy_dinput: one ctest entry per module (nue_test_legacy_win32_<module>). The *_windows modules compare with the
# real operating system and exist on Windows only.
file(GLOB NUE_LEGACY_WIN32_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*.cpp")
add_executable(nue_test_legacy_win32 ${NUE_LEGACY_WIN32_TEST_SOURCES})
target_link_libraries(nue_test_legacy_win32 PRIVATE nue_legacy_win32 nue_legacy_dinput)
if(WIN32)
    target_link_libraries(nue_test_legacy_win32 PRIVATE nue_legacy_win32_host advapi32 user32 winmm dinput8 dxguid)
endif()

set(NUE_LEGACY_WIN32_TEST_MODULES handles time text registry sync files window window_platform dinput process memory arena_memory seh guest_threads guest_timers virtual_api event_input)
if(WIN32)
    list(APPEND NUE_LEGACY_WIN32_TEST_MODULES time_windows text_codepages_windows text_classes_windows text_collation_windows text_locale_windows text_wsprintf_windows
         registry_windows sync_windows files_windows window_windows dinput_windows process_windows memory_windows)
endif()
foreach(module IN LISTS NUE_LEGACY_WIN32_TEST_MODULES)
    add_test(NAME nue_test_legacy_win32_${module} COMMAND nue_test_legacy_win32 ${module})
endforeach()

# The platform interfaces over the native API: the same conformance suite as the std implementation, on the engine's null backend (no window, no sound, no GPU).
add_executable(nue_test_legacy_win32_nue nue/test_nue_backend.cpp test_main.cpp)
target_include_directories(nue_test_legacy_win32_nue PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(nue_test_legacy_win32_nue PRIVATE nue_legacy_win32_nue)
foreach(module platform_conformance_std platform_conformance_nue)
    add_test(NAME nue_test_legacy_win32_${module} COMMAND nue_test_legacy_win32_nue ${module})
endforeach()
