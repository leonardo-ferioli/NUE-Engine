# A game (or tool) that uses an INSTALLED NUE SDK. This project knows nothing about the engine's source tree: it only needs
#     cmake -S samples/consume_sdk -B build_consumer -DCMAKE_PREFIX_PATH=<sdk prefix>        (the folder `cmake --install` wrote)
# The engine's own CI builds and runs it against a fresh install of every preset (ctest: nue_sdk_install).
cmake_minimum_required(VERSION 3.21)
project(consume_sdk LANGUAGES C CXX)

find_package(nue 0.1 CONFIG REQUIRED)            # also finds SDL3 for a static SDK (installed next to it)
message(STATUS "consume_sdk: NUE ${nue_VERSION} from ${nue_DIR}")

# 1) own main(): initialise headless on the null backend, use storage, mixer and the C++ wrapper
add_executable(consume_core consume_core.cpp)
target_link_libraries(consume_core PRIVATE nue::nue)

# 2) engine-owned entry point: define nue_main (nue/entry.h) and link nue::entry; it runs on the SDL3 backend when the SDK has one
if(TARGET nue::entry)
    add_executable(consume_entry consume_entry.cpp)
    target_link_libraries(consume_entry PRIVATE nue::entry)
endif()

# 3) the asset module is a separate library of the SDK
if(TARGET nue::assets)
    add_executable(consume_assets consume_assets.cpp)
    target_link_libraries(consume_assets PRIVATE nue::assets)
endif()

# A shared SDK on Windows: the DLL has to sit next to the executables to run them
foreach(_t consume_core consume_entry consume_assets)
    if(TARGET ${_t} AND WIN32 AND nue_SHARED)
        add_custom_command(TARGET ${_t} POST_BUILD
                           COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${_t}> $<TARGET_FILE_DIR:${_t}>
                           COMMAND_EXPAND_LISTS)
    endif()
endforeach()

enable_testing()
add_test(NAME consume_core COMMAND consume_core)
if(TARGET consume_entry)
    add_test(NAME consume_entry COMMAND consume_entry)
    set_tests_properties(consume_entry PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
endif()
if(TARGET consume_assets)
    add_test(NAME consume_assets COMMAND consume_assets)
endif()
