"""验证只修改单条指令，错版本、错指令和已有文件都拒绝。"""
import hashlib
import struct
import tempfile
from pathlib import Path
import patch_memory as patcher
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);source=root/'source';out=root/'patched'
    # 小型已知模块夹具，沿用相同版本校验和指令替换路径。
    patcher.OFFSET=16
    data=bytearray(64);struct.pack_into('<II',data,16,patcher.OLD,0xf901a721)
    patcher.SOURCE_SHA256=hashlib.sha256(data).hexdigest();source.write_bytes(data)
    patcher.patch(source,out)
    expected=data[:];struct.pack_into('<I',expected,16,patcher.NEW);assert out.read_bytes()==expected
    try:patcher.patch(source,out)
    except FileExistsError:pass
    else:raise AssertionError('overwrote output')
    data[0]=1;source.write_bytes(data)
    try:patcher.patch(source,root/'bad')
    except ValueError:pass
    else:raise AssertionError('accepted wrong version')
    data[16]=0;source.write_bytes(data);patcher.SOURCE_SHA256=hashlib.sha256(data).hexdigest()
    try:patcher.patch(source,root/'bad')
    except ValueError:pass
    else:raise AssertionError('accepted wrong instruction')
print('arena patch: exact instruction, version/shape validation and no overwrite OK')
