# Asset module tests (console only, null backend). tests/assets/test_<name>.cpp -> ctest name nue_assets_test_<name>.
# Tests that need the real game files are opt-in: set NUE_TEST_GAME_DIR to the game's data folder; without it they skip and pass.
file(GLOB NUE_ASSETS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/test_*.cpp")
foreach(src IN LISTS NUE_ASSETS_TEST_SOURCES)
    get_filename_component(name "${src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" short "${name}")
    set(target nue_assets_test_${short})
    add_executable(${target} "${src}")
    target_link_libraries(${target} PRIVATE nue::assets nue_internal)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${CMAKE_CURRENT_LIST_DIR}/..")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 600 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endforeach()
