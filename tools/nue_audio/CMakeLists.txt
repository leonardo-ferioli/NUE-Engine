add_executable(nue_audio_tool main.cpp)
set_target_properties(nue_audio_tool PROPERTIES OUTPUT_NAME nue_audio)
target_link_libraries(nue_audio_tool PRIVATE nue::audio nue_internal)
