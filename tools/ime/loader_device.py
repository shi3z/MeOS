#!/usr/bin/env python3
"""Measure background dictionary startup and serial-shell response on a Tab5.

Each cycle resets the board, discarding unsaved RAM. The tool reads the existing
SD dictionary, sends echo probes, and never requests media writes or uploads.
Host timings include serial delivery; BOOT timings come from the board itself.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys
import time


class ReceiptLog:
    def __init__(self, output):
        self.output = output
        self.data = bytearray()
        self.started = time.monotonic()
        self.loaded_at = None

    def write(self, data):
        self.output.write(data)
        self.data.extend(data)
        if self.loaded_at is None and b"ime: SD dictionary loaded" in self.data:
            self.loaded_at = time.monotonic()

    def flush(self):
        self.output.flush()


def main():
    import serial

    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from board_reset import reset_board
    from sdroot.cache_lifecycle_device import Session

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--probe-ms", type=int, default=500)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 10 or not 100 <= args.probe_ms <= 5000:
        parser.error("cycles must be 1..10 and probe-ms 100..5000")
    if args.report.exists() or args.log.resolve() == args.report.resolve():
        parser.error("report must be a new path distinct from the serial log")
    report = {"result": "INCOMPLETE", "probe_ms": args.probe_ms, "cycles": []}
    link = serial.Serial(port=None, baudrate=115200, timeout=.01,
                         write_timeout=10, exclusive=True)
    link.dtr = link.rts = False
    link.port = args.port
    with args.log.open("xb") as output, args.report.open("x") as result:
        try:
            link.open()
            for cycle in range(args.cycles):
                link.reset_input_buffer()
                log = ReceiptLog(output)
                session = Session(link, log)
                reset_board(link)
                boot = session.until(b"> ", 45)
                if b"MeOS " not in boot or b"shell-ready" not in boot:
                    raise RuntimeError("MeOS boot identity missing")
                # Suppress Home before its 1.5 s grace expires.
                link.write(b"\x03\r")
                session.until(b"> ")
                latency = []
                deadline = time.monotonic() + 90
                while not session.dictionary_loaded:
                    if time.monotonic() > deadline:
                        raise TimeoutError("background dictionary did not load")
                    started = time.monotonic()
                    receipt = session.command("echo IME-BOOT-PROBE")
                    if "IME-BOOT-PROBE" not in receipt:
                        raise RuntimeError("echo probe reply missing")
                    latency.append((time.monotonic() - started) * 1000)
                    time.sleep(args.probe_ms / 1000)
                state = session.command("sdroot")
                if "active=1" not in state:
                    raise RuntimeError("persistent SD root missing")
                session.command("ps")
                data = bytes(log.data)
                if b"imedict " in data[data.rfind(b"SEND ps"):]:
                    raise RuntimeError("dictionary task did not exit")
                stack = re.search(rb"ime: loader stack (\d+)/(\d+) bytes, guard OK", data)
                loaded = re.search(rb"ime: SD dictionary loaded \((\d+) KB\)", data)
                shell = re.search(rb"BOOT \+(\d+) ms shell-ready", data)
                version = re.search(rb"App version:\s+(\S+)", data)
                metric = re.search(rb"ime: dictionary I/O bytes=(\d+) ms=(\d+) naps=(\d+)", data)
                if not stack or not loaded or not shell or not version or not latency:
                    raise RuntimeError("boot/dictionary/stack/probe receipt missing")
                latency.sort()
                item = {"cycle": cycle + 1, "firmware": version[1].decode(),
                        "shell_ready_device_ms": int(shell[1]),
                        "dictionary_ready_host_ms": round((log.loaded_at - log.started) * 1000, 1),
                        "dictionary_kib": int(loaded[1]), "loader_stack_used": int(stack[1]),
                        "loader_stack_bytes": int(stack[2]), "probe_count": len(latency),
                        "probe_median_ms": round(statistics.median(latency), 1),
                        "probe_p95_ms": round(latency[math.ceil(len(latency) * .95) - 1], 1),
                        "probe_max_ms": round(max(latency), 1)}
                if metric:
                    item["dictionary_io"] = dict(zip(("bytes", "ms", "naps"),
                                                      (int(metric[i]) for i in (1, 2, 3))))
                report["cycles"].append(item)
                print(json.dumps(item), flush=True)
            report["result"] = "PASS"
        except Exception as exc:
            report["result"] = "FAIL"
            report["error"] = str(exc)
            raise
        finally:
            link.close()
            output.flush()
            report["serial_log_sha256"] = hashlib.sha256(args.log.read_bytes()).hexdigest()
            json.dump(report, result, indent=2)
            result.write("\n")


if __name__ == "__main__":
    main()
