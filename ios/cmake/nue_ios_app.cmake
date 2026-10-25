# Included by nue_engine/ios/CMakeLists.txt when CMAKE_SYSTEM_NAME is iOS: Objective-C++ glue library + SDL3 + sample app.
# Never run: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 17)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)

# SDL3: use the core's checkout when it provides one, otherwise fetch the pinned release. Keep the tag in sync with nue-core.
set(NUE_SDL3_TAG "release-3.4.18" CACHE STRING "SDL3 git tag fetched when NUE_SDL3_SOURCE_DIR is not set")
set(NUE_SDL3_SOURCE_DIR "" CACHE PATH "Existing SDL3 source tree (overrides the fetch)")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
if(NUE_SDL3_SOURCE_DIR)
    add_subdirectory(${NUE_SDL3_SOURCE_DIR} ${CMAKE_BINARY_DIR}/sdl3 EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG ${NUE_SDL3_TAG} GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
endif()

# ---- glue library --------------------------------------------------------------------------------------------------------------------------------
file(GLOB NUE_IOS_GLUE_SOURCES CONFIGURE_DEPENDS ${NUE_IOS_SRC}/apple/*.mm)
add_library(nue_ios_glue STATIC ${NUE_IOS_GLUE_SOURCES})
target_include_directories(nue_ios_glue PUBLIC ${NUE_IOS_SRC})
target_link_libraries(nue_ios_glue PUBLIC nue_ios_logic)
target_compile_options(nue_ios_glue PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(nue_ios_glue INTERFACE
    "-framework Foundation" "-framework UIKit" "-framework Metal" "-framework QuartzCore" "-framework AVFoundation"
    "-framework GameController" "-framework CoreHaptics" "-framework CoreMotion" "-framework MetricKit"
    "-framework UniformTypeIdentifiers")
set_target_properties(nue_ios_glue PROPERTIES XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES)

# ---- sample app ----------------------------------------------------------------------------------------------------------------------------------
set(NUE_IOS_BUNDLE_ID "org.nue.sample" CACHE STRING "Bundle identifier of the sample app (change it for your own signing team)")
set(NUE_IOS_TEAM_ID "" CACHE STRING "Apple development team id used for signing (empty: unsigned build)")
add_executable(nue_ios_sample MACOSX_BUNDLE ${CMAKE_CURRENT_LIST_DIR}/../sample/main.mm)
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../sample/main.mm PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_compile_options(nue_ios_sample PRIVATE -fobjc-arc)
target_link_libraries(nue_ios_sample PRIVATE nue_ios_glue SDL3::SDL3)
set_target_properties(nue_ios_sample PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/../sample/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${NUE_IOS_BUNDLE_ID}
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    XCODE_ATTRIBUTE_ARCHS "arm64"
    XCODE_ATTRIBUTE_ONLY_ACTIVE_ARCH NO
    XCODE_ATTRIBUTE_ENABLE_BITCODE NO
    XCODE_ATTRIBUTE_GCC_GENERATE_DEBUGGING_SYMBOLS YES
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic)
if(NUE_IOS_TEAM_ID)
    set_target_properties(nue_ios_sample PROPERTIES XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ${NUE_IOS_TEAM_ID})
else()
    set_target_properties(nue_ios_sample PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED NO)
endif()
