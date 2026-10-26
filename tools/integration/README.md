# masanork integration branch

`main` on masanork/MeOS follows shi3z/MeOS. `masanork` collects the development
features; use feature branches for focused upstream pull requests. Merge upstream
updates into `main`, then merge `main` into `masanork`. Do not reset a working
checkout to synchronize it: the original development checkout still has pending
files and two local commits beyond upstream.

## Imported feature tips

The initial integration starts from upstream `a6a9f234e509737f9fe84fe13468a8ec78b60fc9`
and the two existing Tab5 touch/hardware commits. Every tip below is an ancestor
of the integration commit. This table pins the imported revisions; subsequent
feature-branch updates need another merge.

| Branch | Imported revision |
| --- | --- |
| `pr/app-common` | `87189cff98230e94ee17d01f97b435483e99a975` |
| `pr/app-simulator` | `95b084c26b135d5418b72b8f905e4a952ce66f75` |
| `pr/basic` | `fbb8bab9e460af9827e622d170b5cf4719c698fe` |
| `pr/doom` | `a64d96bf1013d22b12f382fd2ee4317fc3758302` |
| `pr/kernel-foundations` | `ca8ada6eb8c8d159b45e1e6c9ef615ea5d38646e` |
| `pr/peripheral-reservations` | `7fa284013e937e0363471f0bb7ead4d204885ecb` |
| `pr/scheme-editor` | `912f8f8c32a26639a2994169792503337a1ffd73` |
| `pr/tab5-audio` | `bf93788541d94d9ce8abea21ee10a4f43e9cb9c3` |
| `pr/tab5-bitmap-resize` | `ee8fef770177d27c41ee0aa98179a4d7b35f3809` |
| `pr/tab5-hardware-diagnostics` | `5d82fc19e99a7b52d72826cb577c269fdd41fe9a` |
| `pr/tab5-i2c-recovery` | `0ba17604cb38ff56a6184685338fff37d65e8fb7` |
| `pr/tab5-microphone` | `97c4da646b9386cf40a9933f64629aaef3e5b7b3` |
| `pr/tab5-multitouch` | `09314e9790fbccf54f5e5fe34c0af7a8744ab142` |
| `pr/tab5-psram-bss` | `f1a7335da2e032fa3ac8ec4bdaae97b10eb7dcc5` |
| `pr/tab5-sd-write` | `8cf6338690514de0207461bc5240b53e1aedb18f` |
| `pr/tab5-st712x` | `b7e9da870cd063030987c3309aa4bd372ec1f2c7` |
| `pr/task-stack-diagnostics` | `0ddaadaa0238c68b01c47e20eca71bbcef7b0071` |
| `pr/vfat-mp3-player` | `89f53eff40ac578a8103a18037798a1794b626eb` |

## Shared changes and conflict resolutions

- Preserve the foundation layer's priority inheritance, retryable cleanup,
  microphone ownership checks, I2C timeout/error diagnostics, watchdog reservation,
  PSRAM bounds and selectable host compiler when merging earlier hardware branches.
- Combine BASIC, Scheme, writing editor and Doom commands, includes and build
  definitions rather than replacing one application's shell integration with another.
- Import the pending `/data/bas` and `/data/scm` hierarchy and custom directories,
  legacy record reading, updated storage fault tests and `data` command alias.
- Combine pending ST712x fast report reads with foundation I2C timeout diagnostics.
  The fast path restores normal timing under the bus lock, including on failure.
  Host tests cover fast ACK/NACK/timeout, slow fallback and subsequent normal timing.
- Keep the newer 767-byte Tab5 VFAT name bound, MP3 player, writable SD support,
  scheduler floating-point context, source export and CI fixes from the feature
  branches. The original dirty checkout has older copies of these files.
- Carry the remaining shell/WASM filename bounds checks and update storage
  diagnostics to reflect explicit SD mounts, preserving RTC/microphone diagnostics.

The original standalone `tools/sd/vfat_vfs_test.c` and its older fixture family
are superseded by the merged SD/player UTF-8, path-boundary and real-VFS
read/write/remount suites. The older original checkout is preserved for comparison.
`experiment/quake` is included as an opt-in engine in the subsequent SD-root
integration; it remains experimental and does not ship PAK files. No default branch setting, upstream ref or original working file
is changed by this integration.

## Combined image size and Linux CI fixes

The initial combined Doom image exceeded the unchanged 0x3e0000 app slot.
Doom builds now select ESP-IDF size optimization; the engine/adapter/MP3 retain
-O2 and WASM retains -O3. BIZ UD's existing glyph codec now searches more candidate
matches, saving 135,138 bytes with all 11,741 decoded glyphs and metrics unchanged.
The fixed partition map and font coverage are preserved. For a pre-existing Doom
sdkconfig, explicitly select `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` and disable the
DEBUG choice, or regenerate the ignored config from defaults before building.
The opt-in generated defaults do not silently replace an existing sdkconfig.

The BASIC storage fault test keeps its GCC warning fix. Dictionary candidate bounds
use standard C `memchr` instead of a POSIX-only `strnlen` declaration, and build
entrypoints re-execute Bash when invoked by a POSIX shell such as Linux dash, so
ESP-IDF can identify its installation. These issues were found in Linux CI.

## Verification

Run from the integration checkout:

```sh
python3 tools/fonts/check_bizud.py
sh tools/host_checks.sh
sh tools/common/host_test.sh
sh tools/basic/host_test.sh
sh tools/sim/check-clean.sh
python3 tools/doom/release_test.py
sh tools/doom/host_test.sh
sh tools/build.sh tab5 build
M5OS_WATCHDOG=1 sh tools/build.sh tab5 reconfigure build
sh tools/doom/build.sh --without-wad build
```

Host tests and simulator checks use sanitizers and fresh source export where
supported. Firmware builds compile/link the combined tree, with Doom optional;
the WAD-free configuration includes BASIC, Scheme, writing editor and MP3 player.
Build output is local and ignored. Hardware flashing, interactive coexistence,
flash-store provisioning/reboot/power-cut, microphone and watchdog reset behavior
require separate device qualification. No board was flashed or reset during this
integration. Existing device evidence on individual features does not qualify
the newly combined revision.

## Host acceptance while the board is occupied

Do not open the board's USB port, reset, flash, mount/modify its SD card or change
its partitions during the separate SD-boot work. This work does not qualify the
physical firmware. Run `sh tools/sim/handoff_test.sh` for same-process Scheme /
writing-editor handoff with IME, graph, file and cleanup assertions. Run
`sh tools/basic/host_test.sh` for BASIC floating-point, graphics/cancellation and
shared display-lease regressions.

With an existing local IWAD, `sh tools/doom/host_test.sh /path/to/doom1.wad
/path/to/frame.ppm` additionally renders 120 frames through the real Doom engine
and MeOS's video/libc port, and exercises its mixer on game data. The IWAD is read
from its existing private cache, never downloaded, committed or copied onto the
board by this test. This headless harness is not Doom's MeOS app lifecycle or a
same-process Doom -> Scheme handoff; that remains a device/integration gap.

The filesystem redesign belongs in its own feature branch and PR series, based on
`masanork`. Keep the host acceptance work independent. A useful proposed split is
(1) VFS contracts and compatibility tests, (2) storage/boot backend, then (3) language
source paths and user-facing migration. The concrete split should follow the
other session's design; no new filesystem layout or migration is implemented here.


## SD-root and optional Quake integration

Starting from `masanork` revision `fb1accf`, the combined branch merges
`experiment/sd-root` at `d0ace23` and `experiment/quake` at `7d72f30`. The original
source checkout remains untouched. Keep the newer I2C fast/timeout tests, selectable
host compiler, UTF-8 VFAT paths, all language/editor commands and both optional
engine build definitions when resolving the older SD-root branch's conflicts.

Additional integration behavior:

- A valid littlefs SD store boots the persistent M5FS root without an 8 MiB RAM
  disk. Ordinary VFAT cards still use the RAM-root plus explicit FAT mount path.
- Active SD roots reject raw probe/FAT mount/benchmark commands. Explicit SD-root
  formatting also rejects a mounted FAT filesystem. Never auto-format at boot.
- Restore the Flash-backed Wasm store on persistent roots. Identical cached app
  files are reused instead of truncated/re-written at boot. A read/close failure
  is not treated as an identical cache.
- Each sparse COW bank has a 12 MiB logical capacity, enough for a full 8 MiB
  changed-sector snapshot plus transaction headroom. This is storage capacity;
  the in-RAM sector index remains 64 KiB. Existing shorter log files remain readable.
- The foundational host matrix now runs COW, actual littlefs adapter, SD/FAT
  exclusion and persistent Wasm cache tests. The Quake workflow separately checks
  its optional engine build and stack reports. Doom and Quake select alternative
  firmware images; they are not linked together, and no proprietary WAD/PAK ships.

The existing SD-root receipt describes the smaller trial firmware. It proves
format/boot/reset/100 saves/collection and 7.94 MiB recovery for that trial, not
all features on this combined image. Combined host/build and device evidence is
recorded separately below. Physical power-cut qualification remains open. BASIC, Scheme, writing and text-editor saves now use whole-buffer replacement
on transactional M5FS. See `docs/atomic-editor-save.md` for failure and fallback semantics. The outer littlefs namespace is
not yet a general `/sd` asset mount; FAT cards remain the existing game/media path.

### Combined verification (2026-10-24)

Local host suites, clean-source simulator and unchanged-glyph checks passed. The
combined default, WAD-free Doom and Quake images compile/link within the unchanged
app partition; Quake's 1,048 stack-frame reports pass. The imported engine-specific
play receipts remain separate from combined-image play qualification.

Default firmware source `cd95cc15e6ee` was application-only flashed and verified on
the Tab5. It booted the already formatted SD root, preserved `qualification-099`,
restored `snake.wasm` and `shooter.wasm`, ran BASIC (`10 PRINT 2+3` -> `5`), saved
`/home/integration.bas`, and loaded/ran that file after another reset. `sd probe`
was rejected while the SD root was active. The initial combined-image attempt had
fallen back to RAM after an early SD probe failure; a bounded three-attempt probe
and failure diagnostics were added, with host coverage of success after two
failures and complete exhaustion. Subsequent combined boots used SD successfully;
the original transient's hardware cause is not established.

The sanitized receipt is `tools/integration/device-results-20261024.json`. A settled
combined sample reported 28,672 KiB PSRAM heap, 731 KiB used / 27,940 KiB free; this
is a different workload from the isolated SD-root 7.94 MiB comparison. Raw serial
logs remain private because background network logs may contain identity/link data.
No card was reformatted during this combined qualification. Physical power removal,
manual Scheme/writing-editor use, sound/camera input and optional-engine interactive
coexistence on this exact image remain unqualified.


## Atomic editor save integration

`docs/atomic-editor-save.md` documents the whole-buffer VFS path now used by BASIC,
Scheme, the Japanese writing editor and `edit` (including BASIC EDIT). Transactional
SD-root saves have one commit; readonly/permission/space/IO errors never trigger a
streaming retry. Other providers retain prior behavior. The optional backend hint
reserves room for the whole save before truncation and can trigger collection first.

Host coverage includes 987 real M5FS/VFS cut points (16/32 KiB, empty and new files),
rollback and uncertain final sync, pin cleanup, readonly/write-only permissions,
legacy short writes/close errors and app buffers retained on failure. Simulator
source exports now include the shared streaming helper. Physical power-cut and
manual keyboard/display qualification remain separate.
