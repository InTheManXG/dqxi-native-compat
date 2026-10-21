#!/bin/sh
# 只链接通用线程、装载器和现有 Metal 后端，不带暗黑专用逻辑。
# Link only generic threading, the loader and the existing Metal backend, without D2R-specific gameplay logic.
set -eu
cd "$(dirname "$0")/../.."
output=${1:?usage: build_probe_metal.sh /absolute/path/to/module_probe_metal}
case "$output" in /*) ;; *) echo 'output must be absolute' >&2; exit 2;; esac
object=$(mktemp -t dqxi-probe-object)
trap 'rm -f "$object"' EXIT HUP INT TERM
xcrun clang -O1 -g -DDM_PROBE_METAL -Dmain=module_probe_main -c dm/verify/module_probe.c -o "$object"
xcrun clang -O1 -g -fobjc-arc -Idm/gfx/nvn/include -DDM_NVN_M3=1 -DDM_PROBE_METAL -Wl,-dead_strip \
    "$object" dm/verify/probe_window.m dm/verify/probe_keyboard.m dm/host/loader.c dm/host/entry.S \
    dm/host/thread.c dm/host/hle_thread.c dm/host/crash.c dm/gfx/hle_nv.c dm/gfx/hle_vi.c \
    dm/gfx/nvn/*.c dm/gfx/nvn/*.m dm/gfx/nvn/*.S dm/gfx/tex/*.c dm/gfx/tex/*.m \
    -framework GameController -framework AudioToolbox -framework AppKit -framework Metal -framework Foundation -framework QuartzCore \
    -framework ImageIO -framework CoreGraphics -framework UniformTypeIdentifiers -o "$output"
