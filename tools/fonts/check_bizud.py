#!/usr/bin/env python3
"""Check ordinary and OpenType-vertical BIZ UD atlas pixels."""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = "c7d570a208bb4ad4115c58f2ea63013c350431ee75e841c42044dcde1e5dd5c4"
EXPECTED_VERTICAL = "2ace1258c215fa83df45af8310bfa6d2ebd9afda96e13e36f50f01f19762de34"


def read_atlas(path):
    if path.suffix == ".m5f":
        raw = path.read_bytes()
        magic, version, count, index_size, data_size, index_crc, data_crc = struct.unpack("<8sIIIIII", raw[:32])
        assert magic == b"M5BIZ24\0" and version == 1 and index_size == count * 16
        index, payload = raw[32:32 + index_size], raw[32 + index_size:]
        assert len(payload) == data_size and zlib.crc32(index) & 0xffffffff == index_crc
        assert zlib.crc32(payload) & 0xffffffff == data_crc
        magic2, vertical_count, vertical_offset = struct.unpack("<8sII", payload[-16:])
        assert magic2 == b"M5VRT2\0\0" and vertical_offset % 4 == 0
        assert vertical_offset + vertical_count * 16 == len(payload) - 16
        vertical_index = payload[vertical_offset:-16]
        ordinary = [struct.unpack("<IIHbbBBBx", index[n:n + 16]) for n in range(0, len(index), 16)]
        vertical = [struct.unpack("<IIHbbBBBx", vertical_index[n:n + 16])
                    for n in range(0, len(vertical_index), 16)]
        return ordinary, vertical, payload[:vertical_offset]
    index, encoded = path.read_text().split("static const uint8_t coverage[] = {")
    data = bytes(map(int, re.findall(r"\d+", encoded)))
    ordinary, vertical = index.split("static const biz_glyph vertical_glyphs[] = {")
    ordinary = ordinary.split("static const biz_glyph glyphs[] = {", 1)[1]
    parse = lambda text: [tuple(map(int, row.split(","))) for row in re.findall(r"\{([^{}]+)\}", text)]
    return parse(ordinary), parse(vertical), data


def decoded_digest(entries, data):
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
    return digest.hexdigest(), count


def checked_atlas(path):
    ordinary, vertical, data = read_atlas(path)
    normal_digest, normal_count = decoded_digest(ordinary, data)
    vertical_digest, vertical_count = decoded_digest(vertical, data)
    assert normal_count == 11741 and normal_digest == EXPECTED, \
        f"{path}: ordinary glyph metrics or pixels changed: {normal_digest}"
    assert vertical_count == 350 and vertical_digest == EXPECTED_VERTICAL, \
        f"{path}: vertical glyph metrics or pixels changed: {vertical_digest}"
    return normal_digest, vertical_digest, normal_count, vertical_count, len(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=Path)
    args = parser.parse_args()
    paths = [args.atlas] if args.atlas else [ROOT / "drivers/lcd/bizud24_data.inc", ROOT / "assets/fonts/bizud24.m5f"]
    results = [checked_atlas(path) for path in paths]
    for path, (_, _, count, vertical_count, size) in zip(paths, results):
        print(f"BIZ UD {path.name}: {count} ordinary + {vertical_count} vertical glyphs; "
              f"all metrics/pixels unchanged; {size} encoded bytes PASS")
    assert len(set(results)) == 1, "embedded and SD atlases differ"
