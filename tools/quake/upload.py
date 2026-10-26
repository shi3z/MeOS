#!/usr/bin/env python3
"""Upload separately supplied PAK0 through the bounded MeOS developer protocol."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pak", type=Path)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    size = args.pak.stat().st_size
    if not 12 <= size <= 64 * 1024 * 1024:
        parser.error("PAK must be between 12 bytes and 64 MiB")
    crc = 0
    with args.pak.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            crc = zlib.crc32(block, crc)
    import serial  # Install pyserial, or use the ESP-IDF Python environment.
    awake = None
    connection = None
    try:
        if sys.platform == "darwin":
            # Scoped to this process; does not change system power settings.
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

        def until(token, timeout):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                failure = pending.find(b"QFAIL")
                if failure >= 0:
                    raise RuntimeError(pending[failure:].split(b"\n", 1)[0].decode(errors="replace"))
                at = pending.find(token)
                if at >= 0:
                    result = bytes(pending[:at + len(token)])
                    del pending[:at + len(token)]
                    return result
                if len(pending) > 65536:
                    del pending[:-32768]
                pending.extend(connection.read(max(1, connection.in_waiting)))
            raise TimeoutError(f"waiting for {token!r}")

        print("Waiting for the MeOS shell; quit any running app first.", flush=True)
        connection.write(b"\r")
        until(b"\n> ", 120)
        pending.clear()
        connection.write(f"quake receive {size} {crc:08x}\r".encode())
        until(f"QREADY {size} 4096\r\n".encode(), 120)
        start = time.monotonic()
        done = 0
        with args.pak.open("rb") as source:
            while done < size:
                block = source.read(min(4096, size - done))
                if not block:
                    raise RuntimeError("source changed during upload")
                connection.write(block)
                previous = done
                done += len(block)
                until(f"QACK {done}\r\n".encode(), 120)
                if done // 1048576 != previous // 1048576:
                    print(f"{done}/{size} bytes ({100 * done / size:.1f}%)", flush=True)
        until(f"QDONE {size} {crc:08x}\r\n".encode(), 120)
        print(f"Transferred in {time.monotonic() - start:.1f}s; verifying stored data…", flush=True)
        until(b"> ", 120)
        pending.clear()
        connection.write(b"quake verify\r")
        result = until(b"\n> ", 1800)
        match = re.search(rb"QVERIFY (\d+) ([0-9a-f]{8})\r?\n", result)
        if not match or (int(match[1]), int(match[2], 16)) != (size, crc):
            raise RuntimeError("stored-data verification failed")
        print(f"Stored PAK verified: {size} bytes, CRC32 {crc:08x}")
    finally:
        if connection is not None:
            connection.close()
        if awake is not None:
            awake.terminate()
            awake.wait()


if __name__ == "__main__":
    main()
