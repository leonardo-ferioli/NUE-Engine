# legacy_dinput: the DirectInput 8 device model (device enumeration, data formats, state, buffered data, properties, cooperative levels, acquire rules, action maps for
# keyboard and mouse) as plain C++ over the platform input interface. No COM, no guest pointers, no game knowledge. Added by src/legacy/win32/CMakeLists.txt, which
# also owns the platform interfaces it is written against.
add_library(nue_legacy_dinput STATIC src/dinput.cpp)
add_library(nue::legacy_dinput ALIAS nue_legacy_dinput)
target_link_libraries(nue_legacy_dinput PUBLIC nue_legacy_platform)
set_target_properties(nue_legacy_dinput PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NUE_INSTALL)
    include(GNUInstallDirs)
    set_target_properties(nue_legacy_dinput PROPERTIES EXPORT_NAME legacy_dinput)
    install(TARGETS nue_legacy_dinput EXPORT nueTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
