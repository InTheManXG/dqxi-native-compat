#!/bin/sh
# 从源码构建原生解码桥并先验证合成帧；不读取游戏内容。
# Build the native bridge and validate a synthetic frame before deployment.
set -eu
cd "$(dirname "$0")"
out=${1:?usage: build.sh /absolute/output/directory}
case "$out" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
prefix=${VPX_PREFIX:-/opt/homebrew/opt/libvpx}
mkdir -p "$out"
xcrun clang -O1 -g -Iomx -I"$prefix/include" native_vp9.c test_native_vp9.c -L"$prefix/lib" -lvpx -o "$out/test_native_vp9"
"$out/test_native_vp9"
xcrun clang -dynamiclib -O1 -g -Iomx -I"$prefix/include" native_vp9.c -L"$prefix/lib" -lvpx -o "$out/NativeVP9.dylib"
install_name_tool -change "$prefix/lib/libvpx.12.dylib" '@loader_path/libvpx.12.dylib' "$out/NativeVP9.dylib"
codesign -s - --force "$out/NativeVP9.dylib"
cp -fL "$prefix/lib/libvpx.12.dylib" "$out/libvpx.12.dylib"
cp LICENSE.libvpx.txt "$out/LICENSE.libvpx.txt"
xcrun clang -O1 -g -Iomx test_probe_video.c -o "$out/test_probe_video"
DM_PROBE_VIDEO_LIB="$out/NativeVP9.dylib" "$out/test_probe_video"
