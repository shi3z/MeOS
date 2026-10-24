#!/usr/bin/env python3
"""Optional real-engine smoke with a separately supplied shareware PAK; no download."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def validate(pak):
    data = pak.read_bytes()
    if len(data) < 12:
        raise ValueError('short PAK header')
    magic, offset, size = struct.unpack_from('<4sII', data)
    if magic != b'PACK' or size % 64 or not 0 < size // 64 <= 2048 or offset > len(data) or size > len(data) - offset:
        raise ValueError('invalid PAK directory')
    for at in range(offset, offset + size, 64):
        name, position, length = struct.unpack_from('<56sII', data, at)
        if b'\0' not in name or position > len(data) or length > len(data) - position:
            raise ValueError('invalid PAK entry')
    print('Input PAK:', len(data), 'bytes, SHA256', hashlib.sha256(data).hexdigest(), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pak0', type=Path)
    parser.add_argument('--image', type=Path, default=Path('/tmp/meos-quake-headless.ppm'))
    args = parser.parse_args()
    pak = args.pak0.resolve(strict=True)
    validate(pak)
    output = args.image.resolve()
    with tempfile.TemporaryDirectory(prefix='meos-quake-engine-') as temp:
        build = Path(temp)
        (build / 'data/id1').mkdir(parents=True)
        (build / 'data/id1/pak0.pak').symlink_to(pak)
        (build / 'esp_attr.h').write_text('#pragma once\n#define EXT_RAM_BSS_ATTR\n')
        (build / 'socket.h').write_text('#include <sys/socket.h>\n#include <netinet/in.h>\n#include <arpa/inet.h>\n')
        flags = shlex.split(os.environ.get('CC', 'cc')) + ['-O1', '-g', '-fsanitize=address', '-w', '-fno-builtin', '-DMEOS_QUAKE_PROFILE=1',
            '-I' + str(build), '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'third_party/quakegeneric/source')]
        component = ROOT / 'components/quakeengine'
        sources = [ROOT / 'third_party/quakegeneric/source' / n for n in re.findall(r'\$\{QROOT\}/(\w+\.c)', (component / 'CMakeLists.txt').read_text())]
        sources += [component / 'sys_meos.c', component / 'sound_null.c']
        objects = []
        for source in sources:
            obj = build / (source.stem + '.o')
            subprocess.run(flags + ['-include', str(component / 'compat.h'), '-c', str(source), '-o', str(obj)], check=True)
            objects.append(str(obj))
        binary = build / 'quake'
        subprocess.run(flags + ['-DM5OS_BOARD_TAB5=1', '-DM5OS_QUAKE=1', str(ROOT / 'tools/quake/headless.c'),
            str(ROOT / 'apps/quake/libc.c'), str(ROOT / 'apps/quake/profile.c'), str(ROOT / 'kernel/lib/number.c'), *objects, '-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary), str(build / 'data'), str(output)], check=True, timeout=120)
        if not output.is_file():
            raise RuntimeError('engine did not capture a frame')
        print('Captured:', output)

if __name__ == '__main__':
    main()
