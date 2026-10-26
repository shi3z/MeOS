#!/usr/bin/env python3
"""Transfer a separately supplied PAK to a new SD file; never overwrites a PAK."""
import argparse
from pathlib import Path
import time
import zlib
import serial
from headless import validate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('pak', type=Path)
    parser.add_argument('--prepare-sd', action='store_true', help='mount SD read/write and create id1 (no format)')
    parser.add_argument('--readonly-after', action='store_true', help='unmount and remount SD read-only after verified upload')
    args = parser.parse_args()
    validate(args.pak)
    data = args.pak.read_bytes()
    if not 12 <= len(data) <= 64 * 1024 * 1024:
        parser.error('PAK exceeds receiver bounds')
    crc = zlib.crc32(data) & 0xffffffff
    with serial.Serial() as device:
        device.port, device.baudrate = args.port, 115200
        device.timeout, device.write_timeout = 0.2, 30
        device.dtr = device.rts = False
        device.open()
        pending = bytearray()

        def wait(token, timeout=30):
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                pending.extend(device.read(max(1, device.in_waiting)))
                if b'QFAIL' in pending:
                    raise RuntimeError(bytes(pending).decode(errors='replace'))
                at = pending.find(token)
                if at >= 0:
                    del pending[:at + len(token)]
                    return
                if len(pending) > 65536:
                    del pending[:-32768]
            raise TimeoutError('device did not respond: ' + repr(token))

        def command(text):
            device.write((text + '\r').encode())
            wait(b'> ')

        # Opening USB-serial may reset this board; drain boot before commands.
        time.sleep(3)
        device.write(b'\r')
        wait(b'> ')
        if args.prepare_sd:
            command('sd mount rw')
            command('mkdir /sd/id1')
        device.write(f'quake receive {len(data)} {crc:08x}\r'.encode())
        wait(f'QREADY {len(data)} 4096\r\n'.encode())
        start = time.monotonic()
        for offset in range(0, len(data), 4096):
            block = data[offset:offset + 4096]
            device.write(block)
            done = offset + len(block)
            wait(f'QACK {done}\r\n'.encode())
            if done % (256 * 1024) == 0:
                print(f'{done}/{len(data)} bytes', flush=True)
        wait(f'QDONE {len(data)} {crc:08x}\r\n'.encode())
        wait(b'> ')
        print(f'CRC verified: {len(data)} bytes, {crc:08x}, {time.monotonic()-start:.1f}s')
        if args.readonly_after:
            command('sd unmount')
            command('sd mount ro')

if __name__ == '__main__':
    main()
