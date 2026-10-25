# NUE audio module: library nue_audio (static; alias nue::audio) on top of nue::nue. Owner: the audio session.
#   include/nue/audio_codec.h   game audio codecs (ADPCM families) + container decoders (WAV, FLAC, Ogg Vorbis, Ogg Opus) + streaming decode
#   include/nue/audio_dsp.h     resampler, channel layout conversion, dynamics, loudness (EBU R128), reverb (FDN, convolution), EFX/EAX model
#   include/nue/audio_spatial.h spatial rendering stage interface (panning, ambisonics, HRTF hooks)
#   src/audio/codecs, dsp, spatial   our own code
#   src/audio/third_party/<name>/    vendored code, original licence kept; listed in docs/THIRD_PARTY_AUDIO.md
# The module only needs the public engine API. Root contract: the root CMakeLists.txt calls add_subdirectory(src/audio) after the
# engine library is defined (request in docs/REQUESTS_audio.md).
cmake_minimum_required(VERSION 3.21)

option(NUE_AUDIO_TESTS "Build tests/audio" ${NUE_BUILD_TESTS})
if(NUE_OS STREQUAL "ios" OR NUE_OS STREQUAL "android")
    set(_nue_audio_tools_default OFF)
else()
    set(_nue_audio_tools_default ON)
endif()
option(NUE_AUDIO_TOOLS "Build tools/nue_audio" ${_nue_audio_tools_default})

get_filename_component(NUE_AUDIO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# ------------------------------------------------------------------ vendored third-party code (C, own warning level)
set(NUE_AUDIO_TP "${CMAKE_CURRENT_LIST_DIR}/third_party")
set(NUE_AUDIO_TP_SOURCES "")
set(NUE_AUDIO_TP_INCLUDES "")
set(NUE_AUDIO_TP_DEFINES "")
foreach(_frag IN ITEMS stb_vorbis dr_flac ebur128 opus)
    if(EXISTS "${NUE_AUDIO_TP}/${_frag}/nue_fragment.cmake")
        include("${NUE_AUDIO_TP}/${_frag}/nue_fragment.cmake")   # appends to the NUE_AUDIO_TP_* lists, defines NUE_AUDIO_HAVE_<NAME>
    endif()
endforeach()

file(GLOB NUE_AUDIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/codecs/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/dsp/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/spatial/*.cpp")
add_library(nue_audio STATIC ${NUE_AUDIO_SOURCES})
add_library(nue::audio ALIAS nue_audio)
target_include_directories(nue_audio PUBLIC
    "$<BUILD_INTERFACE:${NUE_AUDIO_ROOT}/include>"
    "$<INSTALL_INTERFACE:include>")
target_include_directories(nue_audio PRIVATE "${CMAKE_CURRENT_LIST_DIR}" ${NUE_AUDIO_TP_INCLUDES})
target_compile_features(nue_audio PUBLIC cxx_std_17)
target_link_libraries(nue_audio PUBLIC nue::nue PRIVATE "$<BUILD_INTERFACE:nue_internal>")
set_target_properties(nue_audio PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NUE_AUDIO_TP_SOURCES)
    # Third-party C code is compiled without our warning flags (they are not ours to fix) and with its own definitions.
    add_library(nue_audio_third_party OBJECT ${NUE_AUDIO_TP_SOURCES})
    target_include_directories(nue_audio_third_party PRIVATE ${NUE_AUDIO_TP_INCLUDES})
    target_compile_definitions(nue_audio_third_party PRIVATE ${NUE_AUDIO_TP_DEFINES})
    set_target_properties(nue_audio_third_party PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
    if(MSVC)
        target_compile_options(nue_audio_third_party PRIVATE /W0 /utf-8)
        target_compile_definitions(nue_audio_third_party PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(nue_audio_third_party PRIVATE -w)
        if(NOT WIN32)
            target_link_libraries(nue_audio_third_party PRIVATE m)
        endif()
    endif()
    target_sources(nue_audio PRIVATE $<TARGET_OBJECTS:nue_audio_third_party>)
endif()
target_compile_definitions(nue_audio PRIVATE ${NUE_AUDIO_TP_FEATURES})

if(NUE_INSTALL)
    install(TARGETS nue_audio EXPORT nueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(NUE_AUDIO_TESTS AND NOT NUE_OS STREQUAL "ios" AND EXISTS "${NUE_AUDIO_ROOT}/tests/audio/CMakeLists.txt")
    add_subdirectory("${NUE_AUDIO_ROOT}/tests/audio" "${CMAKE_CURRENT_BINARY_DIR}/tests_audio")
endif()
if(NUE_AUDIO_TOOLS AND EXISTS "${NUE_AUDIO_ROOT}/tools/nue_audio/CMakeLists.txt")
    add_subdirectory("${NUE_AUDIO_ROOT}/tools/nue_audio" "${CMAKE_CURRENT_BINARY_DIR}/nue_audio_tool")
endif()
