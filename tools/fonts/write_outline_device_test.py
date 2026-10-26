#!/usr/bin/env python3
"""Exercise outline Write sizing/layout on Tab5 and verify PSRAM cleanup."""
import argparse
import re
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--file", default="/home/note.txt")
    parser.add_argument("--leave-open", action="store_true")
    args = parser.parse_args()
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

    try:
        link.write(b"\x03\r")
        read_until(b"> ", 45)
        time.sleep(0.2)
        link.reset_input_buffer()
        link.write(b"\x03\r")
        read_until(b"> ", 10)
        # The shell becomes ready before the asynchronous 5 MiB IME dictionary
        # finishes loading.  Do not count that intentional resident allocation
        # as a Write leak on boards that reset when USB serial is opened.
        for _ in range(60):
            if "imedict" not in command("ps"):
                break
            time.sleep(0.5)
        else:
            raise TimeoutError("waiting for background IME dictionary load")
        before = free_psram(command("mem"))
        link.reset_input_buffer()
        link.write(f"write {args.file}\r".encode())
        opened = read_until(b"write: BIZ UD 24px outline", 30)
        if "otfont: opened 4667380-byte OpenType face" not in opened:
            raise RuntimeError(f"outline open receipt missing: {opened!r}")
        for keys, receipt in ((b"\x1bOS", b"write: outline 28px"),
                              (b"\x1bOS", b"write: outline 32px"),
                              (b"\x1bOR", b"write: outline 28px"),
                              (b"\x14", b"write: layout horizontal"),
                              (b"\x14", b"write: layout vertical")):
            link.write(keys)
            read_until(receipt, 20)
        if args.leave_open:
            print("WRITE OUTLINE PASS: 24 -> 28 -> 32 -> 28px; vertical screen left open")
            return
        link.reset_input_buffer()
        link.write(b"\x11")
        read_until(b"> ", 20)
        after = free_psram(command("mem"))
        if abs(after-before)>32:
            raise RuntimeError(f"PSRAM not reclaimed: before={before} KiB after={after} KiB")
        print(f"WRITE OUTLINE PASS: 24 -> 28 -> 32 -> 28px; vertical/horizontal; PSRAM restored to {after} KiB")
    finally:
        link.close()


if __name__ == "__main__":
    main()
