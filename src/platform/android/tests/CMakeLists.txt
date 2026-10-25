cmake_minimum_required(VERSION 3.21)

# Host tests of the platform independent Android logic (no Android SDK / NDK needed).
#   cmake -S src/platform/android/tests -B build_android_host -G Ninja
#   cmake --build build_android_host && ctest --test-dir build_android_host --output-on-failure
project(nue_android_host_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(ANDROID_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(GLOB LOGIC_SOURCES CONFIGURE_DEPENDS "${ANDROID_DIR}/logic/*.cpp")

add_library(nue_android_logic STATIC ${LOGIC_SOURCES})
target_include_directories(nue_android_logic PUBLIC "${ANDROID_DIR}/logic" "${ANDROID_DIR}/include")
set_target_properties(nue_android_logic PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_compile_definitions(nue_android_logic PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(NOT MSVC)
    target_compile_options(nue_android_logic PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(nue_android_logic PUBLIC Threads::Threads)
endif()

enable_testing()
add_executable(nue_android_tests test_main.cpp test_storage.cpp test_import.cpp test_logic.cpp)
target_link_libraries(nue_android_tests PRIVATE nue_android_logic)
if(NOT MSVC)
    target_compile_options(nue_android_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(suite storage import lifecycle memory display thermal gpu input audio crash events cpu)
    add_test(NAME android_${suite} COMMAND nue_android_tests ${suite})
endforeach()
