#!/bin/sh
# 固定公开音频实现版本；只构建音频与内存接口，不链接 CPU 执行器。
# Pin the public audio implementation; build audio/memory contracts without a CPU executor.
set -eu
source_root=${1:?usage: build.sh /absolute/audio-source-checkout /absolute/output}
output=${2:?output directory required}
case "$source_root:$output" in /*:/*) ;; *) echo 'absolute paths required' >&2; exit 2;; esac
pin=4930dced25c0d9ec33096b0f2601d262daa477bf
[ "$(git -C "$source_root" rev-parse HEAD)" = "$pin" ] || { echo 'audio source revision mismatch' >&2; exit 2; }
git -C "$source_root" diff --quiet HEAD -- src/Ryujinx.Audio src/Ryujinx.Memory || { echo 'audio source modified' >&2; exit 2; }
project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"${DOTNET:-dotnet}" publish "$project/AudioBridge.csproj" -c Release -r osx-arm64 -p:AudioSource="$source_root/src" -o "$output"
cp "$project/LICENSE.audio-source.txt" "$output/"
xcrun clang -O1 -g -framework AudioToolbox "$project/test_audio_bridge.c" -o "$output/test_audio_bridge"
"$output/test_audio_bridge" "$output/AudioBridge.dylib"
