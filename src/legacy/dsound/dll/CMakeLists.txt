# The COM facade (static, so tests can drive it with their own Device) and the drop-in dsound.dll. Windows only.
add_library(nue_dsound_comlib STATIC com_facade.cpp)
target_link_libraries(nue_dsound_comlib PUBLIC nue_legacy_dsound PRIVATE nue_internal dxguid uuid)
target_include_directories(nue_dsound_comlib PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(nue_dsound_comlib PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

add_library(nue_dsound_dll SHARED main.cpp dsound.def)
set_target_properties(nue_dsound_dll PROPERTIES OUTPUT_NAME dsound PREFIX "" ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dll_import_dsound")
target_link_libraries(nue_dsound_dll PRIVATE nue_dsound_comlib nue_internal user32)
