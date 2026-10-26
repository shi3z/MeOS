#!/usr/bin/env python3
from pathlib import Path
import importlib.util
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare", ROOT / "tools/basic/prepare_programs_config.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix="meos-program-config-") as temporary:
    work = Path(temporary)
    sdk, base, out, table = (work / name for name in ("sdk", "base.csv", "output-sdk", "output.csv"))
    sdk.write_text('CONFIG_IDF_TARGET="esp32p4"\nCONFIG_ESPTOOLPY_FLASHSIZE="16MB"\nCONFIG_PARTITION_TABLE_CUSTOM_FILENAME="old.csv"\nCONFIG_PARTITION_TABLE_OFFSET=0x8000\n')
    original = (ROOT / "partitions_tab5.csv").read_text()
    base.write_text(original)
    config = sdk.read_bytes()
    module.prepare(sdk, out, base, table)
    assert sdk.read_bytes() == config and base.read_text() == original
    assert table.read_text().startswith(original.rstrip()) and "0xC00000, 0x100000" in table.read_text()
    assert str(table.resolve()) in out.read_text()
    assert module.table_with_programs(table.read_text()) == table.read_text()
    for text in (original+'\nother,data,0x43,0xC00000,4096,\n',
                 original+'\nprograms,data,0x42,0xC00000,0x100000,encrypted\n',
                 original+'\nprograms,data,0x42,0xC00000,0x200000,\n'):
        try:
            module.table_with_programs(text)
        except ValueError:
            pass
        else:
            raise AssertionError("unsafe layout accepted")
    try:
        module.prepare(sdk, sdk, base, table)
    except ValueError:
        pass
    else:
        raise AssertionError("input overwrite allowed")
print("opt-in program partition/config preparation tests PASS")
