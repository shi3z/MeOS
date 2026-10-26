#!/usr/bin/env python3
"""Measure cold Write readiness with a many-glyph document on Tab5."""
import argparse
from pathlib import Path
import re
import time
import zlib

import serial


def benchmark_text():
    generated = Path(__file__).resolve().parents[2] / "drivers/lcd/bizud24_data.inc"
    ordinary = generated.read_text().split("static const biz_glyph vertical_glyphs", 1)[0]
    codepoints = [int(value) for value in re.findall(r"^\{(\d+),", ordinary, re.MULTILINE)]
    codepoints = [cp for cp in codepoints if 0x4E00 <= cp <= 0x9FFF][:900]
    if len(codepoints) != 900:
        raise RuntimeError(f"expected 900 CJK glyphs, found {len(codepoints)}")
    lines = ["".join(chr(cp) for cp in codepoints[n:n + 50])
             for n in range(0, len(codepoints), 50)]
    return (("\n".join(lines) + "\n") * 4).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--file", default="/home/font-layout-bench.txt")
    args = parser.parse_args()
    payload = benchmark_text()
    checksum = zlib.crc32(payload)
    link = serial.Serial(port=None, baudrate=115200, timeout=0.1, exclusive=True)
    link.dtr = link.rts = False
    link.port = args.port
    link.open()
    pending = bytearray()

    def read_until(token, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            pending.extend(link.read(max(1, link.in_waiting)))
            at = pending.find(token)
            if at >= 0:
                result = bytes(pending[:at + len(token)])
                del pending[:at + len(token)]
                return result
            if len(pending) > 65536:
                del pending[:-32768]
        raise TimeoutError(f"waiting for {token!r}; tail={pending[-800:]!r}")

    def command(text, timeout=45):
        pending.clear()
        link.write(text.encode() + b"\r")
        read_until(text.encode(), timeout)
        return read_until(b"> ", timeout)

    try:
        link.write(b"\x03\r")
        read_until(b"> ", 45)
        time.sleep(0.2)
        pending.clear()
        link.write(b"\x03\r")
        read_until(b"> ", 10)

        pending.clear()
        link.write(f"putraw {args.file} {len(payload)} {checksum:08x}\r".encode())
        read_until(f"PUTREADY {len(payload)} 4096\r\n".encode())
        sent = 0
        while sent < len(payload):
            block = payload[sent:sent + 4096]
            link.write(block)
            sent += len(block)
            read_until(f"PUTACK {sent}\r\n".encode(), 30)
        read_until(f"PUTDONE {len(payload)} {checksum:08x}\r\n".encode(), 30)
        read_until(b"> ", 10)
        command("sdroot io-reset")

        pending.clear()
        started = time.monotonic()
        # Queue F3 after the shell command.  Write handles it only after its
        # initial full layout and paint, then reports the 24 -> 20px change.
        link.write(f"write {args.file}\r".encode() + b"\x1bOR")
        receipt = read_until(b"write: outline 20px", 120)
        ready_ms = round((time.monotonic() - started) * 1000)
        if b"write: BIZ UD 24px outline" not in receipt:
            raise RuntimeError(f"outline receipt missing: {receipt[-800:]!r}")
        if b"resident 4557 KiB/" not in receipt:
            raise RuntimeError(f"resident face receipt missing: {receipt[-800:]!r}")
        link.write(b"\x11")
        read_until(b"> ", 60)
        io = command("sdroot")
        match = re.search(rb"SDROOT IO reads=(\d+) sectors=(\d+) full4k=(\d+) "
                          rb"adjacent=\+(\d+)/-(\d+) other=(\d+)", io)
        if not match:
            raise RuntimeError(f"SDROOT I/O receipt missing: {io[-800:]!r}")
        reads, sectors, full4k, forward, backward, other = map(int, match.groups())
        cache = re.search(rb"SDROOT CACHE hit=(\d+) miss=(\d+) physical=(\d+)/(\d+) sectors", io)
        if not cache:
            raise RuntimeError(f"SDROOT cache receipt missing: {io[-800:]!r}")
        hits, misses, physical, physical_sectors = map(int, cache.groups())
        sectors_cached = re.search(rb"SDROOT SECTOR hit=(\d+) miss=(\d+) slots=(\d+)", io)
        if not sectors_cached:
            raise RuntimeError(f"SDROOT sector-cache receipt missing: {io[-800:]!r}")
        sector_hits, sector_misses, sector_slots = map(int, sectors_cached.groups())
        command(f"rm {args.file}")
        print(f"WRITE LAYOUT DEVICE PASS: {len(payload)} bytes, 900 distinct CJK; "
              f"adaptive resident cold ready {ready_ms} ms; "
              f"SD reads {reads}/{sectors} sectors, full4k {full4k}, "
              f"adjacent +{forward}/-{backward}, other {other}; cache {hits}/{misses}, "
              f"sector-cache {sector_hits}/{sector_misses}/{sector_slots}, "
              f"physical {physical}/{physical_sectors} sectors; temporary file removed")
    finally:
        link.close()


if __name__ == "__main__":
    main()
