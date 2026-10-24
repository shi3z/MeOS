#!/usr/bin/env python3
"""Verify the SD BIZ UD atlas loads on a connected Tab5 and is released."""
import argparse
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    args = parser.parse_args()
    link = serial.Serial()
    link.port = args.port
    link.baudrate = 115200
    link.timeout = 0.1
    link.write_timeout = 10
    link.dtr = link.rts = False
    link.exclusive = True
    link.open()

    def read_until(token, timeout=20):
        data = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            data.extend(link.read(max(1, link.in_waiting)))
            if token in data:
                return data.decode(errors="replace").replace("\r", "")
        raise TimeoutError(f"waiting for {token!r}; tail={data[-500:]!r}")

    def command(text):
        time.sleep(0.1)
        link.reset_input_buffer()
        link.write(text.encode() + b"\r")
        return read_until(b"\n> ")

    try:
        link.write(b"\x03\r")
        read_until(b"> ")
        listing = command("ls /assets/fonts/bizud24.m5f")
        if "2432659" not in listing:
            raise RuntimeError(f"font asset missing or wrong size: {listing!r}")
        before = command("mem")
        match = re.search(r"psram\s+total .* free (\d+) KB", before)
        if not match:
            raise RuntimeError(f"cannot parse baseline memory: {before!r}")
        free_before = int(match.group(1))
        link.write(b"write /home/bizud-device-check.txt\r")
        time.sleep(5)
        link.write(b"\x11")  # Ctrl-Q
        result = read_until(b"\n> ", 30)
        if "write:" in result:
            raise RuntimeError(f"writing editor failed: {result!r}")
        loaded = re.search(r"bizud: loaded 11741 glyphs, 2432627 bytes from SD in (\d+) ms", result)
        if not loaded:
            raise RuntimeError(f"font load evidence missing: {result!r}")
        after = command("mem")
        match = re.search(r"psram\s+total .* free (\d+) KB", after)
        if not match or abs(int(match.group(1)) - free_before) > 16:
            raise RuntimeError(f"font/frame memory not reclaimed: before={before!r} after={after!r}")
        print(f"BIZ UD DEVICE PASS: SD atlas loaded in {loaded.group(1)} ms; PSRAM returned to {match.group(1)} KiB")
    finally:
        link.close()


if __name__ == "__main__":
    main()
