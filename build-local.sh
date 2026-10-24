#!/usr/bin/env bash
# Build only the Wine 11 classic ARM64 Android unixlib and its mixer test.
set -euo pipefail
if [[ $# -ne 6 ]]; then
    echo "Usage: $0 NDK_ROOT WINE_SOURCE WINE_BUILD ARM64_NTDLL_SO AAUDIO_BRIDGE_SO OUTPUT_DIR" >&2
    exit 2
fi
ndk_root=$(realpath "$1")
wine_source=$(realpath "$2")
wine_build=$(realpath "$3")
ntdll_so=$(realpath "$4")
aaudio_bridge=$(realpath "$5")
mkdir -p "$6"
output_dir=$(realpath "$6")
source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
compiler="$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android28-clang"
readelf_tool="$ndk_root/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
"$readelf_tool" -h "$ntdll_so" | grep -q 'Machine:.*AArch64'
"$readelf_tool" -d "$aaudio_bridge" | grep -q 'Library soname: \[libwaudio.so\]'
flags=(
    -I"$wine_source/dlls/winealsa.drv"
    -I"$wine_build/include" -I"$wine_source/include"
    -D__WINESRC__ -DWINE_UNIX_LIB -fPIC -fvisibility=hidden
    -fno-stack-protector -fno-strict-aliasing -g -O2
)
"$compiler" "${flags[@]}" -c "$source_dir/directaudio.c" -o "$output_dir/directaudio.o"
"$compiler" -shared -Wl,-Bsymbolic,-z,defs,-z,now,--build-id=sha1,-soname,winedirectaudio.so \
    "$output_dir/directaudio.o" "$ntdll_so" "$aaudio_bridge" -llog \
    -o "$output_dir/winedirectaudio.so"
"$compiler" "${flags[@]}" -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "$source_dir/tests/mixer_test.c" -lm -o "$output_dir/mixer-test"
"$compiler" "${flags[@]}" -ffunction-sections -fdata-sections -Wl,--gc-sections \
    "$source_dir/tests/mixer_extended_test.c" -lm -o "$output_dir/mixer-extended-test"
"$compiler" "${flags[@]}" -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -fsanitize=undefined -fsanitize-trap=all \
    "$source_dir/tests/mixer_extended_test.c" -lm -o "$output_dir/mixer-extended-ubsan"
"$readelf_tool" -d "$output_dir/winedirectaudio.so"
"$readelf_tool" --dyn-syms "$output_dir/winedirectaudio.so" | grep '__wine_unix_call'
