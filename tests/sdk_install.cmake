# ctest nue_sdk_install: proves the SDK is consumable. Run as
#   cmake -DBUILD_DIR=<engine build dir> -DSOURCE_DIR=<engine source dir> -DWORK_DIR=<scratch> -DGENERATOR=... -DBUILD_TYPE=... [toolchain vars] -P sdk_install.cmake
# 1. `cmake --install` of the finished build into a fresh prefix (what the release workflow does per preset)
# 2. the prefix contains every public header, the libraries, the package config and the documentation
# 3. samples/consume_sdk (an EXTERNAL project that only knows find_package(nue)) configures, builds and its tests pass
# Console only; the consumer's tests use the null backend / SDL dummy drivers.
cmake_minimum_required(VERSION 3.21)

foreach(var BUILD_DIR SOURCE_DIR WORK_DIR GENERATOR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "sdk_install: ${var} is required")
    endif()
endforeach()

set(PREFIX "${WORK_DIR}/prefix")
set(CONSUMER_BUILD "${WORK_DIR}/consumer")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(run_step title)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        # everything the step printed (CMake errors go to stderr), and the verdict again as the LAST line so a CI annotation of the tail shows it
        message(FATAL_ERROR "sdk_install: step '${title}' failed (exit ${rc})\n--- stdout ---\n${out}\n--- stderr ---\n${err}\n--- end ---\nsdk_install: FAILED in step '${title}'")
    endif()
    set(LAST_OUTPUT "${out}" PARENT_SCOPE)
endfunction()

# --- 1. install --------------------------------------------------------------------------------------------------------------------------
set(install_cmd "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${PREFIX}")
if(DEFINED CONFIG AND NOT "${CONFIG}" STREQUAL "")
    list(APPEND install_cmd --config "${CONFIG}")
endif()
run_step("cmake --install" ${install_cmd})

# --- 2. completeness ---------------------------------------------------------------------------------------------------------------------
set(missing "")
foreach(dir include/nue include/nuepp include/nue_legacy)
    file(GLOB_RECURSE headers RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/${dir}/*.h" "${SOURCE_DIR}/${dir}/*.hpp")
    foreach(h IN LISTS headers)
        if(NOT EXISTS "${PREFIX}/${h}")
            list(APPEND missing "${h}")
        endif()
    endforeach()
endforeach()
foreach(f lib/cmake/nue/nueConfig.cmake lib/cmake/nue/nueConfigVersion.cmake lib/cmake/nue/nueTargets.cmake
          share/doc/nue/API.md share/doc/nue/ABI.md share/doc/nue/API_CHANGES.md share/doc/nue/STATUS.md
          share/doc/nue/samples/consume_sdk/CMakeLists.txt)
    if(NOT EXISTS "${PREFIX}/${f}")
        list(APPEND missing "${f}")
    endif()
endforeach()
file(GLOB libs "${PREFIX}/lib/*nue*" "${PREFIX}/bin/*nue*")
list(LENGTH libs lib_count)
if(lib_count LESS 1)
    list(APPEND missing "lib/nue (no library installed)")
endif()
if(missing)
    string(REPLACE ";" "\n  " missing_text "${missing}")
    message(FATAL_ERROR "sdk_install: the install tree is incomplete, missing:\n  ${missing_text}")
endif()
message(STATUS "sdk_install: install tree complete (${PREFIX})")

# --- 3. external consumer ----------------------------------------------------------------------------------------------------------------
set(configure_cmd "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/samples/consume_sdk" -B "${CONSUMER_BUILD}" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${PREFIX}" "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
foreach(var C_COMPILER CXX_COMPILER C_COMPILER_TARGET CXX_COMPILER_TARGET TOOLCHAIN_FILE MAKE_PROGRAM GENERATOR_PLATFORM)
    if(DEFINED ${var} AND NOT "${${var}}" STREQUAL "")
        if(var STREQUAL "GENERATOR_PLATFORM")
            list(APPEND configure_cmd -A "${${var}}")
        else()
            list(APPEND configure_cmd "-DCMAKE_${var}=${${var}}")
        endif()
    endif()
endforeach()
run_step("configure consume_sdk" ${configure_cmd})
set(build_cmd "${CMAKE_COMMAND}" --build "${CONSUMER_BUILD}")
get_filename_component(_cmake_bin "${CMAKE_COMMAND}" DIRECTORY)
set(test_cmd "${_cmake_bin}/ctest" --test-dir "${CONSUMER_BUILD}" --output-on-failure)
if(DEFINED CONFIG AND NOT "${CONFIG}" STREQUAL "")
    list(APPEND build_cmd --config "${CONFIG}")
    list(APPEND test_cmd -C "${CONFIG}")
endif()
run_step("build consume_sdk" ${build_cmd})
set(ENV{SDL_VIDEODRIVER} dummy)
set(ENV{SDL_AUDIODRIVER} dummy)
run_step("run consume_sdk tests" ${test_cmd})
message(STATUS "sdk_install: consumer built and its tests passed\n${LAST_OUTPUT}")

file(REMOVE_RECURSE "${WORK_DIR}")
