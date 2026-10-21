"""检查真实 IPA 的 iOS 启动契约，避免缺少 Scene 生命周期或桌面图标。"""
import plistlib
import sys
import zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    root='Payload/DQXI S.app/'
    info=plistlib.loads(archive.read(root+'Info.plist'))
    scenes=info.get('UIApplicationSceneManifest',{}).get('UISceneConfigurations',{}).get('UIWindowSceneSessionRoleApplication',[])
    assert scenes, 'iOS 新系统要求声明 UIWindowScene 生命周期，不能只使用 UIApplicationDelegate 创建窗口'
    executable=archive.read(root+info['CFBundleExecutable'])
    for scene in scenes:
        name=scene['UISceneDelegateClassName']
        assert ('_OBJC_CLASS_$_'+name).encode() in executable, f'Scene 类未链接：{name}'
    icon=info.get('CFBundleIcons',{}).get('CFBundlePrimaryIcon',{})
    assert icon.get('CFBundleIconName'), '缺少主应用图标配置'
    assert root+'Assets.car' in archive.namelist(), '缺少编译后的图标资源'
    assert any(n.startswith(root+'AppIcon') and n.endswith('.png') for n in archive.namelist()), '缺少桌面 PNG 图标'
    assert not any('/._' in n for n in archive.namelist()), '包含 AppleDouble 文件'
print('IPA launch contract: linked Scene delegate, compiled icons and clean archive OK')
