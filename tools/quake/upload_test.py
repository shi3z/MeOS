"""Protocol qualification with a fake serial peer, no device/assets needed."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
import zlib

spec = importlib.util.spec_from_file_location("uploader", Path(__file__).with_name("upload.py"))
uploader = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uploader)


class Peer:
    reject = False
    bad_crc = False

    def __init__(self):
        self.rx = bytearray()
        self.data = bytearray()
        self.closed = False
        self.size = 0

    def open(self):
        pass

    def close(self):
        self.closed = True

    @property
    def in_waiting(self):
        return len(self.rx)

    def read(self, n):
        assert self.rx, "unexpected protocol wait"
        result = bytes(self.rx[:n])
        del self.rx[:n]
        return result

    def write(self, b):
        if b == b"\r" and not self.size:
            self.rx.extend(b"\r\n> ")
        elif b.startswith(b"quake receive ") and not self.size:
            if self.reject:
                self.rx.extend(b"QFAIL destination exists\r\n")
                return
            fields = b.split()
            self.size = int(fields[2])
            self.crc = int(fields[3], 16)
            self.rx.extend(f"QREADY {self.size} 4096\r\n".encode())
        elif len(self.data) < self.size:
            self.data.extend(b)
            self.rx.extend(f"QACK {len(self.data)}\r\n".encode())
            if len(self.data) == self.size:
                assert zlib.crc32(self.data) == self.crc
                self.rx.extend(f"QDONE {self.size} {self.crc:08x}\r\n> ".encode())
        else:
            assert b == b"quake verify\r"
            self.rx.extend(f"QVERIFY {self.size} {self.crc ^ int(self.bad_crc):08x}\r\n> ".encode())


class UploadTests(unittest.TestCase):
    def run_peer(self, peer):
        payload = bytes(range(256)) * 17 + b"PACK"
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "test.pak"
            source.write_bytes(payload)
            with patch.object(sys, "argv", ["upload", "--port", "fake", str(source)]), \
                    patch.object(sys, "platform", "linux"), \
                    patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=lambda: peer)}), \
                    contextlib.redirect_stdout(io.StringIO()):
                uploader.main()
        self.assertEqual(bytes(peer.data), payload)
        self.assertTrue(peer.closed)

    def test_transfer_and_stored_verification(self):
        self.run_peer(Peer())

    def test_existing_destination_stops_before_binary(self):
        peer = Peer()
        peer.reject = True
        with self.assertRaisesRegex(RuntimeError, "QFAIL"):
            self.run_peer(peer)
        self.assertFalse(peer.data)
        self.assertTrue(peer.closed)

    def test_stored_crc_mismatch_is_failure(self):
        peer = Peer()
        peer.bad_crc = True
        with self.assertRaisesRegex(RuntimeError, "stored-data verification"):
            self.run_peer(peer)
        self.assertTrue(peer.closed)


if __name__ == "__main__":
    unittest.main()
