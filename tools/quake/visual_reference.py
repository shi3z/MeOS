#!/usr/bin/env python3
"""Write the synthetic dmaview reference (no game data or third-party packages)."""
import argparse
from pathlib import Path
import struct
import zlib

COLORS = (0x0000, 0xffff, 0xf800, 0x07e0, 0x001f, 0xffe0, 0x07ff, 0xf81f, 0x8410, 0x4208)

def pixel(x, y):
    color = 2 + x // 40 % 6 if y < 140 else 8 + x // 10 % 2 if y < 170 else (x // 8 + y // 8) & 1
    if x % 40 == 0 or y % 20 == 0 or x < 2 or x >= 318 or y < 2 or y >= 198:
        color = 1
    if (x < 12 and y < 12) or (x >= 308 and y >= 188):
        color = 2
    if (x >= 308 and y < 12) or (x < 12 and y >= 188):
        color = 4
    return COLORS[color]

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--resolution', choices=['320x200','512x300'], default='320x200')
    args = parser.parse_args()
    high = args.resolution == '512x300'
    source_width, source_height = (512, 300) if high else (320, 200)
    width, height = (1024, 600) if high else (960, 720)
    y_offset = 60 if high else 0
    def sampled(x, y):
        sx = (x * source_width // width) * 320 // source_width
        sy = (y * source_height // height) * 200 // source_height
        return pixel(sx, sy)
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            value = sampled(x, y)
            rows.extend(((value >> 11) * 255 // 31, ((value >> 5) & 63) * 255 // 63, (value & 31) * 255 // 31))
    args.output.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    checksum = 2166136261
    for physical_x in range(width):
        for y in range(720):
            value = sampled(width - 1 - physical_x, y - y_offset) if y_offset <= y < y_offset + height else 0
            checksum = ((checksum ^ value) * 16777619) & 0xffffffff
    print(f'Expected viewport FNV32: {checksum:08x}')

if __name__ == '__main__':
    main()
