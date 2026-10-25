# legacy_dsound: the DirectSound layer of the NUE Engine. Added from the root CMakeLists.txt by -DNUE_WITH_LEGACY_DSOUND=ON.
#   nue_legacy_dsound   Level A (include/nue_legacy/dsound_*.hpp): DirectSound semantics as plain handle-based C++, on nue::audio (resampler, channel
#                       mixer, spatial, reverb) and nue::nue (output stream). No COM, no guest pointers, no game knowledge.
#   dsound.dll          (Windows) the drop-in COM facade over it: IDirectSound, IDirectSoundBuffer, 3D listener/buffer, Notify. Copy it next to a
#                       Windows game's executable (32-bit game: build the windows-x86 preset).
#   tests               tests/legacy/dsound (headless, null backend); Windows-only tools compare with the system DirectSound.
# See docs/LEGACY_DSOUND.md.
get_filename_component(NUE_DSOUND_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

if(NOT TARGET nue_audio)
    message(FATAL_ERROR "legacy_dsound needs the audio module (src/audio): target nue_audio is missing")
endif()

file(GLOB NUE_DSOUND_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
add_library(nue_legacy_dsound STATIC ${NUE_DSOUND_SOURCES})
add_library(nue::legacy_dsound ALIAS nue_legacy_dsound)
target_include_directories(nue_legacy_dsound
    PUBLIC "$<BUILD_INTERFACE:${NUE_DSOUND_ROOT}/include>"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src")
target_compile_features(nue_legacy_dsound PUBLIC cxx_std_20)
target_link_libraries(nue_legacy_dsound PUBLIC nue::audio PRIVATE "$<BUILD_INTERFACE:nue_internal>")
set_target_properties(nue_legacy_dsound PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/dll")
endif()

if(NUE_BUILD_TESTS AND EXISTS "${NUE_DSOUND_ROOT}/tests/legacy/dsound/CMakeLists.txt")
    add_subdirectory("${NUE_DSOUND_ROOT}/tests/legacy/dsound" "${CMAKE_CURRENT_BINARY_DIR}/tests")
endif()
