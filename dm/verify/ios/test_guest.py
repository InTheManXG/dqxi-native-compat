"""验证签名段保留前置跳板距离和原字节；兼顾原有尾置布局。"""
import importlib.util
import struct
import tempfile
from pathlib import Path
spec = importlib.util.spec_from_file_location('guest', Path(__file__).resolve().parents[2] / 'tools/guest_dylib.py')
guest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guest)
def sections(binary):
    pos = 32
    result = {}
    for _ in range(struct.unpack_from('<I', binary, 16)[0]):
        cmd, size = struct.unpack_from('<II', binary, pos)
        if cmd == 0x19:
            prot, count = struct.unpack_from('<iI', binary, pos + 60)
            for i in range(count):
                at = pos + 72 + i*80
                name = binary[at:at+16].split(b'\0')[0].decode()
                _, length, offset = struct.unpack_from('<QQI', binary, at+32)
                result[name] = (binary[offset:offset+length], prot)
        pos += size
    return result
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    data = bytes(range(256)) * 256
    (root/'main.patched.bin').write_bytes(data)
    for prefix in (True, False):
        values = dict(TEXT_END='0x8000', PATCH_OFF='0x0' if prefix else '0xc000',
                      IMAGE_OFF='0x4000' if prefix else '0x0', PATCH_END='0x100', KEY_PAGE_OFF='0x8000')
        (root/'main.fixups.tsv').write_text(''.join(f'# {k}\t{v}\n' for k,v in values.items()))
        parts = sections(guest.build(root, ['main']))
        assert parts['__dmmainx0'] == (data[:0x8000], 5)
        assert parts['__img'] == (data[0x8000:] if prefix else data[0x8000:0xc000], 3)
        if prefix: assert '__dmmainx1' not in parts
        else: assert parts['__dmmainx1'] == (data[0xc000:], 5)
    (root/'main.fixups.tsv').write_text('# TEXT_END 0x8000\n# PATCH_OFF 0x0\n# IMAGE_OFF 0x4001\n# PATCH_END 0x100\n# KEY_PAGE_OFF 0x8000\n')
    try: guest.build(root,['main'])
    except AssertionError: pass
    else: raise AssertionError('unaligned image accepted')
print('Guest: prefix/tail exact bytes, permissions and invalid layout OK')
