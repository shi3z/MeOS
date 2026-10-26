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

## Bounded asset-read cache (2026-10-25)

The `/assets` path now coalesces sequential littlefs reads into two 64 KiB
read-ahead windows and retains 2,048 four-way 512-byte sectors for metadata and
CTZ traversal. The native SDMMC driver accepts the resulting 128-sector request
as one DMA command. Writes and erases invalidate both caches. Only the task
performing an asset read enables this path, so COW recovery and unrelated
littlefs operations retain their normal behavior.
Replacement slots are invalidated before issuing a physical read. If a failed
DMA transfer has already changed part of the buffer, the old block/sector tag
cannot expose those bytes as a cache hit; a later read must fetch the data again.

The cache is an optional 1,170 KiB PSRAM allocation, separate from buffers that
are required to mount the root. If it cannot be allocated, `/assets` continues
with direct I/O. `sdroot io-reset` clears the cache and counters; `sdroot` reports
logical reads, adjacency, hits/misses and physical requests for diagnosis.
`mem trim` waits for the current bounded asset read, releases the cache and arms
one lazy allocation attempt for the next read. If PSRAM is still constrained,
that read and later reads stay on direct I/O instead of retrying every chunk;
another `mem trim` or `/assets` remount rearms a single attempt. Write, Scheme and
the outline renderer also release the cache after a failed PSRAM allocation
before retrying their required allocation.

On the same Tab5 and card, a cold Write layout of a temporary document containing
900 distinct CJK characters fell from 9.947 s immediately before caching to
3.487 s. The passing cached run made 12,406 logical littlefs reads but only 1,865
physical SD reads; the 2,048-sector cache recorded 8,157 hits and 1,762 misses.
The temporary document was removed. A 24 px FreeType run and Write's 24/28/32 px
switching both reclaimed their application memory after exit. This qualifies one
card and workload, not hot removal, concurrent network saturation, power-cut
behavior or other cards.

`sh tools/sdroot/host_test.sh` also checks 64 trim/recreate cycles, 32 injected
allocation failures with an open asset handle, the one-attempt retry policy,
write invalidation, and partial failed refills of both cache types. These use
the real littlefs/COW/M5FS adapter on a simulated card under ASan/UBSan.

For an idle MeOS Tab5 with both font assets provisioned, the following read-only
device harness repeatedly releases the SD cache, renders 20/24/28/32 px samples,
opens Write, switches size/layout, and checks PSRAM after every exit. The log
path must not already exist. The final state has both optional caches released.

```sh
python3 tools/sdroot/cache_lifecycle_device.py /dev/cu.usbmodemXXXX \
  --cycles 20 --log /tmp/meos-cache-lifecycle.log
```

Adding `--boot-contention` explicitly resets the board and additionally requires
at least one cache release while the background IME reader is alive, followed
by a successful dictionary-load receipt. It rejects a non-MeOS boot before
sending shell commands. Dictionary-load overlap is not a network-throughput or
power-loss test. The harness leaves 50 ms between boot-time command pairs so
the higher-priority shell cannot starve the lower-priority IME reader.
When background services still change their startup allocations, add
`--settle-seconds 60` to wait on the same serial connection after the verified
IME overlap and before sampling the memory baseline. This keeps the original
32 KiB drift limit. The [aligned-sector cache comparison](sd-sector-cache.md)
records an initial unsettled failure and the passing settled rerun.

On physical Tab5 firmware `65eaf0c`, this test completed 628 cache releases
while the 5,228 KiB dictionary loaded successfully (loader stack guard OK),
then passed all 20 font/Write cycles. Each cycle returned to 21,949 KiB free
PSRAM with the 1,170 KiB cache resident; the final trim returned to 23,119 KiB,
within 16 KiB of the initial trimmed baseline. Font loads took 598–608 ms and
warm draws 5.5–10.8 ms across 20/24/28/32 px. The 900-CJK long-Write workload
also passed at 4.245 s. See `tools/sdroot/cache-lifecycle-device-20261025.json`.
Partial-transfer errors are still injected on the host rather than forced on
the physical card; removal, power cuts and network saturation remain untested.

## Read-only SD and Wi-Fi coexistence

`crc32 FILE` streams a regular file through VFS with a fixed 4 KiB buffer and
prints its IEEE CRC32, observed byte count and elapsed milliseconds. It opens
only for reading and emits the digest only after successful reads and close.
Every 64 KiB it sleeps for one scheduling interval so lower-priority network
services can make progress. The digest is of bytes observed during the read;
it does not promise a snapshot of a file being modified concurrently.

With the existing 18,689,235-byte PAK asset (CRC32 `74e3b084`) and both BIZ UD
assets present, this host tool compares two quiet reads with reads, font
rendering and unsaved Write sessions under 25/100 pps ICMP traffic:

```sh
python3 tools/sdroot/wifi_coexistence_device.py /dev/cu.usbmodemXXXX --reset \
  --pps 25 100 --reads 2 --log /tmp/meos-sd-wifi.log \
  --report /tmp/meos-sd-wifi.json
```

`--reset` discards unsaved RAM. Only the IPv4 address reported by the attached
Tab5 receives traffic; the tool checks ICMP payloads and checksums, PAK CRC32,
packet loss by workload and final PSRAM. The default loss limit is 5%. The new
log/report paths must not exist. No asset is uploaded or changed. The tool
requires datagram ICMP socket support and LAN reachability to the board.
These bounded rates test coexistence, not maximum network throughput.
When background services such as Tailscale still hold temporary startup buffers,
add `--settle-seconds 60` to keep serial open while they settle before the quiet
reads and memory baseline. Reopening USB serial can itself reset the board, so
waiting before opening the port does not establish a settled device. This option
does not change the packet-loss or memory-drift limits.

The physical comparison exposed RX starvation: the Wi-Fi daemon previously
ran below the foreground shell/renderer and polled every 100 ms. It now runs at
the same priority as the shell, sharing the existing 10 ms round-robin slices,
and polls every 10 ms while connected. Disconnected polling remains 100 ms.

On the same Tab5/card/AP, firmware `69077d5` versus `373285b` gave these results
for the requested 100 pps rate with 1,400-byte echo payloads:

| Workload | Packet loss before / after | RTT p95 before / after |
| --- | --- | --- |
| SD asset CRC read | 51.256% / 0% | 7,674 / 33 ms |
| Font load/render | 51.331% / 0% | 5,485 / 59 ms |
| Write size/layout changes | 69.677% / 1.667% | 2,935 / 94 ms |

At the requested 25 pps rate, all four phases (idle, SD, font and Write) had
zero packet loss after the fix. ICMP checksum/payload checks and all six
18,689,235-byte PAK CRC reads passed after the fix. The 100 pps SD reads took
40.705/40.732 s versus 38.571/38.440 s without added traffic; 24 px face open
took 682 ms versus 638 ms at 25 pps. Final trimmed PSRAM was 23,119 KiB versus
23,135 KiB initially, within the 32 KiB tolerance. Requested rates are pacing
limits; the evidence retains actual sent/received counts rather than claiming
that the host or link sustained the exact requested rate.

The before/after reports are `tools/sdroot/wifi-coexistence-{before,after}-20261025.json`.
They contain app and private serial-log digests. No media writes, power cuts,
hot removal or full Tailscale throughput were tested. The faster connected
polling and fair RX scheduling trade a little foreground execution time for
network responsiveness; this is not a maximum-SD-bandwidth benchmark.

## Background IME dictionary startup

The shell is usable before the large SD dictionary finishes loading; the built-in
mini dictionary remains available meanwhile. The background loader now reads
8 KiB directly into its final PSRAM allocation and sleeps after 64 KiB of progress,
instead of reading 2 KiB and sleeping after every 8 KiB. Higher-priority input and
Wi-Fi tasks retain normal preemption. The loader's 16 KiB stack and final dictionary
allocation do not grow. Registration occurs only after a complete read and a
successful close. A failed read/close or invalid dictionary releases the allocation.
An I/O receipt reports bytes, elapsed time including sleeps, and sleep count.

This reset-based harness measures shell readiness, dictionary readiness and serial
echo latency. Each cycle discards unsaved RAM and suppresses Home. It reads existing
assets and sends no save, upload or format command. Host readiness measurements
include EN reset, serial delivery and up to the probe interval before queued output
is read; `shell-ready` and I/O timings are measured on the board. Raw transcripts
may contain network identity and are kept privately.

```sh
python3 tools/ime/loader_device.py /dev/cu.usbmodemXXXX --cycles 3 \
  --log /tmp/meos-ime-loader.log --report /tmp/meos-ime-loader.json
```

On the same Tab5/card with a 5,354,161-byte dictionary, three resets of `373285b`
and three of `14ecd18` produced:

| Metric | Before | After |
| --- | ---: | ---: |
| Dictionary ready after EN reset, host median | 19.261 s | 13.275 s |
| Shell ready, device range | 3.422–3.590 s | 3.540–3.646 s |
| Echo RTT median, per-run range | 5.3–5.7 ms | 5.7–6.8 ms |
| Echo RTT maximum across probes | 13.0 ms | 12.8 ms |
| Loader stack watermark | 3,344 / 16,384 bytes | 3,360 / 16,384 bytes |

Dictionary readiness improved by 31%; shell startup did not improve. Post-change
loader I/O took 8.871–9.090 s including 81 voluntary sleeps. Host ASan/UBSan tests
compare all loaded bytes, check bounded scheduling and cover missing/oversized
files, allocation failure, short/zero/error reads, close failures and invalid blobs.
The clean-source simulator verifies editor/IME behavior using its built-in dictionary;
it does not execute the SD loader. A boot-time network-overlap attempt did not
establish overlap: the dictionary loaded before Wi-Fi reported connected IPv4.
Physical results qualify this card and workload, not hot removal, power loss or
maximum network throughput.

Additional postboot coexistence checks remain **FAIL**, independently of the
passing dictionary-startup comparison. Two shortened runs sampled temporary
Tailscale startup allocations and rejected free-PSRAM increases of 501/499 KiB;
the second also exceeded the 5% network-loss limit during the SD phase (5.279%).
Opening serial again had reset the board, so neither was a settled-device retry.
Keeping serial open for a 60 s settle brought final trimmed PSRAM within 16 KiB
of baseline (23,135 to 23,119 KiB free), passing the unchanged 32 KiB tolerance.
That run still failed: the 25 pps font phase lost 10/75 echoes (13.333%). Its
100 pps idle/SD/font phases had zero loss and Write lost 5/180 (2.778%).
All seven PAK CRC reads across the three attempts matched, and no echo payload
or checksum corruption was observed. The loader had exited before these
postboot workloads; the source of network variability remains unassigned.
The complete sanitized results, source/app identities and private-log digests
are in `tools/ime/loader-device-20261025.json`. These failures have not been
converted into passes or attributed to the IME change.
