# Tab5 SD-root experiment (2026-10-24)

## Scope and layout

Flash retains the bootloader and kernel. Card-wide littlefs v2.11.3 contains a
read-only `base.img` plus `delta-a.log` / `delta-b.log`. M5FS presents an 8 MiB
logical root; the base stores only initialized metadata, with zero-filled virtual
sectors past EOF. Base writes are restricted to explicit initialization. In the isolated trial each log
had a 256 KiB logical capacity and zero-filled reads beyond its current EOF;
truncate+sync implements reset. File synchronization precedes publication.

littlefs is vendored unmodified at commit
`6cb4e86540eca0d9ba62500a298385c9d863c8be` (BSD-3-Clause). Static 512-byte caches
avoid newlib allocation and logging. The SD adapter uses 512-byte sector IO and
logical 4096-byte erase units (explicit 0xff writes); no raw NAND assumptions or
SD erase commands are used. The existing synchronous CMD24 transport waits for
ready and checks CMD13 for each write. Actual FTL power-loss behavior is unqualified.

The optional COW sector index costs 64 KiB of PSRAM for this 8 MiB root. It is
rebuilt from validated committed records at mount/recovery, updated for live
transaction reads, discarded on uncertain errors and retained/rebuilt across bank
collection. This avoids replaying the whole log for every unchanged sector during
collection. No image-sized RAM disk is allocated on successful SD-root boot.

## Controls and failure handling

- `sdroot` reports card geometry, active state, epoch, bank and committed log length.
- `sdroot format EXPECTED_SECTORS` is explicitly destructive, requires exact current
  geometry and refuses an active/mounted SD root. No boot path automatically formats.
- `sdroot save TEXT` atomically replaces `/home/sd-boot.txt` as a qualification probe.
- Missing/invalid SD store falls back to the existing RAM root; before/after memory
  comparisons use the same firmware. Bootloader/partition/dictionary/app-store flash
  regions are not flashed by the test command; the pre-test 16 MiB flash backup is
  private at `/tmp/meos-sd-root-device/original-flash.bin`.

The base-image formatter can format a detached block device while another volume
is mounted; it refuses the currently mounted block-device object. Callers must not
pass an alias of a mounted device. This allows preparation from the RAM-root shell.

The isolated trial skipped Wasm import on SD roots. The combined `masanork`
integration restores it and reuses identical cached apps, and raises each sparse
log capacity to 12 MiB without increasing the 64 KiB RAM index. Editor
whole-buffer saves are now connected to the transactional path; see
`atomic-editor-save.md`.
This trial firmware is built from the isolated branch, not the dirty primary tree;
it does not incorporate all concurrent Quake/Scheme/BASIC work.

## Build and host checks

```
sh tools/cow/host_test.sh
M5OS_BOARD=tab5 sh -c '. tools/env.sh; idf.py -B build-tab5-sdroot -DSDKCONFIG=sdkconfig.tab5 -DIDF_TARGET=esp32p4 build'
```

Host tests include 6,628 indexed/unindexed byte cuts, 210 real M5FS transaction
crashes, 17,506 collection byte cuts, transaction/concurrency/error tests and actual
littlefs-backed adapter initialization/remount/save/collection under ASan/UBSan.
The littlefs integration model is a 32 MiB simulated SD, not proof for a 15 GiB card.

## Device evidence

Logs and build/flash receipts are retained privately under
`/tmp/meos-sd-root-device/`. Summaries contain no Wi-Fi identity or credentials.

- Card identified through the Tab5 slot as high capacity, 30,867,456 sectors
  (15,072 MiB). Exact geometry was supplied to the explicit format command.
- Full original 16 MiB Flash read succeeded before flashing; only the application
  at 0x10000 was replaced. Bootloader and partition table remain the existing ones.
- `SDROOT FORMAT PASS` and `rc=0` established explicit littlefs/store initialization.
- Startup reported a persistent M5FS root and no RAM-disk allocation.
- `persisted-20261024` was saved atomically and read after an EN/USB reset;
  `second-boot-verified` was then saved successfully. Reset persistence is distinct
  from removing physical power during a write.
- Settled same-firmware heap samples (shell labels KB mean KiB):

| Backing | PSRAM total | PSRAM used | PSRAM free | Internal free |
|---|---:|---:|---:|---:|
| RAM root | 28,672 KiB | 8,762 KiB | 19,909 KiB | 83 KiB |
| SD root | 28,672 KiB | 634 KiB | 28,037 KiB | 83 KiB |

Net observed gain: **8,128 KiB (7.9375 MiB)**. This is the 8 MiB RAM disk minus
64 KiB index, subject to the displayed KiB truncation. Physical RAM and internal
SRAM capacity do not increase.

## Remaining qualification

Physical abrupt power removal, repeated-card/media-error campaigns, sustained
performance, card removal/rescue UI, full app-store and development-app integration,
on-disk indexing for larger roots, and bounds on collection latency remain open.
The outer littlefs namespace is not yet exposed as a general VFS mount. On-disk
formats are experimental; keep the recovery image and exact build receipt.

### Repeated saves and real bank collection

`save-cycles.log` records 100 successful atomic replacements, ending in
`qualification-099`, with no reported save failure. The selected epoch advanced
from 1/bank 0 to 2/bank 1, demonstrating real littlefs-backed collection and
continued writes. The post-cycle heap sample was 698 KiB PSRAM used / 27,973 KiB
free, a further 64 KiB dynamic allocation relative to the earlier sample. COW
collection allocates no heap and reuses the existing index; unrelated service
allocation was not instrumented, so this sample is not a device heap-leak verdict.
No physical power was removed during these operations.

### Final verified installation

The final application built from source commit `6fbd921` was written at 0x10000;
esptool verified its data hash. A first final-flash attempt failed during USB
connection, and an intervening boot showed a different Quake firmware. With the
port free, a retry succeeded. `final-verified-boot.log` then confirmed the new boot
message, active littlefs SD root, epoch 2/bank 1 and `qualification-099` after
reset. The settled memory sample returned to 634 KiB used / 28,037 KiB free.

The sanitized machine-readable receipt is
`tools/sdroot/device-results-20261024.json`. It contains exact app/backup hashes
and explicitly leaves physical power-cut and full concurrent-app integration
unqualified. No bootloader, partition table, eFuse or secure-boot setting was
changed. The device is left running the SD-root trial firmware.

### Bulk asset namespace

The Quake experiment adds `/assets`, backed by an isolated `/assets` directory
on the existing littlefs volume. The M5FS logical root remains 8 MiB. The driver
uses littlefs's `LFS_THREADSAFE` hooks with one kernel mutex shared by bulk files
and base/delta operations. Normalized relative paths are confined to the asset
directory. The root backing files are not exposed. No format is needed to add
bulk data to an already mounted native SD root.

## Integrated native SDMMC result

Commit `2948a6c35bfaba48c41d99ffdfd2a12e756392cb` combines the Quake branch,
the reviewed SD-root stack and the Tab5 native SDMMC transport. Only the app
partition was flashed; the bootloader, partition table and SD contents were not
replaced. The 4,038,720-byte app image has SHA-256
`953e36f0a38e9f1772ee9f4f8a3e96ee40ea9592ec3805ec9323e7c5e3169948`.

After a hardware reset, native slot 0 initialized at 4-bit / 20 MHz and mounted
the existing 30,867,456-sector littlefs/COW root. Boot printed `RAM disk not
allocated`; settled PSRAM was 159 KiB used / 27,272 KiB free. By comparison, the
same integrated image's RAM-root fallback used 8,287 KiB, an observed 8,128 KiB
difference. `/home/sd-boot.txt` still contained `qualification-099`, and
`/assets/quake/id1/pak0.pak` still had size 18,689,235 bytes.

Wi-Fi initialized concurrently on SDMMC slot 1 at 4-bit / 20 MHz and obtained an
IPv4 address while the SD root remained active. A complete PAK reread returned
CRC32 `74e3b084` in 77.315 seconds, or 236.1 KiB/s through littlefs/VFS. The older
software-SPI/FAT result was 74.1 KiB/s for sequential reads, about 3.2x slower,
but this is not a controlled transport-only comparison: the cards and filesystems
differ. The current littlefs adapter issues 512-byte sector operations, so this
measurement is far below the native bus ceiling and identifies batching as the
next performance target. No media writes were performed in this validation.
