#!/bin/sh
# 只生成独立 iPhone 产物，不替换 Mac 程序、资源或存档。
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../../.." && pwd)
game=${1:?usage: build.sh GAME AUDIO_DYLIB VPX_SOURCE VPX_BUILD OUTPUT}
audio=${2:?}; vpx_source=${3:?}; vpx_build=${4:?}; output=${5:?}
case "$game:$audio:$vpx_source:$vpx_build:$output" in /*:/*:/*:/*:/*) ;; *) echo 'all paths must be absolute' >&2; exit 2;; esac
[ ! -e "$output" ] || { echo 'output already exists; choose a new directory' >&2; exit 2; }
[ -f "$game/romfs-storage.bin" ] && [ -f "$audio" ] && [ -f "$vpx_build/libvpx.a" ]
mkdir -p "$output"
sdk=$(xcrun --sdk iphoneos --show-sdk-path)
cd "$root"
xcrun --sdk iphoneos clang -target arm64-apple-ios26.0 -isysroot "$sdk" -O2 -DDM_PROBE_METAL -DDM_SHELL -Dmain=module_probe_main -c dm/verify/module_probe.c -o "$output/probe.o"
xcrun --sdk iphoneos clang -target arm64-apple-ios26.0 -isysroot "$sdk" -O2 -fobjc-arc \
    -Idm/gfx/nvn/include -DDM_NVN_M3=1 -DDM_PROBE_METAL -DDM_SHELL -Wl,-dead_strip -Wl,-rpath,@executable_path/Frameworks \
    "$output/probe.o" dm/verify/ios/main.m dm/verify/probe_keyboard.m dm/host/loader.c dm/host/entry.S \
    dm/host/thread.c dm/host/hle_thread.c dm/host/crash.c dm/gfx/hle_nv.c dm/gfx/hle_vi.c \
    dm/gfx/nvn/*.c dm/gfx/nvn/*.m dm/gfx/nvn/*.S dm/gfx/tex/*.c dm/gfx/tex/*.m \
    -framework AudioToolbox -framework AVFAudio -framework UIKit -framework Metal -framework Foundation -framework QuartzCore \
    -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers -o "$output/DQXI"
xcrun --sdk iphoneos clang -target arm64-apple-ios26.0 -isysroot "$sdk" -O2 -dynamiclib \
    -Idm/verify/video/omx -I"$vpx_source" -I"$vpx_build" dm/verify/video/native_vp9.c "$vpx_build/libvpx.a" \
    -install_name @rpath/NativeVP9.framework/NativeVP9 -o "$output/NativeVP9"
mkdir -p "$output/icons"
xcrun actool "$here/Assets.xcassets" --compile "$output/icons" --platform iphoneos \
    --minimum-deployment-target 26.0 --target-device iphone --target-device ipad \
    --app-icon AppIcon --output-partial-info-plist "$output/icons.plist"
python3 "$here/package.py" "$game" "$audio" "$output"
