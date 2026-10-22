# Minimal iOS toolchain file for CMake >= 3.21 (arm64 only). CMake itself knows how to build for iOS when CMAKE_SYSTEM_NAME is "iOS" and
# the Xcode generator is used; this file only selects the SDK, architecture and deployment target.
#   NUE_IOS_PLATFORM = device (default) | simulator
# Never run: written without macOS/Xcode access (see nue_engine/docs/PLATFORM_IOS.md).
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "")
if(NOT DEFINED NUE_IOS_PLATFORM)
    set(NUE_IOS_PLATFORM device)
endif()
if(NUE_IOS_PLATFORM STREQUAL "simulator")
    set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "")
else()
    set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "")
endif()
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "")
endif()
# Apple clang only. No bitcode (removed in Xcode 14). Static libraries only: apps link everything statically.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "")
