# Cross-compile NUE for Linux from any host with `zig cc` (Zig bundles clang, lld and glibc/musl headers for every
# Linux target, so no sysroot is needed).
#
#   cmake -S nue_engine -B build-linux-x64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=nue_engine/src/platform/linux/cmake/toolchain-linux-zig.cmake \
#         -DNUE_ZIG_TARGET=x86_64-linux-gnu.2.31          # the suffix pins the glibc version (2.31 = Ubuntu 20.04 / Steam Runtime soldier)
#   arm64: -DNUE_ZIG_TARGET=aarch64-linux-gnu.2.31
#
# `zig cc` takes the target on its command line, so small launcher scripts carrying zig's path and the target are
# generated into the build directory at configure time.
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
set(_nue_dir "${CMAKE_BINARY_DIR}/nue-zig")
file(MAKE_DIRECTORY "${_nue_dir}")
if(CMAKE_HOST_WIN32)
  set(_ext cmd)
  file(TO_NATIVE_PATH "${NUE_ZIG}" _zig)
  file(WRITE "${_nue_dir}/cc.cmd"     "@echo off\r\n\"${_zig}\" cc -target ${NUE_ZIG_TARGET} %*\r\n")
  file(WRITE "${_nue_dir}/c++.cmd"    "@echo off\r\n\"${_zig}\" c++ -target ${NUE_ZIG_TARGET} %*\r\n")
  file(WRITE "${_nue_dir}/ar.cmd"     "@echo off\r\n\"${_zig}\" ar %*\r\n")
  file(WRITE "${_nue_dir}/ranlib.cmd" "@echo off\r\nexit /b 0\r\n")
else()
  set(_ext sh)
  foreach(_t cc c++ ar)
    if(_t STREQUAL "ar")
      file(WRITE "${_nue_dir}/ar.sh" "#!/bin/sh\nexec \"${NUE_ZIG}\" ar \"$@\"\n")
    else()
      file(WRITE "${_nue_dir}/${_t}.sh" "#!/bin/sh\nexec \"${NUE_ZIG}\" ${_t} -target ${NUE_ZIG_TARGET} \"$@\"\n")
    endif()
  endforeach()
  file(WRITE "${_nue_dir}/ranlib.sh" "#!/bin/sh\nexit 0\n")
  file(CHMOD "${_nue_dir}/cc.sh" "${_nue_dir}/c++.sh" "${_nue_dir}/ar.sh" "${_nue_dir}/ranlib.sh" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()
set(CMAKE_C_COMPILER "${_nue_dir}/cc.${_ext}")
set(CMAKE_CXX_COMPILER "${_nue_dir}/c++.${_ext}")
set(CMAKE_AR "${_nue_dir}/ar.${_ext}" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_nue_dir}/ranlib.${_ext}" CACHE FILEPATH "" FORCE)   # `zig ar` writes the index itself
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
