# NUE platform layer for iOS: included by nue_engine/CMakeLists.txt when the target OS is iOS (see docs/PORTING.md).
#   target_sources(nue_platform_obj ...) adds: adapter (nue_platform_ios_get_ops), glue (nue_ios.h services), pure logic.
# Never configured on iOS: written without macOS/Xcode access (see docs/PLATFORM_IOS.md).
enable_language(OBJCXX)   # the .mm files; requires CMake >= 3.21 (the root requires 3.21)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

set(NUE_IOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

target_sources(nue_platform_obj PRIVATE
    ${NUE_IOS_DIR}/adapter/platform_ios.mm
    ${NUE_IOS_DIR}/apple/nue_ios_core.mm
    ${NUE_IOS_DIR}/apple/nue_ios_storage.mm
    ${NUE_IOS_DIR}/apple/nue_ios_audio.mm
    ${NUE_IOS_DIR}/apple/nue_ios_input.mm
    ${NUE_IOS_DIR}/apple/nue_ios_display.mm
    ${NUE_IOS_DIR}/apple/nue_ios_system.mm
    ${NUE_IOS_DIR}/apple/nue_ios_diag.mm
    ${NUE_IOS_DIR}/logic/lifecycle.cpp
    ${NUE_IOS_DIR}/logic/power.cpp
    ${NUE_IOS_DIR}/logic/caps.cpp
    ${NUE_IOS_DIR}/logic/storage.cpp
    ${NUE_IOS_DIR}/logic/audio_policy.cpp
    ${NUE_IOS_DIR}/logic/input_logic.cpp
    ${NUE_IOS_DIR}/logic/nue_map.cpp)

target_include_directories(nue_platform_obj PRIVATE
    ${NUE_IOS_DIR}            # nue_ios.h
    ${NUE_IOS_DIR}/apple      # nue_ios_internal.hpp
    ${NUE_IOS_DIR}/logic      # lifecycle.hpp, nue_map.hpp ...
    ${NUE_IOS_DIR}/include)   # nue_ext_ios.h

set_source_files_properties(
    ${NUE_IOS_DIR}/adapter/platform_ios.mm
    ${NUE_IOS_DIR}/apple/nue_ios_core.mm ${NUE_IOS_DIR}/apple/nue_ios_storage.mm ${NUE_IOS_DIR}/apple/nue_ios_audio.mm
    ${NUE_IOS_DIR}/apple/nue_ios_input.mm ${NUE_IOS_DIR}/apple/nue_ios_display.mm ${NUE_IOS_DIR}/apple/nue_ios_system.mm
    ${NUE_IOS_DIR}/apple/nue_ios_diag.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# Frameworks used by the glue (static library: the final app links them through the library's interface).
target_link_libraries(nue_platform_obj PUBLIC
    "-framework Foundation" "-framework UIKit" "-framework Metal" "-framework QuartzCore" "-framework AVFoundation"
    "-framework GameController" "-framework CoreHaptics" "-framework CoreMotion" "-framework MetricKit"
    "-framework UniformTypeIdentifiers")
