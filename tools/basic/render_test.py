#!/usr/bin/env python3
"""Smoke-test the host BASIC SVG capture without using the board or SDL."""
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
NS = {"svg": "http://www.w3.org/2000/svg"}


def render(source, destination):
    return subprocess.run(
        ["sh", str(ROOT / "tools/basic/render.sh"), str(source), str(destination)],
        cwd=ROOT, capture_output=True, text=True,
    )


def main():
    with tempfile.TemporaryDirectory(prefix="meos-basic-svg-") as temporary:
        work = Path(temporary)
        output = work / "sine.svg"
        run = render(ROOT / "user/basic/examples/sine_plot.bas", output)
        assert run.returncode == 0, run.stderr
        svg = ET.parse(output).getroot()
        points = [node for node in svg.findall("svg:rect", NS)
                  if node.get("width") == "1" and node.get("height") == "1"]
        assert len(points) == 126 and len(svg.findall("svg:line", NS)) == 1
        assert svg.find("svg:text", NS).text == "y = sin(x)"
        assert all(0 <= int(p.get("x")) < 1280 and 0 <= int(p.get("y")) < 720 for p in points)
        bad = work / "invalid.bas"
        bad.write_text("10 SCREEN 1\n20 PSET .5,0,1\n")
        output.write_text("keep prior output\n")
        run = render(bad, output)
        assert run.returncode != 0 and "integer argument required" in run.stderr
        assert output.read_text() == "keep prior output\n"
        escaped = work / "escaped.bas"
        escaped.write_text('10 SCREEN 1\n20 TEXT 0,0,"x < y & z",65535\n')
        run = render(escaped, output)
        assert run.returncode == 0, run.stderr
        assert ET.parse(output).getroot().find("svg:text", NS).text == "x < y & z"
    print("BASIC SVG capture smoke tests PASS")


if __name__ == "__main__":
    main()
