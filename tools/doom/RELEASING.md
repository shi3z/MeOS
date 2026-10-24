# Tab5 Doom firmware candidates

This is a firmware-integrated application, not an installable WASM/M5EX app.
Do not publish a release yet: neither the upstream MeOS repository nor this fork
currently provides a root license. A public GitHub fork alone does not establish
permission to redistribute the firmware. Doomgeneric has GPL-2.0-or-later headers;
the license of the combined work, corresponding source and ESP-IDF components
must be resolved before distributing binaries. Do not assign a license to others'
code without their authorization.

## Build and inspect

Use a Git checkout, Python 3 and the ESP-IDF/toolchain pinned in
`tools/doom/dependencies.json`. Install that SDK's esp32p4 tools and retain its
v5.4.4 tag. Override the SDK path with `M5OS_IDF_P4` if necessary.

```
sh tools/doom/release_build.sh
python3 tools/doom/release.py package --output dist/MeOS-tab5-doom.zip
```

The public build uses `sdkconfig.doom-release` and `build-doom-release`.
WAD embedding is explicitly OFF even when the private cache contains a WAD.
The ordinary `tools/doom/build.sh` remains the private embedded-WAD build.
Never upload its firmware or the cache. The checker validates build settings,
Doom ELF symbols, absence of embedded-WAD symbols, target, SDK/compiler pins,
flash regions and image sizes. This is a check of our embedding mechanism,
not a general content scanner or a hardware qualification.

Packaging requires a clean committed checkout, includes its Git source archive
and generated source revision/time metadata, and refuses to overwrite a ZIP.
The manifest and SHA256SUMS identify the images. `--development` permits a local
dirty-tree experiment; its manifest marks it as development only and omits a
misleading HEAD-only source archive. Such a ZIP must not be published.
CI builds and checks, but never uploads firmware or creates a Release.
Reproducible-build settings remove timestamps and normalize compiler paths;
compare ELF/BIN hashes across independent builds before claiming reproducibility
on additional machines. ZIP identity across Python versions is not promised.

The generated source archive can be extracted and built with
`sh tools/doom/build.sh --without-wad build`; packaging/verification tools require
a Git checkout. SDK sources and tools are separate pinned dependencies.

## Before an actual GitHub Release

- Obtain an explicit MeOS redistribution license from the relevant authors and
  review compatibility with GPL-2.0-or-later and linked SDK/component licenses.
- Complete the dependency/notice inventory, including linked ESP-IDF components.
  The notices currently bundled are provisional, not a complete compliance claim.
  Provide corresponding source, local changes, build scripts and the required
  SDK source by the chosen licenses' distribution mechanism. An upstream link
  alone is not asserted to satisfy source-distribution obligations.
- Commit all intended inputs, build from that clean revision, inspect the ZIP,
  and record hardware acceptance separately. No WAD or game assets in the Release.
- Test this exact WAD-free image with a real FAT microSD card. Existing device
  testing used an embedded IWAD; SD-backed game startup is not yet qualified.
- After those checks, publish a tagged firmware Release in masanork/MeOS with
  source, notices, hashes, limitations and installation/recovery instructions.

Suggested upstream question (draft only; not sent):
“May we redistribute MeOS and modified Tab5 firmware, including a GPL-2.0-or-later
Doom engine? Could you clarify MeOS's license and the rights of contributors,
and add an explicit license file to the repository?”

## Installation and game data

Back up the device before flashing. After the above release gates are resolved,
unzip a verified candidate, verify SHA256SUMS, install the pinned SDK's esptool,
and run from the extracted directory:

```
python -m esptool --chip esp32p4 --port YOUR_PORT write_flash @flash_args
```

This overwrites bootloader, partition table and app; it does not erase the full
flash. Review partition changes and backup compatibility before upgrading.
Provide your own lawfully obtained compatible IWAD separately. With a FAT card
containing `DOOM1.WAD` at its root:

```
sd mount
doom /sd/DOOM1.WAD
```

The current SD reader is read-only and uses FAT 8.3 names; exFAT is unsupported.
WAD-free firmware has not yet been exercised with a physical SD card.

## Later: standalone native applications

The existing URL/QR installer targets WASM with a 512 KiB app slot and an 8 MiB
runtime memory ceiling. The M5EX builder currently emits Xtensa code, and its
loader has 256 KiB memory / 16 KiB stack limits and internal-SRAM execution
constraints. Doom needs the RISC-V Tab5 backend and a 32 KiB stack. A native SDK,
versioned OS ABI, RISC-V loader/relocation or XIP design, resource lifecycle and
installer support are separate work. Do not advertise this firmware as an
installable `.m5ex` or QR application.

## Local preparation evidence (2026-10-23)

Two independent build directories on the same macOS host produced identical
application BIN and ELF, bootloader BIN and partition table BIN using the pinned
SDK/toolchain. Application SHA256:
`c66bf5b374599ea7bdce92c9d602ced316c94127cf2e386b2a7249f99e49686c`.
This was a dirty development checkout, not a release source revision. Host checks
passed for Doom, touch, I2C/hardware and FAT SD parsing. The candidate ZIP's checksums
and absence of `.wad` entries were checked, and dirty release packaging was rejected.
GitHub Actions and SD-backed hardware startup have not been run for this candidate.
