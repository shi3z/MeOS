#!/usr/bin/env python3
"""Check the ICMP load generator's integrity and reply attribution offline."""
import struct
import unittest

from wifi_coexistence_device import PingLoad, checksum, MeasurementInvalid, validate_clock, sd_io_stats


def reply(load, index):
    packet = struct.pack("!BBHHH", 0, 0, 0, 7, index & 65535) + load.body(index)
    return packet[:2] + struct.pack("!H", checksum(packet)) + packet[4:]


class IntegrityTests(unittest.TestCase):
    def setUp(self):
        self.load = PingLoad("127.0.0.1", 25)
        self.load.phases["pak"] = {"sent": 1, "rtt_ms": []}
        self.load.pending[123] = (1.0, "pak")

    def test_generated_requests(self):
        for payload in (12, 13, 1400):
            load = PingLoad("127.0.0.1", 25, payload)
            packet = load.packet(70000)
            self.assertEqual(len(packet), payload + 8)
            self.assertEqual(checksum(packet), 0)
            self.assertEqual(packet[8:20], load.nonce + struct.pack("!I", 70000))

    def test_ipv4_reply_and_duplicate(self):
        packet = b"\x45" + bytes(19) + reply(self.load, 123)
        self.load.accept(packet, 1.025)
        self.load.accept(packet, 1.030)
        self.assertNotIn(123, self.load.pending)
        self.assertEqual(len(self.load.phases["pak"]["rtt_ms"]), 1)
        self.assertAlmostEqual(self.load.phases["pak"]["rtt_ms"][0], 25)

    def test_corrupt_reply_does_not_count(self):
        packet = bytearray(reply(self.load, 123))
        packet[-1] ^= 1
        self.load.accept(bytes(packet), 1.025)
        self.assertEqual(self.load.invalid, 1)
        self.assertIn(123, self.load.pending)

    def test_changed_body_with_valid_checksum_does_not_count(self):
        packet = bytearray(reply(self.load, 123))
        packet[-1] ^= 1
        packet[2:4] = b"\0\0"
        packet[2:4] = struct.pack("!H", checksum(bytes(packet)))
        self.load.accept(bytes(packet), 1.025)
        self.assertEqual(self.load.invalid, 1)
        self.assertIn(123, self.load.pending)

    def test_unrelated_or_truncated_reply_is_ignored(self):
        other = PingLoad("127.0.0.1", 25)
        for packet in (b"", b"\x45\0", reply(self.load, 123)[:10], reply(other, 123)):
            self.load.accept(packet, 1.025)
        self.assertEqual(self.load.invalid, 0)
        self.assertIn(123, self.load.pending)

    def test_burst_accounting_and_bounds(self):
        class Sink:
            def __init__(self):
                self.packets = []

            def send(self, data):
                self.packets.append(data)
                return len(data)

        load = PingLoad("127.0.0.1", 100, burst_packets=12)
        load.sock = Sink()
        load.phase = "burst"
        self.assertEqual(load.send_burst(70000), 70012)
        self.assertEqual(load.phases["burst"]["sent"], 12)
        self.assertEqual(len(load.pending), 12)
        for i, packet in enumerate(load.sock.packets):
            self.assertEqual(checksum(packet), 0)
            self.assertEqual(struct.unpack("!I", packet[16:20])[0], 70000 + i)
            self.assertEqual(load.pending[70000 + i][1], "burst")
        for invalid in (0, 17):
            with self.assertRaises(ValueError):
                PingLoad("127.0.0.1", 100, burst_packets=invalid)

    def test_clock_discontinuities_invalidate_measurements(self):
        validate_clock(38.8, 38.9, 38715)
        validate_clock(100, 100.1)
        for args in ((10.4, 800, 38715), (10.4, 10.4, 38715),
                     (38, 30, 38000), (38, 38, 0)):
            with self.assertRaises(MeasurementInvalid):
                validate_clock(*args)

    def test_sd_timings_require_all_classes_and_fields(self):
        fields = "calls=2 blocks=2 errors=0 wait_ms=1 prepare_ms=2 dma_ms=3 finish_ms=4 hold_ms=9 " \
                 "max_wait_us=100 max_prepare_us=200 max_dma_us=300 max_finish_us=400 max_hold_us=900 max_polls=50"
        lines = [f"SDMMC {op} max_blocks={n} {fields}" for op in ("READ", "WRITE") for n in (1,8,32,128)]
        text = "\n".join(lines)
        parsed = sd_io_stats(text)
        self.assertEqual(parsed["read"]["128"]["max_hold_us"], 900)
        for invalid in ("\n".join(lines[:-1]), text + "\n" + lines[0], text.replace("max_polls=50", "")):
            with self.assertRaises(RuntimeError):
                sd_io_stats(invalid)


if __name__ == "__main__":
    unittest.main()
