"""Fetch the Doom 1.9 shareware IWAD into the ignored local cache, never the repo."""
import hashlib
import pathlib
import subprocess
import io
import zipfile

url = 'https://ftp.fu-berlin.de/pc/games/idgames/idstuff/doom/doom19s.zip'
expected = '1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771'
dest = pathlib.Path(__file__).resolve().parent / 'cache' / 'doom1.wad'
dest.parent.mkdir(parents=True, exist_ok=True)
if not dest.exists() or hashlib.sha256(dest.read_bytes()).hexdigest() != expected:
    temp = dest.parent / 'doom19s.part'
    subprocess.run(['curl', '--fail', '--location', '--max-time', '60', url, '--output', str(temp)], check=True)
    # The original installer is split into two DEICE volumes containing a self-extracting ZIP.
    with zipfile.ZipFile(temp) as outer:
        packed = outer.read('DOOMS_19.1') + outer.read('DOOMS_19.2')
    with zipfile.ZipFile(io.BytesIO(packed)) as inner:
        data = inner.read('DOOM1.WAD')
    if hashlib.sha256(data).hexdigest() != expected:
        temp.unlink()
        raise SystemExit('demo hash mismatch; refusing to use changed game data')
    dest.write_bytes(data)
    temp.unlink()
print(f'Local demo: {dest} (Doom 1.9 shareware; game data is not included in source)')
