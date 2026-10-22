# Console-only tests: they use the null backend (or SDL's dummy/offscreen drivers) and never open a real window or audio device.
# Add a test: drop tests/test_<name>.cpp here; it is picked up automatically and registered with ctest as nue_test_<name>.
file(GLOB NUE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(src IN LISTS NUE_TEST_SOURCES)
    get_filename_component(name "${src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" short "${name}")
    set(target nue_test_${short})
    add_executable(${target} "${src}")
    target_link_libraries(${target} PRIVATE nue::nue nue_internal)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(short STREQUAL "wrapper" AND NOT MSVC)
        # The C++ wrapper promises to build and work without exceptions: this test is compiled that way on purpose (GCC/clang, also clang on Windows).
        target_compile_options(${target} PRIVATE -fno-exceptions)
        if(WIN32)
            target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)   # the MSVC STL needs this to stop using throw
        endif()
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endforeach()
