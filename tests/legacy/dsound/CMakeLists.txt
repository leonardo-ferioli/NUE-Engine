# legacy_dsound tests (console only, no window, no sound): registered by src/legacy/dsound/CMakeLists.txt. ctest names: nue_dsound_<name>.
function(nue_dsound_test name source)
    add_executable(nue_dsound_${name} "${source}")
    target_link_libraries(nue_dsound_${name} PRIVATE nue::legacy_dsound nue_internal)
    target_include_directories(nue_dsound_${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME nue_dsound_${name} COMMAND nue_dsound_${name})
    set_tests_properties(nue_dsound_${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endfunction()

nue_dsound_test(device ds_device_tests.cpp)

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
