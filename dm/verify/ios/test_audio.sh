#!/bin/sh
# 在已启动的 iOS 模拟器执行真实 NativeAOT 音频库，覆盖初始化日志和渲染完成事件。
set -eu
audio=${1:?usage: test_audio.sh /absolute/iossimulator/AudioBridge.dylib SIMULATOR_UDID}
device=${2:?simulator UDID required}
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
xcrun vtool -show-build "$audio" | grep -q 'platform IOSSIMULATOR' || {
    echo 'requires a real iOS simulator library, not macOS or iPhoneOS' >&2; exit 2;
}
xcrun --sdk iphonesimulator clang -target arm64-apple-ios26.0-simulator -O1 \
    -framework AudioToolbox "$here/../audio/test_audio_bridge.c" -o "$temporary/audio-test"
xcrun simctl spawn "$device" "$temporary/audio-test" "$audio"
