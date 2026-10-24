#!/usr/bin/env python3
"""Check the entire decoded BIZ UD atlas against its pre-integration pixels."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = "c7d570a208bb4ad4115c58f2ea63013c350431ee75e841c42044dcde1e5dd5c4"


def read_atlas(path):
    if path.suffix == ".m5f":
        raw = path.read_bytes()
        magic, version, count, index_size, data_size, index_crc, data_crc = struct.unpack("<8sIIIIII", raw[:32])
        assert magic == b"M5BIZ24\0" and version == 1 and index_size == count * 16
        index, data = raw[32:32 + index_size], raw[32 + index_size:]
        assert len(data) == data_size and zlib.crc32(index) & 0xffffffff == index_crc
        assert zlib.crc32(data) & 0xffffffff == data_crc
        return [struct.unpack("<IIHbbBBBx", index[n:n + 16]) for n in range(0, len(index), 16)], data
    index, encoded = path.read_text().split("static const uint8_t coverage[] = {")
    data = bytes(map(int, re.findall(r"\d+", encoded)))
    entries = [tuple(map(int, row.split(","))) for row in re.findall(r"\{([^{}]+)\}", index)]
    return entries, data


def decoded_digest(path):
    entries, data = read_atlas(path)
    digest = hashlib.sha256()
    previous = -1
    count = 0
    for cp, offset, flags, x, y, width, height, advance in entries:
        assert previous < cp and width <= 64 and height <= 64
        previous = cp
        length = flags & 32767
        assert offset + length <= len(data)
        raw = data[offset:offset + length]
        size = (width * height + 1) // 2
        packed = bytearray()
        pos = 0
        if flags & 32768:
            while len(packed) < size:
                group = raw[pos]
                pos += 1
                for bit in range(8):
                    if len(packed) >= size:
                        break
                    if group & (1 << bit):
                        token = (raw[pos] << 8) | raw[pos + 1]
                        pos += 2
                        back, run = token >> 4, (token & 15) + 3
                        assert 0 < back <= len(packed) and len(packed) + run <= size
                        for _ in range(run):
                            packed.append(packed[-back])
                    else:
                        packed.append(raw[pos])
                        pos += 1
            assert pos == length
        else:
            packed = raw
        assert len(packed) == size
        digest.update(struct.pack("<IbbBBB", cp, x, y, width, height, advance))
        digest.update(packed)
        count += 1
    assert count == 11741
    return digest.hexdigest(), count, len(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=Path)
    args = parser.parse_args()
    paths = [args.atlas] if args.atlas else [ROOT / "drivers/lcd/bizud24_data.inc", ROOT / "assets/fonts/bizud24.m5f"]
    results = [decoded_digest(path) for path in paths]
    for path, (checksum, count, size) in zip(paths, results):
        assert checksum == EXPECTED, f"{path}: glyph metrics or decoded pixels changed: {checksum}"
        print(f"BIZ UD {path.name}: {count} glyphs, all metrics/pixels unchanged; {size} encoded bytes PASS")
    assert len(set(results)) == 1, "embedded and SD atlases differ"
