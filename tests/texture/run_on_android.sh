#!/bin/bash
# Run the texture tests on an Android device or emulator (arm64, NEON) through adb. Build first (NDK r26+):
#   cmake -S src/texture -B build_tex_android -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26
#   cmake --build build_tex_android
# Then:  tests/texture/run_on_android.sh build_tex_android      (needs `adb devices` to list one arm64 target)
# nue_texture_test_simd compares the NEON build with tests/texture/data/simd_golden.bin (written by the x86 SSE4.1 build); the other tests are the normal suite.
set -e
B="${1:-build_tex_android}"
ADB="${ADB:-adb}"
R=/data/local/tmp/nue_texture_tests
$ADB shell "rm -rf $R && mkdir -p $R/data"
$ADB push "$(dirname "$0")/data/simd_golden.bin" $R/data/ > /dev/null
fail=0
for t in "$B"/tests_texture/nue_texture_test_*; do
  case "$t" in *.*) continue;; esac
  n=$(basename "$t")
  $ADB push "$t" $R/ > /dev/null
  $ADB shell "chmod 755 $R/$n"
  extra=""
  [ "$n" = nue_texture_test_simd ] && extra="$R/data/simd_golden.bin"
  echo "=== $n"
  out=$($ADB shell "cd $R && ./$n $extra; echo EXIT=\$?" | tr -d '\r')
  echo "$out" | tail -4
  echo "$out" | grep -q "EXIT=0" || fail=1
done
exit $fail
