# nue_pack: list / extract / convert / pack / verify / export / repack for game resource files and .nuepack.
add_executable(nue_pack main.cpp)
target_link_libraries(nue_pack PRIVATE nue::assets nue_internal)
if(WIN32)
    target_compile_definitions(nue_pack PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
