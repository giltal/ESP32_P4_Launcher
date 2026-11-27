"""launcher/partitions.csv against the table lx_layout.c writes when it moves a board to the new layout.

Builds the binary table the way gen_esp32part.py does from k_new[] in components/lx_update/lx_layout.c and
compares it with gen_esp32part.py's output for partitions.csv (ESP-IDF's tool, IDF_PATH or the default path).
    python tests/host/test_layout.py
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
IDF = os.environ.get('IDF_PATH') or os.path.expanduser('~/esp/v5.5.6/esp-idf')


def c_table():
    src = open(os.path.join(ROOT, 'components/lx_update/lx_layout.c'), encoding='utf-8').read()
    body = re.search(r'k_new\[\] = \{(.*?)\n\};', src, re.S).group(1)
    rows = re.findall(r'\{\s*"(\w+)",\s*(\d+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)\s*\}', body)
    t = b''
    for label, typ, sub, addr, size in rows:
        t += struct.pack('<2sBBLL16sL', b'\xAA\x50', int(typ), int(sub, 16), int(addr, 16), int(size, 16),
                         label.encode(), 0)
    t += b'\xEB\xEB' + b'\xFF' * 14 + hashlib.md5(t).digest()
    return t + b'\xFF' * (0xC00 - len(t)), len(rows)


def csv_table():
    out = os.path.join(tempfile.mkdtemp(), 'pt.bin')
    subprocess.check_call([sys.executable, os.path.join(IDF, 'components/partition_table/gen_esp32part.py'),
                           '--flash-size', '16MB', '-q', os.path.join(ROOT, 'launcher/partitions.csv'), out])
    return open(out, 'rb').read()


def main():
    c, n = c_table()
    g = csv_table()
    if c != g:
        print('MISMATCH: lx_layout.c k_new[] (%d entries) differs from launcher/partitions.csv' % n)
        for i in range(0, 0xC00, 32):
            if c[i:i + 32] != g[i:i + 32]:
                print('  entry %d\n    C:   %s\n    csv: %s' % (i // 32, c[i:i + 32].hex(), g[i:i + 32].hex()))
        return 1
    print('ok: %d partitions, table identical (%d bytes)' % (n, len(g)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
