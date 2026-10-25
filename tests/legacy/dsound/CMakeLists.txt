# legacy_dsound tests (console only, no window, no sound): registered by src/legacy/dsound/CMakeLists.txt. ctest names: nue_dsound_<name>.
function(nue_dsound_test name source)
    add_executable(nue_dsound_${name} "${source}")
    target_link_libraries(nue_dsound_${name} PRIVATE nue::legacy_dsound nue_internal)
    target_include_directories(nue_dsound_${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    add_test(NAME nue_dsound_${name} COMMAND nue_dsound_${name})
    set_tests_properties(nue_dsound_${name} PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endfunction()

nue_dsound_test(device ds_device_tests.cpp)
