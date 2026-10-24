#!/usr/bin/env python3
"""Prepare a separate opt-in Tab5 config/table, without flashing or moving partitions.

python3 tools/basic/prepare_programs_config.py sdkconfig.tab5 OUTPUT_SDKCONFIG --table-out OUTPUT_CSV
Use the output config with idf.py -B A_SEPARATE_DIRECTORY -DSDKCONFIG=OUTPUT_SDKCONFIG build.
"""
import argparse
import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
START, SIZE, FLASH = 0xC00000, 0x100000, 0x1000000


def size_value(text):
    text = text.strip()
    multiplier = 1
    if text[-1:].lower() in ("k", "m"):
        multiplier = 1024 if text[-1].lower() == "k" else 1024 * 1024
        text = text[:-1]
    return int(text, 0) * multiplier


def table_with_programs(text):
    rows = list(csv.reader(line for line in text.splitlines() if line.strip() and not line.lstrip().startswith("#")))
    found = False
    for row in rows:
        if len(row) < 5:
            raise ValueError("partition row must have explicit offset and size")
        name, kind, subtype, offset, length = (field.strip() for field in row[:5])
        off, length = size_value(offset), size_value(length)
        if length <= 0 or off < 0 or off + length > FLASH:
            raise ValueError("partition out of the 16 MiB flash range")
        if name == "programs":
            if found or kind != "data" or subtype.lower() != "0x42" or off != START or length != SIZE or any(field.strip() for field in row[5:]):
                raise ValueError("incompatible or duplicate programs partition")
            found = True
        elif off < START + SIZE and off + length > START:
            raise ValueError(f"programs region overlaps {name}")
    if found:
        return text
    return text.rstrip() + "\n# Opt-in source store, explicit programs init/mount only.\nprograms, data, 0x42, 0xC00000, 0x100000,\n"


def prepare(input_sdk, output_sdk, base_csv, output_csv):
    paths = [p.resolve() for p in (input_sdk, output_sdk, base_csv, output_csv)]
    if len(set(paths)) != 4:
        raise ValueError("input and output paths must all be separate")
    config = input_sdk.read_text()
    if 'CONFIG_IDF_TARGET="esp32p4"' not in config or 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"' not in config:
        raise ValueError("requires a Tab5/ESP32-P4 16 MiB config")
    for line in config.splitlines():
        if line.startswith("CONFIG_PARTITION_TABLE_OFFSET=") and int(line.split("=", 1)[1], 0) != 0x8000:
            raise ValueError("programs adapter requires the existing 0x8000 partition table")
    table = table_with_programs(base_csv.read_text())
    changes = {
        "CONFIG_PARTITION_TABLE_CUSTOM": "y",
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": json.dumps(str(output_csv.resolve())),
        "CONFIG_PARTITION_TABLE_FILENAME": json.dumps(str(output_csv.resolve())),
        "CONFIG_PARTITION_TABLE_SINGLE_APP": None,
        "CONFIG_PARTITION_TABLE_TWO_OTA": None,
    }
    kept = [line for line in config.splitlines() if not any(
        line.startswith(key + "=") or line == f"# {key} is not set" for key in changes)]
    kept += [f"{key}={value}" if value is not None else f"# {key} is not set" for key, value in changes.items()]
    output_sdk.parent.mkdir(parents=True, exist_ok=True)
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    output_csv.write_text(table)
    output_sdk.write_text("\n".join(kept) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_sdk", type=Path)
    parser.add_argument("output_sdk", type=Path)
    parser.add_argument("--base-table", type=Path, default=ROOT / "partitions_tab5.csv")
    parser.add_argument("--table-out", type=Path, required=True)
    args = parser.parse_args()
    try:
        prepare(args.input_sdk, args.output_sdk, args.base_table, args.table_out)
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    print(f"Prepared opt-in config: {args.output_sdk}")
    print(f"Prepared partition table: {args.table_out}")


if __name__ == "__main__":
    main()
