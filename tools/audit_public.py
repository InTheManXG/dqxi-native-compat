"""检查公开源码边界及源清单，不输出疑似凭据正文；不能代替法律审查。"""
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
BAD = {'.ipa', '.ipa1', '.dylib', '.metallib', '.nsp', '.nsz', '.nca', '.nso', '.xci', '.bin', '.keys', '.p12', '.mobileprovision', '.cer', '.png', '.jpg', '.ips', '.log'}
# 分段构造，以免扫描器将自身的示例当作凭据；只报告文件名。
patterns = [r'BEGIN [A-Z ]*PRIVATE' + r' KEY', r'gh[pousr]_' + r'[A-Za-z0-9]{20,}', r'AKIA' + r'[A-Z0-9]{16}']
errors = []
for path in ROOT.rglob('*'):
    if '.git' in path.relative_to(ROOT).parts or not path.is_file():
        continue
    rel = path.relative_to(ROOT).as_posix()
    if path.is_symlink() or path.suffix.lower() in BAD or path.name.startswith('._'):
        errors.append(f'禁止发布的文件: {rel}')
    try:
        text = path.read_text()
    except UnicodeError:
        errors.append(f'非文本文件: {rel}')
        continue
    if any(re.search(pattern, text) for pattern in patterns):
        errors.append(f'疑似凭据: {rel}')
    if re.search(r'/Users/[A-Za-z0-9_.-]+/', text):
        errors.append(f'个人路径: {rel}')
manifest = json.loads((ROOT / 'SOURCE_MANIFEST.json').read_text())
for item in manifest['files']:
    path = ROOT / item['path']
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
        errors.append(f'来源清单不匹配: {item["path"]}')
assert not errors, '\n'.join(errors)
print(f'PASS: source manifest ({len(manifest["files"])} files), publication file boundary and credential-pattern scan')
