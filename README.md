# DQXI Native Compatibility — experimental source

面向 Apple Silicon 的实验性原生兼容层源码，包含模块装载、部分系统服务、Metal 图形接口、音视频桥和 UIKit 触屏输入。基于 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 的开源代码裁剪整理。

**这是研究源码，不是游戏，也不是已验证可玩的 iPhone 发行版。**项目与 Nintendo、Square Enix、Blizzard 或 Apple 无关联、无授权背书。名称仅用于说明兼容目标。

## 包含与排除

包含：宿主兼容代码、平台输入、合成测试，以及保留许可声明的第三方源码。

不包含：游戏程序或其转换产物、ROM/资源、着色器包、游戏图像/音频/存档、密钥、签名证书、IPA，以及原开发仓库的 Git 历史。来源不明的社区离线补丁和逐指令翻译的游戏内嵌着色器也已排除。详见 [PUBLICATION_REVIEW.md](PUBLICATION_REVIEW.md)。

此快照不是原项目的无损镜像：内嵌游戏着色器回退不可用；缺少映射时保持原错误处理，不伪造渲染成功。仓库不提供解密、账户验证绕过或游戏获取步骤。拥有游戏副本不自动意味着所有处理行为获得授权，需自行确认适用法律和许可。

## 无游戏数据的验证

Apple Silicon Mac，macOS 26+ SDK/编译工具，Python 3：

```sh
python3 tools/audit_public.py
python3 tests/test_public_audit.py
xcrun clang -Wall -Wextra -Werror -fsanitize=address,undefined \
  dm/verify/ios/test_touch.c -o /tmp/dqxi-touch-test
/tmp/dqxi-touch-test
python3 dm/verify/ios/test_timezone.py
python3 dm/verify/ios/test_patch_memory.py
xcrun clang -O1 dm/verify/probe_services.c dm/host/loader.c dm/host/entry.S \
  dm/host/thread.c dm/host/hle_thread.c -framework Foundation -o /tmp/dqxi-services-test
/tmp/dqxi-services-test
# 紧凑地址布局：上面的 clang 命令另加 -DDM_PROBE_COMPACT_AS=1。
sh dm/verify/build_probe_metal.sh /tmp/dqxi-native-host
```

音频桥另依赖 .NET 10、公开 MIT 音频源码 [alula/Ryujinx](https://github.com/alula/Ryujinx) 的 `4930dced25c0d9ec33096b0f2601d262daa477bf` 提交。`dm/verify/audio/build.sh` 核对固定版本，使用其 `src/Ryujinx.Audio` 和 `src/Ryujinx.Memory`，Common 来自本仓库。视频桥依赖 libvpx 1.15.2，许可见 `dm/verify/video/LICENSE.libvpx.txt`。

## 运行与 iOS 限制

运行还需要用户依法自行准备的兼容模块映像、重定位元数据、资源和着色器映射。公开快照没有完整的资源准备流水线，不能仅克隆仓库就运行游戏。

`dm/verify/ios/build.sh` / `package.py` 是本地组装入口，需要完整 iPhoneOS SDK、iOS NativeAOT 音频库、iOS libvpx，以及用户自行准备的资源。原图标已排除，组装前还需提供自己有权使用的 `dm/verify/ios/Assets.xcassets/AppIcon.appiconset`（PNG和Contents.json）。这些脚本输出的包会嵌入用户提供的游戏模块，**不能因为工具开源就把输出 IPA 作为本项目 Release 发布**。

iOS 触屏支持固定双摇杆、多指、八方向十字键、ABXY组内滑动及后台清键；自由布局编辑尚未实现。完整真机启动、长时间游玩和双设备触屏手感未验收。

## 许可和来源

保留上游 MIT [LICENSE](LICENSE)；第三方及改编算法的单独许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。`SOURCE_MANIFEST.json` 记录源快照、文件哈希及导出修改；不表示所有反向工程相关法律问题已得到确认。不要移除作者或许可声明，也不要将本项目描述成官方移植。
