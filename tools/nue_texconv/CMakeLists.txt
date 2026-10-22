# nue_texconv: texture inspect / convert / compare / plan / benchmark command line tool.
add_executable(nue_texconv main.cpp)
target_link_libraries(nue_texconv PRIVATE ${NUE_TEXTURE_TEST_LIB})
if(WIN32)
    target_compile_definitions(nue_texconv PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
