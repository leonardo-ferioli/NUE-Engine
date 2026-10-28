# hello_gfx: nue_gfx MANAGED mode with a triangle and a textured quad (needs -DNUE_WITH_GFX=ON and the SDL3 backend for nue::entry).
# Owner: gfx session. Added by samples/CMakeLists.txt.
add_executable(nue_sample_hello_gfx main.cpp draw.hpp)
set_target_properties(nue_sample_hello_gfx PROPERTIES CXX_STANDARD 20)    # bx/platform.h of the bgfx headers requires C++20
target_link_libraries(nue_sample_hello_gfx PRIVATE nue::nue nue::entry nue_internal)
target_include_directories(nue_sample_hello_gfx PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "$<TARGET_PROPERTY:bgfx,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:bx,INTERFACE_INCLUDE_DIRECTORIES>"
    "${NUE_BGFX_CMAKE_DIR}/bgfx/examples")      # common/imgui/*.bin.h: bgfx's pre-compiled shaders for every renderer (THIRD_PARTY_GFX.md)
if(MSVC)
    target_compile_definitions(nue_sample_hello_gfx PRIVATE __STDC_LIMIT_MACROS __STDC_FORMAT_MACROS __STDC_CONSTANT_MACROS)
endif()
# The bgfx libraries are linked into nue; the sample uses the bgfx API directly, so it links them too.
target_link_libraries(nue_sample_hello_gfx PRIVATE bgfx bx bimg)

# CI smoke test: no window, no GPU, the null backend and bgfx's Noop renderer; it runs the sample's whole frame loop (resources, draw calls, present, shutdown).
if(NUE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME nue_gfx_smoke COMMAND nue_sample_hello_gfx --noop --frames 30)
    set_tests_properties(nue_gfx_smoke PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "hello_gfx: 30 frames, 0 device resets")
endif()
