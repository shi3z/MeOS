#!/usr/bin/env python3
"""Negative checks for accidental game-data publication and wrong firmware."""
import importlib.util
import json
import pathlib
import struct
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('release', pathlib.Path(__file__).with_name('release.py'))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def elf(name, machine=243):
    strings = b'\0' + name.encode() + b'\0'
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + bytes(9), 2, machine, 1, 0, 0, 52, 0, 52, 0, 0, 40, 3, 0)
    sections = bytes(40)
    sections += struct.pack('<10I', 0, 3, 0, 0, 172, len(strings), 0, 0, 1, 0)
    sections += struct.pack('<10I', 0, 2, 0, 0, 172 + len(strings), 16, 1, 0, 4, 16)
    return header + sections + strings + struct.pack('<IIIBBH', 1, 0, 0, 0, 0, 1)


class SafetyChecks(unittest.TestCase):
    def test_defined_symbols_include_embedded_asset(self):
        self.assertEqual(release.elf_symbols(elf('doom_wad_start')), {'doom_wad_start'})

    def test_wrong_architecture_and_truncation_rejected(self):
        for data in [elf('doom_main', 94), elf('doom_main')[:-1], b'\x7fELF']:
            with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                release.elf_symbols(data)

    def test_dirty_source_and_existing_archive_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            output = pathlib.Path(temp) / "candidate.zip"
            with patch.object(release, "verify", return_value={"source_dirty": True}):
                with self.assertRaisesRegex(ValueError, "working tree is dirty"):
                    release.package(pathlib.Path(temp), output)
            output.write_bytes(b"existing candidate")
            with patch.object(release, "verify", return_value={"source_dirty": False}):
                with self.assertRaisesRegex(ValueError, "refusing to overwrite"):
                    release.package(pathlib.Path(temp), output)
            self.assertEqual(output.read_bytes(), b"existing candidate")

    def test_stale_embedding_settings_rejected_before_packaging(self):
        base = dict(schema=1, board='tab5', architecture='riscv32', wad_embedding_allowed=False, wad_embedded=False, wad_bytes=0)
        for changes, expected in [({'wad_embedding_allowed': True}, 'explicitly disabled'), ({'wad_embedded': True}, 'game data'), ({'wad_bytes': 1}, 'game data'), ({'wad_bytes': False}, 'game data')]:
            with self.subTest(changes=changes), tempfile.TemporaryDirectory() as temp:
                path = pathlib.Path(temp)
                (path / 'doom-build.json').write_text(json.dumps(base | changes))
                with self.assertRaisesRegex(ValueError, expected):
                    release.verify(path)

    def test_stale_wad_compile_input_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            path = pathlib.Path(temp)
            (path / 'config').mkdir()
            (path / 'doom-build.json').write_text(json.dumps(dict(schema=1, board='tab5', architecture='riscv32', wad_embedding_allowed=False, wad_embedded=False, wad_bytes=0)))
            (path / 'CMakeCache.txt').write_text('M5OS_DOOM_EMBED_WAD:BOOL=OFF\n')
            (path / 'config/sdkconfig.h').write_text('#define CONFIG_APP_REPRODUCIBLE_BUILD 1\n#define CONFIG_IDF_TARGET_ESP32P4 1\n')
            (path / 'compile_commands.json').write_text('M5OS_DOOM=1 doom_wad.z.S')
            with self.assertRaisesRegex(ValueError, 'WAD compilation input'):
                release.verify(path)


if __name__ == '__main__':
    unittest.main()
