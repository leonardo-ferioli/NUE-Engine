# legacy_dsound tests (console only, no window, no sound): registered by src/legacy/dsound/CMakeLists.txt. ctest names: nue_dsound_<name>.
function(nue_dsound_test name source)
    add_executable(nue_dsound_${name} "${source}")
    target_link_libraries(nue_dsound_${name} PRIVATE nue::legacy_dsound nue_internal)
    target_include_directories(nue_dsound_${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME nue_dsound_${name} COMMAND nue_dsound_${name})
    set_tests_properties(nue_dsound_${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endfunction()

nue_dsound_test(device ds_device_tests.cpp)
nue_dsound_test(nue ds_nue_tests.cpp)   # on the native stream and the null backend

if(WIN32)
    # COM facade of dsound.dll through the real dsound.h interfaces, over a deterministic engine.
    nue_dsound_test(com ds_com_tests.cpp)
    target_link_libraries(nue_dsound_com PRIVATE nue_dsound_comlib)
endif()

if(WIN32)
    # The same calls on the system DirectSound and on the layer, compared (plays only zero-filled buffers, never touches the system volume, skips
    # itself without an output device). Real time (about 3 s), so it is a tool: built always, run by hand, and part of ctest only on request.
    option(NUE_DSOUND_SYSTEM_TESTS "Register the differential test against the system DirectSound with ctest" OFF)
    add_executable(nue_dsound_differential ds_differential_tests.cpp)
    target_link_libraries(nue_dsound_differential PRIVATE nue_dsound_comlib nue_internal)
    target_include_directories(nue_dsound_differential PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    if(NUE_DSOUND_SYSTEM_TESTS)
        add_test(NAME nue_dsound_differential COMMAND nue_dsound_differential)
        set_tests_properties(nue_dsound_differential PROPERTIES TIMEOUT 120)
    endif()
endif()

if(WIN32)
    # The built dsound.dll loaded by path (exports by ordinal and by name, DllMain, the engine on the null backend). Real time, about 0.2 s.
    add_executable(nue_dsound_dll_test ds_dll_tests.cpp)
    target_link_libraries(nue_dsound_dll_test PRIVATE nue_internal)
    target_include_directories(nue_dsound_dll_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    target_link_libraries(nue_dsound_dll_test PRIVATE nue_legacy_dsound dxguid uuid)
    add_dependencies(nue_dsound_dll_test nue_dsound_dll)
    add_test(NAME nue_dsound_dll_test COMMAND nue_dsound_dll_test $<TARGET_FILE:nue_dsound_dll>)
    set_tests_properties(nue_dsound_dll_test PROPERTIES TIMEOUT 60)
endif()

# Smoke test on the real default output endpoint (silence only, needs hardware and about 1 s of real time): a tool, not in ctest.
add_executable(nue_dsound_endpoint_smoke ds_endpoint_smoke.cpp)
target_link_libraries(nue_dsound_endpoint_smoke PRIVATE nue::legacy_dsound nue_internal)
target_include_directories(nue_dsound_endpoint_smoke PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

# The guest-pointer facade (dsound_guest.hpp) over a fake guest arena: the call sequence of a streaming client through call tokens.
nue_dsound_test(guest ds_guest_tests.cpp)

# What any OutputSink must satisfy for the cursor logic (manual sink and the native stream on the null backend); a new platform sink runs this too.
nue_dsound_test(sink ds_sink_tests.cpp)

# Level A trace: record, replay, divergence, comparison of two clients by the PCM they write.
nue_dsound_test(trace ds_trace_tests.cpp)
