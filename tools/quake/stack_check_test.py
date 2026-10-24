#!/usr/bin/env python3
"""Regression: the original oversized PAK frame must fail the build gate."""
from pathlib import Path
import subprocess
import sys
import tempfile

checker = Path(__file__).with_name('check_stack.py')
required = ['COM_LoadPackFile', 'R_AliasDrawModel', 'R_ScanEdges', 'SV_SendClientDatagram']
with tempfile.TemporaryDirectory(prefix='meos-quake-stack-check-') as temp:
    root = Path(temp)
    report = root / 'engine.su'
    good = ''.join(f'engine.c:1:1:{name}\t128\tstatic\n' for name in required)
    for text, succeeds in [(good, True), (good.replace('COM_LoadPackFile\t128', 'COM_LoadPackFile\t131280'), False),
                           (good.replace('128\tstatic', '128\tdynamic', 1), False), ('', False)]:
        report.write_text(text)
        result = subprocess.run([sys.executable, str(checker), str(root)], capture_output=True, text=True)
        assert (result.returncode == 0) == succeeds, result.stdout + result.stderr
print('Quake stack gate: original oversized frame, missing reports and unbounded use rejected PASS')
