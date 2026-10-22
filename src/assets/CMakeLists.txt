# NUE asset module: library nue_assets (static; alias nue::assets), the command line tool tools/nue_pack and tests/assets.
#   include/nue/assets.h   public C API
#   src/assets/*.cpp       framework: memory, sources, codecs (JDLZ), registry, containers, .nuepack, pack mount
#   src/assets/formats/    format plugins (nfs/ = Need for Speed Underground 2 BUN/LZC)
# The module only needs the public engine API (nue::nue): storage, memory budget, texture module. Root contract: the root CMakeLists calls
# add_subdirectory(src/assets) after the engine library is defined (request in docs/REQUESTS_assets.md).
cmake_minimum_required(VERSION 3.21)

option(NUE_ASSETS_TESTS "Build tests/assets" ${NUE_BUILD_TESTS})
option(NUE_ASSETS_TOOLS "Build tools/nue_pack" ON)

get_filename_component(NUE_ASSETS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

file(GLOB NUE_ASSETS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/formats/*/*.cpp")
add_library(nue_assets STATIC ${NUE_ASSETS_SOURCES})
add_library(nue::assets ALIAS nue_assets)
target_include_directories(nue_assets PUBLIC
    "$<BUILD_INTERFACE:${NUE_ASSETS_ROOT}/include>"
    "$<INSTALL_INTERFACE:include>")
target_include_directories(nue_assets PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_compile_features(nue_assets PUBLIC cxx_std_17)
target_link_libraries(nue_assets PUBLIC nue::nue PRIVATE "$<BUILD_INTERFACE:nue_internal>")
set_target_properties(nue_assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NUE_INSTALL)
    install(TARGETS nue_assets EXPORT nueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(NUE_ASSETS_TESTS AND EXISTS "${NUE_ASSETS_ROOT}/tests/assets/CMakeLists.txt")
    add_subdirectory("${NUE_ASSETS_ROOT}/tests/assets" "${CMAKE_CURRENT_BINARY_DIR}/tests_assets")
endif()
if(NUE_ASSETS_TOOLS AND EXISTS "${NUE_ASSETS_ROOT}/tools/nue_pack/CMakeLists.txt")
    add_subdirectory("${NUE_ASSETS_ROOT}/tools/nue_pack" "${CMAKE_CURRENT_BINARY_DIR}/nue_pack")
endif()
