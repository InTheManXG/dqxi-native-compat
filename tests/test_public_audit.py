"""合成仓库证明扫描器会拒绝产物、凭据模式及来源清单不匹配。"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

script = Path(__file__).resolve().parents[1] / 'tools/audit_public.py'
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / 'tools').mkdir()
    shutil.copyfile(script, root / 'tools/audit_public.py')
    source = root / 'fixture.c'
    source.write_text('/* synthetic source */\n')
    original = source.read_bytes()
    (root / 'SOURCE_MANIFEST.json').write_text(json.dumps({'files': [{'path': source.name, 'sha256': hashlib.sha256(original).hexdigest()}]}))
    def run(ok):
        result = subprocess.run([sys.executable, str(root / 'tools/audit_public.py')], capture_output=True)
        assert (result.returncode == 0) == ok
    run(True)
    extra = root / 'forbidden.bin'
    extra.write_bytes(b'fixture'); run(False); extra.unlink()
    extra = root / 'fixture.txt'
    extra.write_text('ghp_' + 'a' * 36); run(False); extra.unlink()
    source.write_bytes(original + b'changed'); run(False)
    source.write_bytes(original); run(True)
print('PASS: publication scanner accepts clean source and rejects artifacts, token patterns and modified source')
