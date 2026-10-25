# Graphics module tests. Picked up automatically: tests/gfx/test_<name>.cpp -> ctest name nue_gfx_test_<name>.
#   test_*.cpp            console only (null backend, NOOP renderer, PREPARE mode); they run with and without -DNUE_WITH_GFX=ON
#   local/test_*.cpp      need a GPU and open a HIDDEN window with the real renderer; built only with -DNUE_GFX_LOCAL_TESTS=ON (Windows, Direct3D 11); never run in CI
option(NUE_GFX_LOCAL_TESTS "Build the gfx tests that create a real (hidden-window) Direct3D 11 renderer; they need a GPU" OFF)

file(GLOB NUE_GFX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/test_*.cpp")
foreach(src IN LISTS NUE_GFX_TEST_SOURCES)
    get_filename_component(name "${src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" short "${name}")
    set(target nue_gfx_test_${short})
    add_executable(${target} "${src}")
    target_link_libraries(${target} PRIVATE nue::nue nue_internal)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endforeach()

if(NUE_GFX_LOCAL_TESTS AND NUE_WITH_GFX AND NUE_HAVE_SDL3)
    file(GLOB NUE_GFX_LOCAL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/local/test_*.cpp")
    foreach(src IN LISTS NUE_GFX_LOCAL_SOURCES)
        get_filename_component(name "${src}" NAME_WE)
        string(REGEX REPLACE "^test_" "" short "${name}")
        set(target nue_gfx_local_${short})
        add_executable(${target} "${src}")
        set_target_properties(${target} PROPERTIES CXX_STANDARD 20)     # bx headers need C++20
        target_link_libraries(${target} PRIVATE nue::nue nue_internal bgfx bx bimg)
        target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/tests" "${NUE_BGFX_CMAKE_DIR}/bgfx/examples")
        # registered under the label "gpu" so a developer can run `ctest -L gpu`; CI never configures NUE_GFX_LOCAL_TESTS
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES TIMEOUT 120 LABELS gpu)
    endforeach()
endif()
