#!/usr/bin/env python3
"""Reject oversized engine frames from GCC -fstack-usage output (not a call-graph proof)."""
import argparse
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('build', type=Path)
    args = p.parse_args()
    frames = []
    for report in args.build.rglob('*.su'):
        for row in report.read_text().splitlines():
            fields = row.split('\t')
            if len(fields) >= 3:
                frames.append((int(fields[1]), fields[0].rsplit(':', 1)[-1], fields[2]))
    names = {name for size, name, kind in frames}
    required = {'COM_LoadPackFile', 'R_AliasDrawModel', 'R_ScanEdges', 'SV_SendClientDatagram'}
    if not required <= names:
        p.error('missing engine stack reports; build the Quake component with -fstack-usage')
    unsafe = [(size, name, kind) for size, name, kind in frames if size > 16384 or ('dynamic' in kind and 'bounded' not in kind)]
    if unsafe:
        p.error('oversized/unbounded engine stack frames: ' + repr(unsafe))
    print('Engine stack frames:', len(frames), 'checked; largest:', sorted(frames, reverse=True)[:4])

if __name__ == '__main__':
    main()
