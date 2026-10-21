"""签名前调整 DQXI 虚拟分配容量默认值；不改游戏实际物理内存预算。"""
import hashlib
from pathlib import Path
import struct
import sys

SOURCE_SHA256='ff4f303a95dc9e68add216a8c26ffbe8a09872df079a3098e7764fd6b09fefb9'
OFFSET=0x1000000+0x177bbe0
OLD=0xb25e03e1  # mov x1, #0x400000000
NEW=0xb26003e1  # mov x1, #0x100000000

def patch(source, destination):
    source,destination=Path(source),Path(destination)
    data=bytearray(source.read_bytes())
    if hashlib.sha256(data).hexdigest()!=SOURCE_SHA256:
        raise ValueError('DQXI 模块版本不匹配，拒绝修改')
    if len(data)<OFFSET+8 or struct.unpack_from('<II',data,OFFSET)!=(OLD,0xf901a721):
        raise ValueError('DQXI 容量赋值指令不匹配')
    # 游戏会先清零容量字段；修改默认赋值使申请、记录和所有边界读取一致。
    struct.pack_into('<I',data,OFFSET,NEW)
    with destination.open('xb') as output: output.write(data)

if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('usage: patch_memory.py SOURCE NEW_OUTPUT')
    patch(*sys.argv[1:])
