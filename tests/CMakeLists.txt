# Console-only tests: they use the null backend (or SDL's dummy/offscreen drivers) and never open a real window or audio device.
# Add a test: drop tests/test_<name>.cpp here; it is picked up automatically and registered with ctest as nue_test_<name>.
file(GLOB NUE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")

# ABI guard (test_abi.cpp): the layout table of include/nue/*.h is generated at build time by tools/abi/gen_abi_probe.py (Python 3, standard library only)
find_package(Python3 COMPONENTS Interpreter QUIET)
file(GLOB_RECURSE NUE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/nue/*.h")
set(NUE_ABI_PROBE "${CMAKE_CURRENT_BINARY_DIR}/abi/abi_probe.inc")
if(Python3_FOUND)
    add_custom_command(OUTPUT "${NUE_ABI_PROBE}"
                       COMMAND ${Python3_EXECUTABLE} "${PROJECT_SOURCE_DIR}/tools/abi/gen_abi_probe.py" "${PROJECT_SOURCE_DIR}/include" "${NUE_ABI_PROBE}"
                       DEPENDS ${NUE_PUBLIC_HEADERS} "${PROJECT_SOURCE_DIR}/tools/abi/gen_abi_probe.py"
                       COMMENT "ABI guard: reading the public headers" VERBATIM)
endif()

foreach(src IN LISTS NUE_TEST_SOURCES)
    get_filename_component(name "${src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" short "${name}")
    set(target nue_test_${short})
    if(short STREQUAL "abi" AND NOT Python3_FOUND)
        message(WARNING "NUE: Python 3 not found - the ABI guard test (nue_test_abi) is not built")
        continue()
    endif()
    add_executable(${target} "${src}")
    if(short STREQUAL "abi")
        target_sources(${target} PRIVATE "${NUE_ABI_PROBE}")
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/abi")
        target_compile_definitions(${target} PRIVATE NUE_ABI_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/abi/reference" NUE_PROJECT_VERSION="${PROJECT_VERSION}")
    endif()
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

# The SDK is consumable: install the finished build into a temp prefix and build + run the external project samples/consume_sdk against it.
# Not registered when cross compiling (the consumer's executables could not run) or when no install rules exist.
if(NUE_INSTALL AND NOT CMAKE_CROSSCOMPILING AND NOT ANDROID AND NOT IOS AND EXISTS "${PROJECT_SOURCE_DIR}/samples/consume_sdk/CMakeLists.txt")
    add_test(NAME nue_sdk_install
             COMMAND ${CMAKE_COMMAND}
                     -DBUILD_DIR=${CMAKE_BINARY_DIR} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DWORK_DIR=${CMAKE_BINARY_DIR}/sdk_install_work
                     -DGENERATOR=${CMAKE_GENERATOR} -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM} -DBUILD_TYPE=$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,${CMAKE_BUILD_TYPE}>
                     -DCONFIG=$<$<BOOL:${CMAKE_CONFIGURATION_TYPES}>:$<CONFIG>>
                     -DC_COMPILER=${CMAKE_C_COMPILER} -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                     -DC_COMPILER_TARGET=${CMAKE_C_COMPILER_TARGET} -DCXX_COMPILER_TARGET=${CMAKE_CXX_COMPILER_TARGET}
                     -DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/sdk_install.cmake)
    set_tests_properties(nue_sdk_install PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
endif()
