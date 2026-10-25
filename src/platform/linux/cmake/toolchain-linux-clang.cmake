# Cross-compile NUE for Linux with clang + lld and a Linux sysroot (Debian/Ubuntu/Steam Runtime "sniper" SDK image
# extracted to a directory). Not tested on the development machine (no sysroot there); the flags are the standard ones.
#
#   cmake -S nue_engine -B build-linux-x64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=nue_engine/src/platform/linux/cmake/toolchain-linux-clang.cmake \
#         -DNUE_LINUX_TRIPLE=x86_64-linux-gnu -DNUE_LINUX_SYSROOT=/path/to/sysroot
#   (arm64: -DNUE_LINUX_TRIPLE=aarch64-linux-gnu with an arm64 sysroot)
set(CMAKE_SYSTEM_NAME Linux)
if(NOT NUE_LINUX_TRIPLE)
  set(NUE_LINUX_TRIPLE x86_64-linux-gnu)
endif()
if(NUE_LINUX_TRIPLE MATCHES "^aarch64")
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
else()
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${NUE_LINUX_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${NUE_LINUX_TRIPLE})
if(NUE_LINUX_SYSROOT)
  set(CMAKE_SYSROOT "${NUE_LINUX_SYSROOT}")
  set(CMAKE_FIND_ROOT_PATH "${NUE_LINUX_SYSROOT}")
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)  # no running of target binaries during configure
