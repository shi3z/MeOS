#!/usr/bin/env python3
"""Run the Tab5 FreeType comparison, verify cleanup, optionally leave it open."""
import argparse
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--size", type=int, default=24)
    parser.add_argument("--leave-open", action="store_true")
    parser.add_argument("--trim-sd-cache", action="store_true",
                        help="release the SD asset cache first and verify lazy recreation")
    args = parser.parse_args()
    if not 12 <= args.size <= 48:
        parser.error("--size must be 12..48")

    link = serial.Serial(port=None, baudrate=115200, timeout=0.1, exclusive=True)
    link.dtr = link.rts = False
    link.port = args.port
    link.open()

    def read_until(token, timeout=45):
        data = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            data.extend(link.read(max(1, link.in_waiting)))
            if token in data:
                return data.decode(errors="replace").replace("\r", "")
        raise TimeoutError(f"waiting for {token!r}; tail={data[-800:]!r}")

    def command(text, timeout=45):
        link.reset_input_buffer()
        link.write(text.encode() + b"\r")
        data = bytearray()
        marker = text.encode()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            data.extend(link.read(max(1, link.in_waiting)))
            where = data.find(marker)
            if where >= 0 and data.find(b"> ", where + len(marker)) >= 0:
                return data.decode(errors="replace").replace("\r", "")
        raise TimeoutError(f"waiting for command {text!r}; tail={data[-800:]!r}")

    def free_psram(text):
        match = re.search(r"psram\s+total \d+ KB\s+used \d+ KB\s+free (\d+) KB", text)
        if not match:
            raise RuntimeError(f"cannot parse PSRAM: {text!r}")
        return int(match.group(1))

    def sd_cache_kib(text):
        match = re.search(r"SD asset\s+(\d+) KB cached", text)
        if not match:
            raise RuntimeError(f"cannot parse SD asset cache: {text!r}")
        return int(match.group(1))

    try:
        link.write(b"\x03\r")
        read_until(b"> ", 45)
        # Opening USB serial can reset Tab5.  The first prompt can precede the
        # queued Ctrl-C, so synchronize once more with an already-running shell.
        time.sleep(0.2)
        link.reset_input_buffer()
        link.write(b"\x03\r")
        read_until(b"> ", 10)
        for _ in range(60):
            if "imedict" not in command("ps"):
                break
            time.sleep(0.5)
        else:
            raise TimeoutError("waiting for background IME dictionary load")
        listing = command("ls /assets/fonts/BIZUDGothic-Regular.ttf")
        if "4667380" not in listing:
            raise RuntimeError(f"pinned TTF missing or wrong size: {listing!r}")
        before_text = command("mem")
        before = free_psram(before_text)
        cache_before = sd_cache_kib(before_text)
        if args.trim_sd_cache:
            if cache_before < 1100:
                raise RuntimeError(f"SD asset cache not resident before trim: {before_text!r}")
            receipt = command("mem trim")
            if "SD asset cache: trimmed" not in receipt:
                raise RuntimeError(f"SD asset trim receipt missing: {receipt!r}")
            trimmed_text = command("mem")
            trimmed = free_psram(trimmed_text)
            if sd_cache_kib(trimmed_text) != 0 or trimmed < before + 1100:
                raise RuntimeError(f"SD asset cache not reclaimed: before={before} KiB trimmed={trimmed} KiB")
        link.write(f"fontbench {args.size}\r".encode())
        result = read_until(b"FONTBENCH ", 60)
        receipt = result[result.rfind("FONTBENCH "):]
        if "\n" not in receipt:
            result += read_until(b"\n", 10)
        pattern = (r"FONTBENCH (\d+) px: load=(\d+) ms metric=(\d+) us first=(\d+) us warm=(\d+) us "
                   r"misses=(\d+) hits=(\d+) raster=(\d+) us font=(\d+) resident=(\d+) cache=(\d+) "
                   r"io=(\d+) reads=(\d+) metrics=(\d+)/(\d+)")
        match = re.search(pattern, result)
        if not match:
            raise RuntimeError(f"benchmark receipt missing: {result!r}")
        values = list(map(int, match.groups()))
        if (values[0] != args.size or values[5] == 0 or values[6] == 0 or
                values[8] != 4667380 or values[9] != 0 or
                values[11] == 0 or values[12] == 0 or
                values[13] == 0 or values[14] == 0):
            raise RuntimeError(f"invalid benchmark counters: {values}")
        time.sleep(2)
        link.reset_input_buffer()
        link.write(b"q")
        read_until(b"> ", 20)
        after_text = command("mem")
        after = free_psram(after_text)
        if abs(after - before) > 32:
            raise RuntimeError(f"PSRAM not reclaimed: before={before} KiB after={after} KiB")
        if args.trim_sd_cache:
            if sd_cache_kib(after_text) != cache_before:
                raise RuntimeError(f"SD asset cache not recreated: {after_text!r}")
        print("FREETYPE DEVICE PASS: "
              f"{values[0]}px load {values[1]} ms; metrics {values[2]} us; "
              f"first {values[3]} us; warm {values[4]} us; raster {values[7]} us; "
              f"miss/hit {values[5]}/{values[6]}; resident {values[9] // 1024} KiB; "
              f"cache {values[10] // 1024} KiB; I/O {values[11] // 1024} KiB/{values[12]} reads; "
              f"metrics miss/hit {values[13]}/{values[14]}; "
              f"PSRAM restored to {after} KiB"
              f"{'; SD asset trim/recreate PASS' if args.trim_sd_cache else ''}")
        if args.leave_open:
            link.write(f"fontbench {args.size}\r".encode())
            read_until(b"FONTBENCH ", 60)
            print("Comparison screen left open; press Q on Tab5 to return.")
    finally:
        link.close()


if __name__ == "__main__":
    main()

