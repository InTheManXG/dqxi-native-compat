"""将已编译的 iOS 外壳与个人游戏模块组装为待签名 IPA；大资源不重复打包。"""
import importlib.util
import plistlib
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from patch_memory import patch as patch_memory

game, audio, output = map(lambda p: Path(p).resolve(), sys.argv[1:])
spec = importlib.util.spec_from_file_location('guest', Path(__file__).resolve().parents[2] / 'tools/guest_dylib.py')
guest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guest)
# 在本机临时盘签名，避免 exFAT 的 AppleDouble 元数据污染应用包。
staging = Path(tempfile.mkdtemp(prefix='dqxi-ios-package-'))
app = staging/'Payload/DQXI S.app'
app.mkdir(parents=True, exist_ok=False)
frameworks = app/'Frameworks'
frameworks.mkdir()
for source, name in ((audio, 'AudioBridge'), (output/'NativeVP9', 'NativeVP9')):
    # 平台必须来自实际编译结果，不能把 arm64 macOS 库改标签当作 iOS 库。
    result = subprocess.check_output(['xcrun','vtool','-show-build',str(source)],text=True)
    if 'platform IOS\n' not in result: raise ValueError(f'not an iOS library: {source}')
    fw = frameworks/f'{name}.framework'
    fw.mkdir()
    shutil.copy2(source,fw/name)
    subprocess.run(['install_name_tool','-id',f'@rpath/{name}.framework/{name}',str(fw/name)],check=True)
    info = dict(CFBundleExecutable=name,CFBundleIdentifier=f'cn.iamxg.dqxi-ios.{name.lower()}',CFBundleName=name,
                CFBundlePackageType='FMWK',CFBundleVersion='6',CFBundleShortVersionString='0.1.5',MinimumOSVersion='26.0',CFBundleSupportedPlatforms=['iPhoneOS'])
    (fw/'Info.plist').write_bytes(plistlib.dumps(info))
# 只修改本次打包副本；原 Mac 游戏及其签名模块不动。
embedded=staging/'embedded-modules'
embedded.mkdir()
for name in ('main','subsdk0','sdk'):
    source=game/'rewritten-verified'/f'{name}.patched.bin'
    destination=embedded/source.name
    if name=='main':patch_memory(source,destination)
    else:shutil.copyfile(source,destination)
    shutil.copyfile(source.with_name(f'{name}.fixups.tsv'),embedded/f'{name}.fixups.tsv')
guest.write(embedded,frameworks,['main','subsdk0','sdk'],bundle_id='cn.iamxg.dqxi-ios.guest')
if 'platform IOS\n' not in subprocess.check_output(['xcrun','vtool','-show-build',str(output/'DQXI')],text=True):
    raise ValueError('host executable is not iOS')
shutil.copy2(output/'DQXI',app/'DQXI')
licenses=app/'Licenses'
licenses.mkdir()
verify=Path(__file__).resolve().parents[1]
for source in (verify/'audio/LICENSE.audio-source.txt',verify/'video/LICENSE.libvpx.txt'):
    shutil.copy2(source,licenses/source.name)
(app/'modules').mkdir()
for name in ('main','subsdk0','sdk'):
    for suffix in ('fixups.tsv','gates.tsv'):
        shutil.copy2(game/'rewritten-verified'/f'{name}.{suffix}',app/'modules'/f'{name}.{suffix}')
shutil.copy2(game/'shader-map.tsv',app/'shader-map.tsv')
# 现有缓存保存的是可跨平台编译的 MSL；拒绝夹带仅限 macOS 的 metallib。
for source in (game/'shader-cache').rglob('*'):
    if source.name.startswith('._') or not source.is_file() or source.suffix not in ('.metallib','.json'): continue
    if source.suffix == '.metallib':
        with source.open('rb') as f:
            if f.read(4)==b'MTLB': raise ValueError(f'precompiled shader requires platform verification: {source}')
    target=app/'shader-cache'/source.relative_to(game/'shader-cache')
    target.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(source,target)
if (game/'pso_manifest.tsv').exists(): shutil.copy2(game/'pso_manifest.tsv',app/'pso_manifest.tsv')
info=dict(CFBundleExecutable='DQXI',CFBundleIdentifier='cn.iamxg.dqxi-native.ios',CFBundleName='DQXI S',CFBundleDisplayName='DQXI S',
          CFBundlePackageType='APPL',CFBundleVersion='6',CFBundleShortVersionString='0.1.5',MinimumOSVersion='26.0',
          CFBundleSupportedPlatforms=['iPhoneOS'],UIDeviceFamily=[1,2],UIRequiredDeviceCapabilities=['arm64','metal'],
          UISupportedInterfaceOrientations=['UIInterfaceOrientationLandscapeLeft','UIInterfaceOrientationLandscapeRight'],
          UIRequiresFullScreen=True,UIStatusBarHidden=True,UIViewControllerBasedStatusBarAppearance=True,UILaunchScreen={},
          UIFileSharingEnabled=True,LSSupportsOpeningDocumentsInPlace=True,CADisableMinimumFrameDurationOnPhone=True,
          LSApplicationCategoryType='public.app-category.role-playing-games',ITSAppUsesNonExemptEncryption=False,
          DQRomFSBytes=(game/'romfs-storage.bin').stat().st_size)
info['UIApplicationSceneManifest']={
    'UIApplicationSupportsMultipleScenes':False,
    'UISceneConfigurations':{'UIWindowSceneSessionRoleApplication':[
        {'UISceneConfigurationName':'Default','UISceneDelegateClassName':'DQScene'}]}}
# actool 的输出是系统实际使用的图标声明，不能只拷贝一张 PNG 到应用根目录。
icon_info=plistlib.loads((output/'icons.plist').read_bytes())
info.update(icon_info)
for asset in (output/'icons').iterdir():
    if asset.is_file() and not asset.name.startswith('._'): shutil.copy2(asset,app/asset.name)
(app/'Info.plist').write_bytes(plistlib.dumps(info))
entitlements={'com.apple.developer.kernel.increased-memory-limit':True,'com.apple.developer.kernel.extended-virtual-addressing':True}
(output/'Required.entitlements').write_bytes(plistlib.dumps(entitlements))
# 临时签名供完整性检查；安装前必须由签名工具重新签名整个应用及嵌套框架。
for fw in sorted(frameworks.glob('*.framework')): subprocess.run(['codesign','--force','--sign','-',str(fw)],check=True)
subprocess.run(['codesign','--force','--sign','-','--entitlements',str(output/'Required.entitlements'),str(app)],check=True)
subprocess.run(['codesign','--verify','--deep','--strict',str(app)],check=True)
ipa=output/'DQXI-S-iPhone-touch-unsigned.ipa'
with zipfile.ZipFile(ipa,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
    for file in sorted(app.rglob('*')):
        if file.is_file() and not file.name.startswith('._'): archive.write(file,file.relative_to(staging))
with zipfile.ZipFile(ipa) as archive:
    assert archive.testzip() is None
subprocess.run([sys.executable,str(Path(__file__).with_name('verify_package.py')),str(ipa)],check=True)
print(f'IPA: {ipa} ({ipa.stat().st_size} bytes)')

shutil.rmtree(staging)
