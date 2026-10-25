#!/usr/bin/env bash
# Builds the NUE SDK and a runtime redistributable for Linux from this repository.
#
#   packaging/build-redist.sh [--preset linux-x64] [--out dist] [--shared] [--static-cxx]
#
# Outputs (in --out):
#   nue-sdk-<version>-linux-<arch>.tar.gz       headers, libnue (static, or shared with --shared), CMake package config, docs
#   nue-runtime-<version>-linux-<arch>.tar.gz   the sample programs, linux_probe, the smoke test and the dependency list
#
# Needs: cmake >= 3.21, ninja, a C++17 compiler, the development packages of SDL3's backends (see README.md). Run it on the OLDEST
# distribution you want to support (glibc is forward compatible only). Nothing here is run by the engine's own tests.
set -euo pipefail

preset=linux-x64
out=dist
shared=OFF
static_cxx=0
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --shared) shared=ON; shift ;;
        --static-cxx) static_cxx=1; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../../.." && pwd)"
cd "$repo"
build="$repo/build/$preset"
arch="$(uname -m)"
version="$(sed -n 's/^project(nue VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt | head -n 1)"
[ -n "$version" ] || version=0.0.0

extra=()
if [ "$static_cxx" = 1 ]; then
    # libstdc++ / libgcc inside the executables: the redistributable then needs only glibc and the optional dlopen libraries.
    extra+=("-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc" "-DCMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -static-libgcc")
fi

cmake --preset "$preset" -DNUE_BUILD_SHARED="$shared" -DNUE_BUILD_SAMPLES=ON -DNUE_INSTALL=ON "${extra[@]}"
cmake --build --preset "$preset"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cmake --install "$build" --prefix "$stage/sdk"

mkdir -p "$stage/runtime/bin" "$stage/runtime/share/doc/nue"
found=0
while IFS= read -r exe; do
    cp "$exe" "$stage/runtime/bin/"
    found=1
done < <(find "$build" -maxdepth 6 -type f \( -name 'nue_sample_*' -o -name linux_probe \) -perm -u+x)
[ "$found" = 1 ] || { echo "no sample binaries found under $build" >&2; exit 1; }
cp "$here/smoke-test.sh" "$stage/runtime/"
cp "$here/RUNTIME_DEPENDENCIES.txt" "$stage/runtime/share/doc/nue/"
[ -f LICENSE ] && cp LICENSE "$stage/runtime/share/doc/nue/" || true
cp -r "$repo/third_party/SDL/LICENSE.txt" "$stage/runtime/share/doc/nue/SDL3-LICENSE.txt" 2>/dev/null || true

mkdir -p "$out"
sdk_tar="$out/nue-sdk-$version-linux-$arch.tar.gz"
rt_tar="$out/nue-runtime-$version-linux-$arch.tar.gz"
tar -C "$stage/sdk" -czf "$sdk_tar" .
tar -C "$stage/runtime" -czf "$rt_tar" .
echo "wrote $sdk_tar"
echo "wrote $rt_tar"
