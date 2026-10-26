#!/usr/bin/env python3
"""Decode the last complete P-key engine-frame capture from a device serial log."""
import argparse
from pathlib import Path
import re
import struct
import zlib


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)


def decode(text):
    captures = re.findall(r'QSHOT 320 200 (\d+)\s+(.*?)QEND', text, re.S)
    if not captures:
        raise ValueError('no complete 320x200 capture')
    frame, payload = captures[-1]
    palette, rows = {}, {}
    for n, line in re.findall(r'QPAL (\d+) ([0-9a-f]{64})', payload):
        n = int(n)
        if n >= 16 or n in palette:
            raise ValueError('bad or duplicate palette row')
        palette[n] = [int(line[i:i+4], 16) for i in range(0, 64, 4)]
    for n, line in re.findall(r'QROW (\d+) ([0-9a-f]{640})', payload):
        n = int(n)
        if n >= 200 or n in rows:
            raise ValueError('bad or duplicate pixel row')
        rows[n] = bytes.fromhex(line)
    if len(palette) != 16 or len(rows) != 200:
        raise ValueError('incomplete palette/pixels')
    colors = [v for i in range(16) for v in palette[i]]
    raw = bytearray()
    for y in range(200):
        raw.append(0)
        for index in rows[y]:
            v = colors[index]
            raw.extend((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 320, 200, 8, 2, 0, 0, 0))
    return int(frame), png + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    frame, png = decode(args.log.read_text(errors='replace'))
    args.output.write_bytes(png)
    print('Captured engine frame', frame, 'to', args.output)

if __name__ == '__main__':
    main()
