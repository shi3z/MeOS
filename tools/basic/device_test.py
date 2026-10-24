#!/usr/bin/env python3
"""Tab5 serial qualification for BASIC. Requires a freshly built/flashed MeOS.

Run with the ESP-IDF Python environment (pyserial), e.g.:
  python tools/basic/device_test.py /dev/cu.usbmodem1101 --reset --output build-tab5/basic-device.json
--reset deliberately reboots the board and loses RAM state. The test starts and
stops its own CPU hog; it does not touch existing user files or persistent data.
LCD appearance, physical keyboard input and two concurrent FP workloads require
separate qualification. Graphics here is checked via program completion/errors.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import time

import serial

ROOT = Path(__file__).resolve().parents[2]
SHELL = b"\n> "
BASIC = b"Ready> "


class TestFailure(Exception):
    pass


class Driver:
    def __init__(self, port):
        self.serial = serial.Serial(port, 115200, timeout=0.05, exclusive=True)
        self.checks = []
        self.transcript = []
        self.in_basic = False
        self.hog = None

    def read(self, prompt, timeout=12):
        data = bytearray()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            data.extend(self.serial.read(4096))
            if data.replace(b"\r", b"").endswith(prompt):
                text = data.decode("utf-8", errors="replace").replace("\r", "")
                self.transcript.append(text)
                return text
        text = data.decode("utf-8", errors="replace").replace("\r", "")
        self.transcript.append(text)
        raise TestFailure(f"timeout waiting for {prompt!r}; tail={text[-500:]!r}")

    def command(self, command, prompt=BASIC, timeout=12):
        self.serial.write(command.encode("utf-8") + b"\r")
        return self.read(prompt, timeout)

    def check(self, name, response, pattern):
        passed = re.search(pattern, response, re.MULTILINE) is not None
        self.checks.append({"name": name, "passed": passed})
        print(("PASS " if passed else "FAIL ") + name, flush=True)
        if not passed:
            raise TestFailure(f"{name}: expected {pattern!r}; response={response!r}")

    def program(self, lines):
        self.command("NEW")
        for line in lines:
            response = self.command(line)
            if "Error" in response:
                raise TestFailure(f"program entry failed: {response!r}")

    def cleanup(self):
        if self.in_basic:
            self.serial.write(b"\x03")
            try:
                self.read(BASIC, 3)
                self.command("BYE", SHELL, 3)
                self.in_basic = False
            except TestFailure:
                # Do not reboot or overwrite unrelated state on recovery failure.
                return False
        if self.hog is not None:
            try:
                response = self.command(f"stop {self.hog}", SHELL, 3)
                if f"stopped app {self.hog}" not in response:
                    return False
                self.hog = None
            except TestFailure:
                return False
        return True


def qualify(d):
    boot = d.read(SHELL, 25)
    d.check("Tab5 boot identity", boot, r"Board: M5Stack Tab5")
    d.check("calculator decimal division", d.command('calc "12/5"', SHELL), r"^2\.4$")
    d.check("calculator scientific function", d.command('calc "sin(30)"', SHELL), r"^0\.5$")
    started = d.command("run hog", SHELL)
    match = re.search(r"started hog as app (\d+)", started)
    if not match:
        raise TestFailure("cannot start CPU stress app")
    d.hog = int(match.group(1))
    d.check("CPU hog coexists with shell", d.command("ps", SHELL), r"\bhog\b")
    d.in_basic = True
    d.command("basic")
    d.check("floating division / scientific literal", d.command("PRINT 12/5,1.25e-3*2"), r"^2\.4 0\.0025$")
    d.command("DIM A(1)")
    d.command("A(0)=.125")
    d.command("A(1)=A(0)*3")
    d.check("floating array values", d.command("PRINT A(0),A(1)"), r"^0\.125 0\.375$")
    d.check("fractional subscript rejected", d.command("PRINT A(.5)"), r"integer argument required")
    d.serial.write(b"INPUT N\r")
    d.read(b"? ")
    d.serial.write(b"1e101\r")
    d.check("out-of-range INPUT retries", d.read(b"? "), r"\?Redo: numeric range exceeded")
    d.serial.write(b"-1.25e-3\r")
    d.read(BASIC)
    d.check("floating INPUT value", d.command("PRINT N"), r"^-0\.00125$")
    d.serial.write(b"INPUT N\r")
    d.read(b"? ")
    d.serial.write(b"\x03")
    d.check("INPUT Ctrl-C returns to BASIC", d.read(BASIC), r"^Break$")
    d.check("INPUT cancel preserves value", d.command("PRINT N"), r"^-0\.00125$")
    d.program(["10 A=0", "20 FOR I=1 TO 1000", "30 A=A+.125", "40 NEXT I", "50 PRINT A", "60 END"])
    for i in range(3):
        d.check(f"numeric loop under CPU stress #{i+1}", d.command("RUN", timeout=25), r"^125$")
    d.program(["10 SCREEN 1", "20 CLS 0", "30 LINE 20,20,400,200,2016", '40 TEXT 20,250,"BASIC device check",65535', "45 PRINT 777", "50 WAIT 100", "60 PRINT 12345", "70 END"])
    response = d.command("RUN")
    if "Error" in response:
        raise TestFailure(f"graphics command failed: {response!r}")
    d.check("graphics normal completion / serial return", response, r"^12345$")
    d.command("50 WAIT 10000")
    d.serial.write(b"RUN\r")
    d.read(b"777\n")
    time.sleep(0.05)
    d.serial.write(b"\x03")
    d.check("graphics WAIT Ctrl-C / serial return", d.read(BASIC), r"^Break$")
    d.check("BASIC remains usable after graphics cancel", d.command("PRINT 7"), r"^7$")
    d.command("30 PSET .5,0,1")
    d.check("graphics error / serial return", d.command("RUN"), r"integer argument required")
    d.check("BASIC remains usable after graphics error", d.command("PRINT 8"), r"^8$")
    d.command("BYE", SHELL)
    d.in_basic = False
    d.check("shell usable after BASIC", d.command("uptime", SHELL), r"\d+")
    stopped = d.command(f"stop {d.hog}", SHELL)
    d.check("CPU stress app stopped", stopped, rf"stopped app {d.hog}")
    d.hog = None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--reset", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    record = {"port": args.port, "status": "incomplete", "checks": [],
              "limits": ["serial checks do not qualify physical LCD appearance or keyboard", "CPU hog is integer load, not a second floating-point workload"]}
    firmware = ROOT / "build-tab5/m5os.bin"
    if firmware.exists():
        record["host_firmware_sha256"] = hashlib.sha256(firmware.read_bytes()).hexdigest()
    driver = None
    try:
        driver = Driver(args.port)
        if args.reset:
            sys.path.insert(0, str(ROOT / "tools"))
            from board_reset import reset_board
            reset_board(driver.serial)
        qualify(driver)
        record["status"] = "passed"
    except (TestFailure, serial.SerialException, OSError) as exc:
        record["status"] = "failed"
        record["error"] = str(exc)
        print(f"FAIL {exc}", file=sys.stderr, flush=True)
    finally:
        if driver:
            record["cleanup_ok"] = driver.cleanup()
            record["checks"] = driver.checks
            record["transcript"] = driver.transcript
            driver.serial.close()
            if not record["cleanup_ok"]:
                record["status"] = "failed"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n")
    return 0 if record["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
