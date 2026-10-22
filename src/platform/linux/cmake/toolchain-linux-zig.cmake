# Cross-compile NUE for Linux from any host with `zig cc` (Zig bundles clang, lld and glibc/musl headers for every
# Linux target, so no sysroot is needed). Not yet exercised: see PLATFORM_LINUX.md "Verification status".
#
#   cmake -S nue_engine -B build-linux-x64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=nue_engine/src/platform/linux/cmake/toolchain-linux-zig.cmake \
#         -DNUE_ZIG_TARGET=x86_64-linux-gnu.2.31          # the suffix pins the glibc version (2.31 = Ubuntu 20.04 / Steam Runtime soldier)
#   arm64: -DNUE_ZIG_TARGET=aarch64-linux-gnu.2.31
#
# `zig cc` takes the target on its command line, so tiny launcher scripts (tools/zig-cc.*) carry it.
set(CMAKE_SYSTEM_NAME Linux)
if(NOT NUE_ZIG_TARGET)
  set(NUE_ZIG_TARGET x86_64-linux-gnu.2.31)
endif()
if(NUE_ZIG_TARGET MATCHES "^aarch64")
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
else()
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
endif()
find_program(NUE_ZIG zig REQUIRED)
set(ENV{NUE_ZIG_EXE} "${NUE_ZIG}")
set(ENV{NUE_ZIG_TARGET} "${NUE_ZIG_TARGET}")
get_filename_component(_nue_tc_dir "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)
if(CMAKE_HOST_WIN32)
  set(CMAKE_C_COMPILER "${_nue_tc_dir}/zig-cc.cmd")
  set(CMAKE_CXX_COMPILER "${_nue_tc_dir}/zig-c++.cmd")
else()
  set(CMAKE_C_COMPILER "${_nue_tc_dir}/zig-cc.sh")
  set(CMAKE_CXX_COMPILER "${_nue_tc_dir}/zig-c++.sh")
endif()
set(CMAKE_AR "${NUE_ZIG}" CACHE FILEPATH "" FORCE)
set(CMAKE_C_COMPILER_AR "${_nue_tc_dir}/zig-ar.cmd")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
