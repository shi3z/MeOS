#!/usr/bin/env python3
"""Pushes a .wasm app to the running shell, runs it and sends keys, all in one serial session (opening the port resets the board).
  tools/wasm_try.py PORT apps/wasm/snake.wasm [--keys "down,left,up"] [--secs 5]"""
import base64, sys, time, zlib, serial
port, src = sys.argv[1:3]
keys = sys.argv[sys.argv.index("--keys") + 1].split(",") if "--keys" in sys.argv else []
secs = float(sys.argv[sys.argv.index("--secs") + 1]) if "--secs" in sys.argv else 4
K = {"up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D"}
dest = "/apps/" + src.split("/")[-1]
s = serial.Serial(port, 115200, timeout=0.2)
def drain(t=0.5):
    end = time.time() + t; out = b""
    while time.time() < end: out += s.read(512)
    return out.decode("utf-8", "replace")
def wait_prompt(timeout=20):
    buf = ""; end = time.time() + timeout
    while time.time() < end:
        buf += drain(0.3)
        if buf.rstrip().endswith(">"): break
    return buf
time.sleep(0.5)
print("boot:", wait_prompt(25)[-200:].strip()[-80:])
time.sleep(8)                      # wifi / dictionary start-up chatter
drain(1)
data = open(src, "rb").read()
want = "put: wrote %d bytes, crc32 %08x" % (len(data), zlib.crc32(data) & 0xFFFFFFFF)
b64 = base64.b64encode(data).decode()
for attempt in range(4):                         # the serial line drops bytes while the board is busy: verify, retry
    s.write(b"\x03\r"); wait_prompt(3)
    s.write(("put %s\r" % dest).encode()); time.sleep(0.3); drain(0.3)
    for i in range(0, len(b64), 76):
        s.write((b64[i:i+76] + "\r").encode()); time.sleep(0.03)
    s.write(b".\r"); reply = wait_prompt(10)
    if want in reply: break
    print("transfer retry:", reply.strip()[-60:])
else:
    sys.exit("transfer failed")
s.write(("run %s\r" % dest).encode()); time.sleep(1.5); print(drain(0.5))
for k in keys:
    s.write(K.get(k, k.encode())); time.sleep(0.8)
time.sleep(secs)
print("during:", drain(0.5))
s.write(b"q"); time.sleep(1.5)
s.write(b"\rapps\r"); time.sleep(1); print(drain(1))
