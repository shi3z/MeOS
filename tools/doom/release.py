#!/usr/bin/env python3
"""Inspect and package an IWAD-free Tab5 firmware candidate; never publishes."""
import argparse
import gzip
import hashlib
import io
import json
import pathlib
import shutil
import struct
import subprocess
import tarfile
import tempfile
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOCK = json.loads((ROOT / 'tools/doom/dependencies.json').read_text())
FLASH_FILES = {0x2000: 'bootloader/bootloader.bin', 0x8000: 'partition_table/partition-table.bin', 0x10000: 'm5os.bin'}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(*args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_symbols(data):
    """Read defined symbol names from a little-endian ELF32/RISC-V executable."""
    require(len(data) >= 52 and data[:7] == b'\x7fELF\x01\x01\x01', 'expected ELF32 little endian')
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    require(header[1] == 2 and header[2] == 243, 'expected RISC-V executable')
    offset, entsize, count = header[6], header[11], header[12]
    require(entsize == 40 and count > 0 and offset + count * entsize <= len(data), 'invalid ELF sections')
    sections = [struct.unpack_from('<IIIIIIIIII', data, offset + i * entsize) for i in range(count)]
    names = set()
    for sec in sections:
        if sec[1] != 2:  # SHT_SYMTAB
            continue
        require(sec[9] == 16 and sec[5] % 16 == 0 and sec[6] < count, 'invalid ELF symbols')
        strings = sections[sec[6]]
        require(strings[1] == 3 and strings[4] + strings[5] <= len(data), 'invalid ELF strings')
        require(sec[4] + sec[5] <= len(data), 'truncated ELF symbols')
        text = data[strings[4]:strings[4] + strings[5]]
        for pos in range(sec[4], sec[4] + sec[5], 16):
            name, _, _, _, _, defined = struct.unpack_from('<IIIBBH', data, pos)
            if not defined:
                continue
            require(name < len(text), 'invalid ELF symbol name')
            end = text.find(b'\0', name)
            require(end >= 0, 'unterminated ELF symbol')
            names.add(text[name:end].decode('ascii'))
    require(names, 'ELF has no defined symbol table')
    return names


def check_dependencies(idf):
    require(run('git', 'rev-parse', 'HEAD', cwd=idf) == LOCK['esp_idf']['commit'], 'ESP-IDF revision does not match dependencies.json')
    require(run('git', 'rev-parse', LOCK['esp_idf']['version'] + '^{commit}', cwd=idf) == LOCK['esp_idf']['commit'], 'ESP-IDF version tag differs from the pinned revision')
    require(not run('git', 'status', '--porcelain', '--untracked-files=no', cwd=idf), 'ESP-IDF has local modifications')


def verify(build):
    build = build.resolve()
    info = json.loads((build / 'doom-build.json').read_text())
    require(info.get('schema') == 1 and info.get('board') == 'tab5' and info.get('architecture') == 'riscv32', 'not a Tab5 Doom build')
    require(info.get('wad_embedding_allowed') is False, 'WAD embedding must be explicitly disabled')
    require(info.get('wad_embedded') is False and type(info.get('wad_bytes')) is int and info['wad_bytes'] == 0, 'build contains game data')
    cache = (build / 'CMakeCache.txt').read_text()
    require('M5OS_DOOM_EMBED_WAD:BOOL=OFF' in cache, 'CMake cache does not prohibit embedding')
    config = (build / 'config/sdkconfig.h').read_text()
    require('#define CONFIG_APP_REPRODUCIBLE_BUILD 1' in config, 'reproducible build config is disabled')
    require('#define CONFIG_IDF_TARGET_ESP32P4 1' in config, 'wrong chip config')
    commands = (build / 'compile_commands.json').read_text()
    require('M5OS_DOOM=1' in commands, 'Doom was not compiled')
    require('DOOM_EMBED_RAW_SIZE' not in commands and 'doom_wad.z.S' not in commands, 'WAD compilation input remains')
    names = elf_symbols((build / 'm5os.elf').read_bytes())
    require({'doom_main', 'DG_DrawFrame', 'doom_video_palette'} <= names, 'Doom engine/presentation missing')
    require(not any('doom_wad' in s for s in names), 'embedded WAD symbol remains in ELF')
    flash = json.loads((build / 'flasher_args.json').read_text())
    files = {int(offset, 0): name for offset, name in flash['flash_files'].items()}
    require(files == FLASH_FILES, 'unexpected flash offsets/files; refusing to package extra flash regions')
    hashes = {}
    for offset, name in FLASH_FILES.items():
        path = build / name
        require(path.is_file() and not path.is_symlink(), 'missing or symlinked flash file: ' + name)
        data = path.read_bytes()
        if offset != 0x8000:
            require(len(data) >= 24 and data[0] == 0xe9 and struct.unpack_from('<H', data, 12)[0] == 18, 'not an ESP32-P4 image: ' + name)
        else:
            require(data[:2] == b'\xaa\x50', 'not an ESP partition table')
        require(offset != 0x10000 or len(data) <= 0x3e0000, 'app exceeds the partition')
        hashes[name] = {'sha256': digest(path), 'bytes': len(data)}
    project = json.loads((build / 'project_description.json').read_text())
    require(project['target'] == 'esp32p4' and pathlib.Path(project['project_path']).resolve() == ROOT, 'wrong project')
    check_dependencies(project['idf_path'])
    compiler = run(project['c_compiler'], '--version').splitlines()[0]
    require(LOCK['compiler_release'] in compiler, 'compiler release differs from the pinned toolchain')
    revision = run('git', 'rev-parse', 'HEAD', cwd=ROOT)
    require(info['source_revision'] == revision, 'source revision changed; reconfigure and build again')
    require(LOCK['doomgeneric']['commit'] in (ROOT / 'third_party/doomgeneric/README.meos.md').read_text(), 'engine provenance differs from lock')
    return {
        'schema': 1, 'format': 'firmware', 'board': 'tab5', 'architecture': 'riscv32',
        'wad_embedded': False, 'source_revision': revision,
        'source_dirty': bool(run('git', 'status', '--porcelain', '--untracked-files=all', cwd=ROOT)),
        'dependencies': LOCK, 'compiler': compiler, 'flash_files': hashes,
        'elf_sha256': digest(build / 'm5os.elf'),
        'source_date_epoch': int(run('git', 'show', '-s', '--format=%ct', revision, cwd=ROOT)),
        'hardware_qualification': 'not asserted by this tool',
        'meos_license_file_present': any((ROOT / n).is_file() for n in ['LICENSE', 'LICENSE.md', 'LICENSE.txt']),
    }


def source_archive(dest, revision):
    raw = subprocess.check_output(['git', 'archive', '--format=tar', '--prefix=MeOS/', revision], cwd=ROOT)
    stream = io.BytesIO(raw)
    with tarfile.open(fileobj=stream, mode='a') as archive:
        require(not any(m.name.lower().endswith('.wad') for m in archive.getmembers()), 'tracked WAD in source archive')
        epoch = int(run('git', 'show', '-s', '--format=%ct', revision, cwd=ROOT))
        for name, value in [('SOURCE_REVISION', revision), ('SOURCE_DATE_EPOCH', str(epoch))]:
            require(not any(m.name == 'MeOS/' + name for m in archive.getmembers()), 'reserved generated source metadata was committed')
            text = (value + '\n').encode()
            entry = tarfile.TarInfo('MeOS/' + name)
            entry.size, entry.mode, entry.mtime = len(text), 0o644, epoch
            archive.addfile(entry, io.BytesIO(text))
    dest.write_bytes(gzip.compress(stream.getvalue(), mtime=0))


def package(build, output, development=False):
    report = verify(build)
    require(not report['source_dirty'] or development, 'working tree is dirty; commit release inputs first (or --development for a local test only)')
    require(output.suffix == '.zip', 'candidate output must end in .zip')
    require(not output.exists(), 'output already exists; refusing to overwrite')
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temp:
        stage = pathlib.Path(temp) / 'candidate'
        stage.mkdir()
        for name in FLASH_FILES.values():
            dest = stage / name
            dest.parent.mkdir(exist_ok=True)
            shutil.copyfile(build / name, dest)
        for src, dest in [('m5os.elf', 'm5os.elf'), ('config/sdkconfig.h', 'sdkconfig.h'), ('doom-build.json', 'doom-build.json')]:
            shutil.copyfile(build / src, stage / dest)
        shutil.copyfile(ROOT / 'tools/doom/RELEASING.md', stage / 'RELEASING.md')
        notices = stage / 'licenses'
        notices.mkdir()
        for folder in ['doomgeneric', 'wasm3', 'quirc', 'ubasic']:
            shutil.copyfile(ROOT / 'third_party' / folder / 'LICENSE', notices / (folder + '.txt'))
        shutil.copyfile(ROOT / 'third_party/fatfs/README.md', notices / 'fatfs-notice.md')
        shutil.copyfile(ROOT / 'third_party/fatfs/ff.h', notices / 'fatfs-ff.h')
        for name in ['LICENSE', 'LICENSE.md', 'LICENSE.txt']:
            if (ROOT / name).is_file():
                shutil.copyfile(ROOT / name, notices / ('MeOS-' + name))
        report['development_only'] = bool(development)
        if not report['source_dirty']:
            source_archive(stage / 'MeOS-source.tar.gz', report['source_revision'])
        (stage / 'manifest.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
        (stage / 'flash_args').write_text('--flash_mode dio --flash_freq 80m --flash_size 16MB\n' + '\n'.join(f'{offset:#x} {name}' for offset, name in FLASH_FILES.items()) + '\n')
        sums = [f'{digest(p)}  {p.relative_to(stage).as_posix()}' for p in sorted(stage.rglob('*')) if p.is_file()]
        (stage / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
        bundle = pathlib.Path(temp) / 'candidate.zip'
        timestamp = time.gmtime(max(report['source_date_epoch'], 315532800))[:6]
        with zipfile.ZipFile(bundle, 'w') as archive:
            for path in sorted(stage.rglob('*')):
                if not path.is_file():
                    continue
                entry = zipfile.ZipInfo(path.relative_to(stage).as_posix(), timestamp)
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, path.read_bytes())
        shutil.move(str(bundle), output)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['verify', 'package', 'check-dependencies'])
    parser.add_argument('--build-dir', type=pathlib.Path, default=ROOT / 'build-doom-release')
    parser.add_argument('--idf', type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path)
    parser.add_argument('--report', type=pathlib.Path)
    parser.add_argument('--development', action='store_true')
    args = parser.parse_args()
    try:
        if args.command == 'check-dependencies':
            require(args.idf is not None, '--idf is required')
            check_dependencies(args.idf)
            print('Pinned ESP-IDF source matches')
            return
        if args.command == 'package':
            require(args.output is not None, '--output is required')
            report = package(args.build_dir.resolve(), args.output.resolve(), args.development)
        else:
            report = verify(args.build_dir)
        result = json.dumps(report, indent=2, sort_keys=True) + '\n'
        if args.report:
            args.report.write_text(result)
        print(result, end='')
    except (ValueError, KeyError, OSError, struct.error, subprocess.CalledProcessError) as error:
        parser.exit(1, 'release check failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
