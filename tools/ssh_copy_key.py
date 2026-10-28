#!/usr/bin/env python3
"""
Copy an OpenSSH Ed25519 private key (default: ~/.ssh/id_ed25519) from the host
machine into M5Stack Tab5's persistent SPI Flash keystore (0x3F0000) & /home/id_ed25519
over serial, without embedding any secret into the firmware or repository.
"""
import argparse
import base64
import os
import struct
import sys
import time
import serial


def extract_openssh_ed25519_seed(path: str) -> bytes:
    with open(path, "r", encoding="utf-8") as f:
        lines = [
            ln.strip()
            for ln in f
            if ln.strip() and not ln.startswith("-----")
        ]
    raw = base64.b64decode("".join(lines))
    if not raw.startswith(b"openssh-key-v1\x00"):
        raise ValueError(f"{path}: not an OpenSSH v1 private key")

    # Locate [0x00000020][pub 32B][0x00000040][seed 32B][pub 32B]
    for i in range(15, len(raw) - 104 + 1):
        l1, = struct.unpack(">I", raw[i : i + 4])
        l2, = struct.unpack(">I", raw[i + 36 : i + 40])
        if l1 == 32 and l2 == 64:
            pub1 = raw[i + 4 : i + 36]
            seed = raw[i + 40 : i + 72]
            pub2 = raw[i + 72 : i + 104]
            if pub1 == pub2:
                return seed
    raise ValueError(f"{path}: could not find unencrypted ssh-ed25519 key")


def main() -> int:
    ap = argparse.ArgumentParser(description="Copy ~/.ssh/id_ed25519 to M5Stack Tab5 persistent Flash")
    ap.add_argument("-p", "--port", default="/dev/cu.usbmodem211301", help="Serial port of Tab5")
    ap.add_argument("-i", "--identity", default=os.path.expanduser("~/.ssh/id_ed25519"), help="Path to id_ed25519")
    args = ap.parse_args()

    seed = extract_openssh_ed25519_seed(args.identity)
    seed_hex = seed.hex()

    with serial.Serial(args.port, 115200, timeout=0.2) as ser:
        ser.write(b"\r\n")
        time.sleep(0.3)
        ser.read(4096)
        cmd = f"ssh key set {seed_hex}\r\n".encode("ascii")
        ser.write(cmd)
        t0 = time.time()
        out = ""
        while time.time() - t0 < 5.0:
            d = ser.read(4096)
            if d:
                s = d.decode("utf-8", "replace")
                out += s
                print(s, end="", flush=True)
                if "Public key:" in out and "> " in out.split("Public key:")[-1]:
                    break
    return 0


if __name__ == "__main__":
    sys.exit(main())
