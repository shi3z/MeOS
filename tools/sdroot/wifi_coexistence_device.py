#!/usr/bin/env python3
"""Read-only SD/graphics coexistence test with bounded ICMP traffic to Tab5.

Requires a reachable Wi-Fi-connected MeOS Tab5 with the existing Quake PAK and
BIZ UD assets. Serial open may reset the board. The tool issues no firmware-write
or media-mutation command; normal background services may persist their state.
Only the IPv4 address reported by the attached board receives test packets.
"""
import argparse
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import select
import socket
import statistics
import struct
import threading
import time


class MeasurementInvalid(RuntimeError):
    """The host timebase stopped or jumped during a hardware measurement."""


def validate_clock(monotonic_seconds, wall_seconds, device_ms=None):
    # macOS monotonic time can exclude system sleep while the board continues.
    # A large wall-clock adjustment is also unsuitable for a paced measurement.
    if abs(wall_seconds - monotonic_seconds) > 1:
        raise MeasurementInvalid(f"host clock discontinuity: monotonic={monotonic_seconds:.3f}s "
                                 f"wall={wall_seconds:.3f}s")
    if device_ms is not None and (device_ms <= 0 or monotonic_seconds + 1 < device_ms / 1000):
        raise MeasurementInvalid(f"host/device duration mismatch: host={monotonic_seconds:.3f}s "
                                 f"device={device_ms}ms")


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return total ^ 65535


def sd_io_stats(receipt):
    rows = {"read": {}, "write": {}}
    required = {"calls", "blocks", "errors", "wait_ms", "prepare_ms", "dma_ms",
                "finish_ms", "hold_ms", "max_wait_us", "max_prepare_us",
                "max_dma_us", "max_finish_us", "max_hold_us", "max_polls"}
    for direction, limit, fields in re.findall(r"SDMMC (READ|WRITE) max_blocks=(\d+) ([^\n]+)", receipt):
        key = direction.lower()
        values = {name: int(value) for name, value in re.findall(r"([a-z_]+)=(\d+)", fields)}
        if limit in rows[key] or not required.issubset(values):
            raise RuntimeError("duplicate/incomplete SDMMC diagnostic row")
        rows[key][limit] = values
    if any(set(table) != {"1", "8", "32", "128"} for table in rows.values()):
        raise RuntimeError("SDMMC diagnostic classes missing")
    return rows


class PingLoad:
    def __init__(self, target, pps, payload_bytes=1400, burst_packets=1):
        self.target = str(ipaddress.IPv4Address(target))
        if (not 1 <= pps <= 200 or not 12 <= payload_bytes <= 1400 or
                not 1 <= burst_packets <= 16):
            raise ValueError("rate must be 1..200 pps, payload 12..1400 bytes and burst 1..16")
        self.pps = pps
        self.payload_bytes = payload_bytes
        self.burst_packets = burst_packets
        self.nonce = os.urandom(8)
        self.pattern = bytes((i * 29 + 7) & 255 for i in range(payload_bytes - 12))
        self.identifier = int.from_bytes(os.urandom(2), "big")
        self.pending = {}
        self.phases = {}
        self.phase = "idle"
        self.invalid = 0
        self.error = None
        self.stop_event = threading.Event()
        self.pause_event = threading.Event()
        self.thread = None
        self.sock = None

    def body(self, index):
        return self.nonce + struct.pack("!I", index) + self.pattern

    def packet(self, index):
        header = struct.pack("!BBHHH", 8, 0, 0, self.identifier, index & 65535)
        body = self.body(index)
        return struct.pack("!BBHHH", 8, 0, checksum(header + body),
                           self.identifier, index & 65535) + body

    def accept(self, packet, now):
        # macOS datagram ICMP sockets include the IPv4 header; Linux may not.
        if packet and packet[0] >> 4 == 4:
            ihl = (packet[0] & 15) * 4
            if ihl < 20 or len(packet) < ihl + 8:
                return
            packet = packet[ihl:]
        if len(packet) < 20 or packet[:2] != b"\0\0" or packet[8:16] != self.nonce:
            return
        index = struct.unpack("!I", packet[16:20])[0]
        if index not in self.pending:
            return
        if checksum(packet) or packet[8:] != self.body(index):
            self.invalid += 1
            return
        sent_at, phase = self.pending.pop(index)
        self.phases[phase]["rtt_ms"].append((now - sent_at) * 1000)

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_ICMP)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
        self.sock.connect((self.target, 0))
        self.sock.setblocking(False)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        index = 0
        next_send = time.monotonic()
        started_mono, started_wall = time.monotonic(), time.time()
        try:
            while not self.stop_event.is_set():
                now = time.monotonic()
                validate_clock(now - started_mono, time.time() - started_wall)
                if not self.pause_event.is_set() and now >= next_send:
                    index = self.send_burst(index)
                    # Pace groups at the requested average rate, without
                    # catching up with additional groups after a host stall.
                    next_send = time.monotonic() + self.burst_packets / self.pps
                delay = 0.05 if self.pause_event.is_set() else min(0.05, max(0, next_send - now))
                if select.select([self.sock], [], [], delay)[0]:
                    self.accept(self.sock.recv(4096), time.monotonic())
        except Exception as exc:
            self.error = exc

    def send_burst(self, index):
        phase = self.phase
        stats = self.phases.setdefault(phase, {"sent": 0, "rtt_ms": []})
        for _ in range(self.burst_packets):
            packet = self.packet(index)
            now = time.monotonic()
            if self.sock.send(packet) != len(packet):
                raise OSError("short ICMP datagram send")
            self.pending[index] = (now, phase)
            stats["sent"] += 1
            index += 1
        return index

    def finish(self):
        self.pause_event.set()
        # Keep receiving after the final send so trailing replies are counted.
        time.sleep(2)
        self.stop_event.set()
        self.thread.join(3)
        self.sock.close()
        if self.thread.is_alive():
            raise RuntimeError("ICMP worker failed to stop")
        if self.error:
            raise self.error
        report = {"pps": self.pps, "payload_bytes": self.payload_bytes,
                  "burst_packets": self.burst_packets,
                  "invalid_replies": self.invalid, "phases": {}}
        for phase, stats in self.phases.items():
            rtt = sorted(stats["rtt_ms"])
            received = len(rtt)
            report["phases"][phase] = {
                "sent": stats["sent"], "received": received,
                "loss_percent": round(100 * (stats["sent"] - received) / stats["sent"], 3),
                "rtt_median_ms": round(statistics.median(rtt), 3) if rtt else None,
                "rtt_p95_ms": round(rtt[math.ceil(len(rtt) * .95) - 1], 3) if rtt else None,
            }
        return report


def main():
    import serial
    from cache_lifecycle_device import Session, memory

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--pps", type=int, nargs="+", default=[25, 100])
    parser.add_argument("--reads", type=int, default=2)
    parser.add_argument("--burst-packets", type=int, default=1,
                        help="1..16 packets per group; pps limits the average group pacing")
    parser.add_argument("--diagnostics", action="store_true",
                        help="record wifi stats counters on firmware that supports them")
    parser.add_argument("--sd-diagnostics", action="store_true",
                        help="record native SDMMC stage timings outside traffic phases")
    parser.add_argument("--reset", action="store_true", help="reset the board, discarding unsaved RAM")
    parser.add_argument("--settle-seconds", type=int, default=0,
                        help="hold the serial connection open before measurements for background services")
    parser.add_argument("--max-loss-percent", type=float, default=5)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if (not 1 <= args.reads <= 10 or not 0 <= args.max_loss_percent <= 100 or
            not 0 <= args.settle_seconds <= 120 or
            not 1 <= args.burst_packets <= 16 or
            any(not 1 <= rate <= 200 for rate in args.pps)):
        parser.error("reads 1..10, pps 1..200, burst 1..16, loss limit 0..100, settle-seconds 0..120")
    if args.report.exists() or args.log.resolve() == args.report.resolve():
        parser.error("report must be a new path distinct from the serial log")
    report = {"result": "INCOMPLETE", "baseline": [], "loads": [],
              "settle_seconds": args.settle_seconds}
    started_mono, started_wall = time.monotonic(), time.time()

    def check_host_clock():
        mono, wall = time.monotonic() - started_mono, time.time() - started_wall
        report["host_clock"] = {"monotonic_seconds": round(mono, 3),
                                "wall_seconds": round(wall, 3)}
        validate_clock(mono, wall)
    link = serial.Serial(port=None, baudrate=115200, timeout=.05,
                         write_timeout=10, exclusive=True)
    link.dtr = link.rts = False
    link.port = args.port
    with args.log.open("xb") as log, args.report.open("x") as output:
        link.open()
        session = Session(link, log)
        load = None
        try:
            if args.reset:
                import sys
                sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
                from board_reset import reset_board
                reset_board(link)
            else:
                link.write(b"\x03\r")
            boot = session.until(b"> ")
            # Suppress Home during its 1.5 s grace period, before any wait.
            link.write(b"\x03\r")
            session.until(b"> ")
            version = re.search(rb"App version:\s+(\S+)", boot)
            report["firmware_boot_version"] = version[1].decode() if version else "running shell"
            deadline = time.monotonic() + 90
            while "imedict" in session.command("ps"):
                if time.monotonic() > deadline:
                    raise TimeoutError("waiting for dictionary reader")
                time.sleep(.1)
            time.sleep(2)
            # Merely reopening USB serial can reset Tab5. Keep this connection
            # open while optional services finish their temporary allocations;
            # sleeping before opening the port would not provide a warm baseline.
            if args.settle_seconds:
                print(f"Settling background services for {args.settle_seconds} s with serial held open", flush=True)
                time.sleep(args.settle_seconds)
            check_host_clock()
            if "active=1" not in session.command("sdroot"):
                raise RuntimeError("SD root is not active")
            wifi = session.command("wifi status")
            address = re.search(r"IPv4\s+:\s+(\d+\.\d+\.\d+\.\d+)", wifi)
            if not address:
                raise RuntimeError("Tab5 is not connected to Wi-Fi")
            target = str(ipaddress.IPv4Address(address[1]))
            listing = session.command("ls /assets/quake/id1/pak0.pak")
            if "18689235" not in listing:
                raise RuntimeError("expected existing 18 MiB PAK is missing")

            def verify_pak():
                session.command("sdroot io-reset")
                started = time.monotonic()
                wall_started = time.time()
                link.write(b"crc32 /assets/quake/id1/pak0.pak\r")
                receipt = session.until(b"> ", 180)
                elapsed = time.monotonic() - started
                metric = re.search(rb"CRC32 74e3b084 18689235 bytes (\d+) ms\b", receipt)
                if not metric:
                    raise RuntimeError(f"PAK CRC mismatch or read failure: {receipt[-800:]!r}")
                wall_elapsed = time.time() - wall_started
                device_ms = int(metric[1])
                result = {"seconds": device_ms / 1000, "host_seconds": round(elapsed, 3),
                          "wall_seconds": round(wall_elapsed, 3), "device_ms": device_ms,
                          "bytes": 18689235, "crc32": "74e3b084"}
                report["last_read_timing"] = result
                validate_clock(elapsed, wall_elapsed, device_ms)
                result["kib_per_second"] = round(18689235 / 1024 / result["seconds"], 1)
                print("PAK " + json.dumps(result), flush=True)
                return result

            for _ in range(args.reads):
                if args.sd_diagnostics:
                    sd_io_stats(session.command("sd stats reset"))
                sample = verify_pak()
                if args.sd_diagnostics:
                    sample["sd_io"] = sd_io_stats(session.command("sd stats"))
                report["baseline"].append(sample)
            session.command("mem trim")
            free_before, _, _ = memory(session.command("mem"))
            report["trimmed_psram_before_kib"] = free_before
            for rate in args.pps:
                if args.sd_diagnostics:
                    sd_io_stats(session.command("sd stats reset"))
                if args.diagnostics:
                    receipt = session.command("wifi stats reset")
                    if "WIFI RX" not in receipt:
                        raise RuntimeError("Wi-Fi diagnostic firmware missing")
                load = PingLoad(target, rate, burst_packets=args.burst_packets)
                load.phase = "idle"
                load.start()
                time.sleep(3)
                if load.error:
                    raise load.error
                if sum(len(stats["rtt_ms"]) for stats in load.phases.values()) < 2:
                    raise RuntimeError("no verified ICMP reachability to attached Tab5")
                pak = []
                load.phase = "pak"
                for _ in range(args.reads):
                    pak.append(verify_pak())
                load.phase = "font"
                link.write(b"fontbench 24\r")
                session.until(b"FONTBENCH ", 60)
                font = session.until(b"\n").decode(errors="replace")
                if not re.search(r"24 px: .*font=4667380 resident=0", font):
                    raise RuntimeError("font benchmark receipt missing")
                link.write(b"q")
                session.until(b"> ")
                load.phase = "write"
                link.write(b"write\r")
                session.until(b"write: BIZ UD 24px outline", 60)
                for keys, marker in ((b"\x1bOS", b"write: outline 28px"),
                                     (b"\x14", b"write: layout horizontal"),
                                     (b"\x14", b"write: layout vertical")):
                    link.write(keys)
                    session.until(marker)
                link.write(b"\x11")
                session.until(b"> ")
                summary = load.finish()
                load = None
                check_host_clock()
                if args.diagnostics:
                    receipt = session.command("wifi stats")
                    diag = {}
                    for group, fields in re.findall(r"WIFI (RX|TX|ICMP|TIME) ([^\n]+)", receipt):
                        for name, value in re.findall(r"([a-z_]+)=(\d+)", fields):
                            diag[f"{group.lower()}_{name}"] = int(value)
                    if "icmp_requests" not in diag or "rx_partials" not in diag:
                        raise RuntimeError("Wi-Fi diagnostic receipt incomplete")
                    summary["device_diagnostics"] = diag
                if args.sd_diagnostics:
                    summary["sd_io"] = sd_io_stats(session.command("sd stats"))
                summary["pak"] = pak
                summary["font_receipt"] = font.strip()
                report["loads"].append(summary)
                print("NETWORK " + json.dumps(summary), flush=True)
            session.command("mem trim")
            free_after, cache, atlas = memory(session.command("mem"))
            report["trimmed_psram_after_kib"] = free_after
            check_host_clock()
            if cache or atlas or abs(free_after - free_before) > 32:
                raise RuntimeError("PSRAM cleanup drift after concurrent I/O")
            failures = []
            for sample in report["baseline"] + report["loads"]:
                for direction in sample.get("sd_io", {}).values():
                    if any(row["errors"] for row in direction.values()):
                        failures.append("native SDMMC transfer error")
            for summary in report["loads"]:
                if summary["invalid_replies"]:
                    failures.append("invalid ICMP reply payload/checksum")
                diag = summary.get("device_diagnostics", {})
                if any(diag.get(key, 0) for key in (
                        "rx_bad", "rx_reg_errors", "rx_io_errors", "rx_plan_errors",
                        "tx_no_credit", "tx_io_errors", "icmp_failures")):
                    failures.append(f"transport errors: {diag}")
                for phase, stats in summary["phases"].items():
                    if stats["sent"] < 2 or stats["loss_percent"] > args.max_loss_percent:
                        failures.append(f"{summary['pps']} pps {phase}: {stats['loss_percent']}% loss")
            if failures:
                raise RuntimeError("; ".join(failures))
            report["result"] = "PASS"
            print("SD/WIFI COEXISTENCE DEVICE PASS", flush=True)
        except MeasurementInvalid as exc:
            report["result"] = "INVALID"
            report["error"] = str(exc)
            raise
        except Exception as exc:
            report["result"] = "FAIL"
            report["error"] = str(exc)
            raise
        finally:
            if load and load.thread:
                try:
                    report["interrupted_load"] = load.finish()
                except Exception as exc:
                    report["load_cleanup_error"] = str(exc)
            link.close()
            json.dump(report, output, indent=2)
            output.write("\n")


if __name__ == "__main__":
    main()
