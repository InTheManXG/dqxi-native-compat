#!/bin/sh
# 启动已构建的独立原生移植；不触发构建，不修改原始游戏资源。
# Launch the built isolated native port without rebuilding or altering original assets.
set -eu
root=${1:?usage: run_probe_game.sh /absolute/port/root}
case "$root" in /*) ;; *) echo 'port root must be absolute' >&2; exit 2;; esac
for file in tools/module_probe_metal tools/AudioBridge.dylib tools/NativeVP9.dylib tools/libvpx.12.dylib game/romfs-storage.bin game/shader-map.tsv; do
    [ -f "$root/$file" ] || { echo "missing: $root/$file" >&2; exit 2; }
done
mkdir -p "$root/logs"
log="$root/logs/native-$(date +%Y%m%d-%H%M%S).log"
export DM_PROBE_INTERACTIVE=1
# exFAT 资源盘不支持存档事务，允许启动入口指定内置 APFS 存档目录。
export DM_PROBE_SAVE_DIR="${DM_PROBE_SAVE_DIR:-$root/game/saves}"
# 本机管线记录与存档同盘，下次启动配合 game/pso_manifest.tsv 提前编译。
export DM_PSO_USER="${DM_PSO_USER:-$(dirname -- "$DM_PROBE_SAVE_DIR")/pso_user.tsv}"
export DM_PROBE_AUDIO_LIB="$root/tools/AudioBridge.dylib"
export DM_PROBE_VIDEO_LIB="$root/tools/NativeVP9.dylib"
export DM_PROBE_ROMFS="$root/game/romfs-storage.bin"
export DM_NVN_SHMAP="$root/game/shader-map.tsv" DM_MSL_DIR="$root/game/shader-cache"
export DM_NVN_SYNC=1
printf 'DQXI S 原生实验版：默认 Enter 确认，Esc 返回，方向键导航，WASD 移动，IJKL 镜头，P 为 Plus。\n点击 macOS 应用菜单可设置帧率显示、键盘映射及恢复默认键位。\n日志：%s\n' "$log"
exec "$root/tools/module_probe_metal" "$root/game/rewritten-verified" --start _ZN2nn4init5StartEmmPFvvES2_ main subsdk0 sdk >"$log" 2>&1
