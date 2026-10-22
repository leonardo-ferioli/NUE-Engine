# Console tests of the Windows backend. No window is shown, no sound is played, the game is never started.
function(nue_win_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE nue_win_modules)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

nue_win_test(power_tests)
nue_win_test(gpu_display_tests)
nue_win_request_discrete_gpu(gpu_display_tests)
nue_win_test(window_input_tests)
nue_win_test(audio_tests)
nue_win_test(storage_tests)
nue_win_test(diag_tests seh_helpers.cpp)

# The ops table: glue objects + stubs of the core services (see the top of the file); needs the core headers only.
add_executable(platform_glue_tests platform_glue_tests.cpp $<TARGET_OBJECTS:nue_win_glue_obj>)
target_link_libraries(platform_glue_tests PRIVATE nue_win_modules nue_internal)
set_target_properties(platform_glue_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
add_test(NAME platform_glue_tests COMMAND platform_glue_tests)
set_tests_properties(platform_glue_tests PROPERTIES TIMEOUT 180)
