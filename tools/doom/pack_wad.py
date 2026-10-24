"""Validate IWAD bounds, then produce a local zlib payload (never commit game data)."""
import pathlib
import struct
import sys
import zlib

source, dest = map(pathlib.Path, sys.argv[1:3])
data = source.read_bytes()
if not 12 <= len(data) <= 12 * 1024 * 1024 or data[:4] != b'IWAD':
    raise SystemExit('expected an IWAD of at most 12 MiB')
n, off = struct.unpack_from('<II', data, 4)
if not 0 < n <= 32768 or off > len(data) or n * 16 > len(data) - off:
    raise SystemExit('invalid WAD directory')
for i in range(n):
    start, size = struct.unpack_from('<II', data, off + i * 16)
    if start > len(data) or size > len(data) - start:
        raise SystemExit('WAD lump outside file')
packed = zlib.compress(data, 9)
dest.write_bytes(packed)
print(f'Doom IWAD: {len(data)} bytes -> {len(packed)} compressed bytes (local build only)')
