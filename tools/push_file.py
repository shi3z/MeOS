#!/usr/bin/env python3
"""Upload one file through the bounded MeOS putraw developer protocol."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination")
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    size = args.source.stat().st_size
    if not 0 < size <= 64 * 1024 * 1024:
        parser.error("source must be between 1 byte and 64 MiB")
    crc = 0
    with args.source.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            crc = zlib.crc32(block, crc)
    import serial
    awake = connection = None
    try:
        if sys.platform == "darwin":
            awake = subprocess.Popen(["caffeinate", "-is", "-w", str(os.getpid())])
        connection = serial.Serial()
        connection.port = args.port
        connection.baudrate = 115200
        connection.timeout = 0.1
        connection.write_timeout = 30
        connection.dtr = connection.rts = False
        if os.name == "posix":
            connection.exclusive = True
        connection.open()
        pending = bytearray()

        def until(token, timeout=120):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                failure = pending.find(b"PUTFAIL")
                if failure >= 0:
                    raise RuntimeError(pending[failure:].split(b"\n", 1)[0].decode(errors="replace"))
                at = pending.find(token)
                if at >= 0:
                    del pending[:at + len(token)]
                    return
                if len(pending) > 65536:
                    del pending[:-32768]
                pending.extend(connection.read(max(1, connection.in_waiting)))
            raise TimeoutError(f"waiting for {token!r}")

        connection.write(b"\x03\r")
        until(b"> ", 30)
        pending.clear()
        connection.write(f"putraw {args.destination} {size} {crc:08x}\r".encode())
        until(f"PUTREADY {size} 4096\r\n".encode())
        start = time.monotonic()
        done = 0
        with args.source.open("rb") as source:
            while done < size:
                block = source.read(min(4096, size - done))
                if not block:
                    raise RuntimeError("source changed during upload")
                connection.write(block)
                done += len(block)
                until(f"PUTACK {done}\r\n".encode())
                if done == size or done % (1024 * 1024) < 4096:
                    print(f"{done}/{size} bytes ({100 * done / size:.1f}%)", flush=True)
        until(f"PUTDONE {size} {crc:08x}\r\n".encode())
        print(f"Transferred and CRC-verified in {time.monotonic() - start:.1f}s")
    finally:
        if connection is not None:
            connection.close()
        if awake is not None:
            awake.terminate()
            awake.wait()


if __name__ == "__main__":
    main()
