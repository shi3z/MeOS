#!/usr/bin/env python3
"""Export only the declared simulator source closure; never stage the live checkout."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def source_paths():
    paths = []
    for line in (ROOT / "tools/sim/source-files.txt").read_text().splitlines():
        value = line.split("#", 1)[0].strip()
        if not value:
            continue
        path = PurePosixPath(value)
        if path.is_absolute() or ".." in path.parts or str(path) != value:
            raise ValueError(f"invalid source path: {value}")
        source = ROOT / value
        if source.resolve() != source or not source.is_file():
            raise ValueError(f"source missing or symlinked: {value}")
        if value in paths:
            raise ValueError(f"duplicate source path: {value}")
        paths.append(value)
    for directory in ("tools/sim", "tools/scheme", "user/scheme"):
        for file in (ROOT / directory).rglob("*"):
            if file.is_file() and "__pycache__" not in file.parts:
                relative = file.relative_to(ROOT).as_posix()
                if relative not in paths:
                    raise ValueError(f"undeclared candidate file: {relative}")
    return sorted(paths)


def export(destination):
    paths = source_paths()
    destination.mkdir(parents=True, exist_ok=False)
    checksums = {}
    for name in paths:
        data = (ROOT / name).read_bytes()
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        checksums[name] = hashlib.sha256(data).hexdigest()
    revision = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
    ).stdout.strip()
    metadata = {
        "format": 1,
        "base_revision": revision,
        "source": "explicit current-source snapshot, including declared uncommitted files",
        "release_status": "license-review-required; not a published release",
        "sha256": checksums,
    }
    (destination / "SOURCE_MANIFEST.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
    )
    (destination / "README.md").write_text(
        "# MeOS App Simulator source candidate\n\n"
        "Run `sh tools/sim/run.sh` with a C11 compiler and SDL2 development files.\n"
        "Run `sh tools/scheme/host_test.sh` and `sh tools/sim/replay_test.sh` for validation.\n"
        "See tools/sim/README.md and tools/sim/RELEASING.md.\n\n"
        "This is a review/test snapshot. The MeOS project license remains unresolved;\n"
        "third-party notices do not grant a license to the entire project.\n"
    )
    print(f"Source candidate: {destination} ({len(paths)} declared files)")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new output directory")
    args = parser.parse_args()
    try:
        export(args.output.resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"source export failed: {error}\n")
