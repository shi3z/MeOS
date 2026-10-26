#!/usr/bin/env python3
"""Repeat SD cache trim/recreate with font rendering and unsaved Write sessions.

Requires an idle MeOS serial shell and provisioned BIZ UD assets. No firmware
or media writes are issued. --boot-contention resets the board (discarding RAM)
and additionally requires overlap with the background IME dictionary reader.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time

import serial


class Session:
    def __init__(self, link, log):
        self.link = link
        self.log = log
        self.pending = bytearray()
        self.dictionary_loaded = False

    def until(self, token, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            at = self.pending.find(token)
            if at >= 0:
                result = bytes(self.pending[:at + len(token)])
                del self.pending[:at + len(token)]
                return result
            block = self.link.read(max(1, self.link.in_waiting))
            if not block:
                continue
            self.log.write(block)
            self.log.flush()
            self.pending.extend(block)
            self.dictionary_loaded |= b"ime: SD dictionary loaded" in self.pending
            if b"nihos:" in self.pending:
                raise RuntimeError("Tab5 is running nihos; aborting the MeOS test")
            for failure in (b"MeOS PANIC:", b"guard FAILED", b"ime: SD dictionary read failed",
                            b"ime: SD dictionary is damaged"):
                if failure in self.pending:
                    raise RuntimeError(f"device failure: {failure!r}")
        raise TimeoutError(f"waiting for {token!r}; tail={self.pending[-800:]!r}")

    def command(self, text):
        self.log.write(f"\nSEND {text}\n".encode())
        self.link.write(text.encode() + b"\r")
        self.until(text.encode())
        return self.until(b"> ").decode(errors="replace").replace("\r", "")


def memory(text):
    psram = re.search(r"psram\s+total \d+ KB\s+used \d+ KB\s+free (\d+) KB", text)
    cache = re.search(r"SD asset\s+(\d+) KB cached", text)
    atlas = re.search(r"BIZ UD\s+(\d+) KB cached", text)
    if not psram or not cache or not atlas:
        raise RuntimeError(f"memory counters missing: {text!r}")
    return tuple(int(match.group(1)) for match in (psram, cache, atlas))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--cycles", type=int, default=20)
    parser.add_argument("--boot-contention", action="store_true")
    parser.add_argument("--settle-seconds", type=int, default=0,
                        help="hold serial open after IME overlap before sampling the memory baseline")
    parser.add_argument("--log", type=Path, required=True,
                        help="new serial transcript path; existing files are refused")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000 or not 0 <= args.settle_seconds <= 120:
        parser.error("--cycles must be 1..1000 and --settle-seconds 0..120")
    link = serial.Serial(port=None, baudrate=115200, timeout=0.05,
                         write_timeout=10, exclusive=True)
    link.dtr = link.rts = False
    link.port = args.port
    with args.log.open("xb") as log:
        link.open()
        session = Session(link, log)
        try:
            if args.boot_contention:
                sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
                from board_reset import reset_board
                reset_board(link)
                boot = session.until(b"> ")
                if b"MeOS " not in boot or b"shell-ready" not in boot:
                    raise RuntimeError("MeOS boot/shell identity receipt missing")
            else:
                link.write(b"\x03\r")
                session.until(b"> ")
            # Consume the initial queued Ctrl-C before starting commands.
            time.sleep(0.2)
            link.write(b"\x03\r")
            session.until(b"> ")

            overlap = 0
            deadline = time.monotonic() + 90
            while "imedict" in session.command("ps"):
                if time.monotonic() >= deadline:
                    raise TimeoutError("background dictionary reader did not finish")
                receipt = session.command("mem trim")
                # A reader may already recreate the cache before the shell
                # prints memory; only the actual release receipt is counted.
                if re.search(r"SD asset cache: trimmed \((\d+) KB released\)", receipt):
                    overlap += 1
                # The shell has higher priority than imedict. Back-to-back
                # commands can keep it runnable and starve the reader that
                # this test is meant to overlap with.
                time.sleep(0.05)
            if args.boot_contention and (not overlap or not session.dictionary_loaded):
                raise RuntimeError("no verified trim/IME-reader overlap and successful dictionary load")

            # Faster asset reads can finish IME overlap while Tailscale still
            # changes its startup allocations. Keep the verified overlap, then
            # settle with this same connection open, as in the network harness.
            if args.settle_seconds:
                print(f"Settling background services for {args.settle_seconds} s after IME overlap", flush=True)
                time.sleep(args.settle_seconds)
            if "active=1" not in session.command("sdroot"):
                raise RuntimeError("persistent SD root is not active")
            listing = session.command("ls /assets/fonts/BIZUDGothic-Regular.ttf")
            if "4667380" not in listing:
                raise RuntimeError("pinned OpenType asset missing")
            session.command("mem trim")
            free_trimmed, cache, atlas = memory(session.command("mem"))
            if cache or atlas:
                raise RuntimeError("idle caches did not release before baseline")

            results = []
            for cycle in range(args.cycles):
                if cycle:
                    receipt = session.command("mem trim")
                    if "SD asset cache: trimmed" not in receipt:
                        raise RuntimeError("resident cache trim receipt missing")
                    free_now, cache, atlas = memory(session.command("mem"))
                    if cache or atlas or abs(free_now - free_trimmed) > 32:
                        raise RuntimeError(f"trim baseline drift in cycle {cycle + 1}")
                size = (20, 24, 28, 32)[cycle % 4]
                link.write(f"fontbench {size}\r".encode())
                session.until(b"FONTBENCH ", 60)
                receipt = session.until(b"\n").decode(errors="replace")
                metric = re.search(r"(\d+) px: load=(\d+) ms metric=(\d+) us first=(\d+) us warm=(\d+) us "
                                   r"misses=(\d+) hits=(\d+).* font=(\d+) resident=(\d+)", receipt)
                if (not metric or int(metric[1]) != size or int(metric[6]) == 0 or
                        int(metric[7]) == 0 or int(metric[8]) != 4667380 or int(metric[9]) != 0):
                    raise RuntimeError(f"font benchmark failed: {receipt!r}")
                link.write(b"q")
                session.until(b"> ")
                font_free, cache, atlas = memory(session.command("mem"))
                if cache < 1100 or atlas or abs(font_free + cache - free_trimmed) > 32:
                    raise RuntimeError("cache recreation or font memory cleanup failed")

                # No text is changed or saved. Two size/layout transitions
                # exercise repeated OpenType teardown and reinitialization.
                link.write(b"write\r")
                session.until(b"write: BIZ UD 24px outline", 60)
                for keys, marker in ((b"\x1bOS", b"write: outline 28px"),
                                     (b"\x1bOR", b"write: outline 24px"),
                                     (b"\x14", b"write: layout horizontal"),
                                     (b"\x14", b"write: layout vertical")):
                    link.write(keys)
                    session.until(marker)
                link.write(b"\x11")
                session.until(b"> ")
                free_now, cache_now, atlas = memory(session.command("mem"))
                if cache_now != cache or atlas or abs(free_now - font_free) > 32:
                    raise RuntimeError("Write memory cleanup failed")
                result = {"cycle": cycle + 1, "px": size, "load_ms": int(metric[2]),
                          "first_us": int(metric[4]), "warm_us": int(metric[5]),
                          "free_kib": free_now, "cache_kib": cache_now}
                results.append(result)
                print(f"cycle {cycle + 1}/{args.cycles}: {size}px, PSRAM {free_now} KiB, cache {cache_now} KiB", flush=True)
            session.command("mem trim")
            free_final, cache, atlas = memory(session.command("mem"))
            if cache or atlas or abs(free_final - free_trimmed) > 32:
                raise RuntimeError("final trim did not restore baseline memory")
            report = {"result": "PASS", "boot_contention": args.boot_contention,
                      "settle_seconds": args.settle_seconds,
                      "ime_overlap_trims": overlap, "trimmed_baseline_kib": free_trimmed,
                      "trimmed_final_kib": free_final, "cycles": results}
            print("SD CACHE LIFECYCLE DEVICE PASS: " + json.dumps(report))
        finally:
            link.close()


if __name__ == "__main__":
    main()
