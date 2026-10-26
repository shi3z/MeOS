# Aligned sector cache distribution

The native stage baseline in [SDMMC timing](sdmmc-timing.md) showed that an
18,689,235-byte PAK CRC made 28,886 single-sector reads. They consumed 89.8% of
SD lock occupancy despite the existing 1 MiB sector cache and two 64 KiB
read-ahead windows.

littlefs locates file blocks in `lfs_ctz_find` by reading four-byte skip-list
pointers near the start of a 4 KiB block. With the configured 512-byte read
alignment, these are single-sector reads at block-aligned LBAs. The previous
set calculation, `(sector * 2654435761u) & 511`, used only the low nine bits of
the product. Multiplication preserves the three zero alignment bits, so such
headers could use only 64 of 512 sets: effectively 256 sectors (128 KiB) of the
2,048-sector (1 MiB) cache.

The replacement takes the **high** nine bits of the wrapped 32-bit product.
Aligned LBAs can use every set, without adding a second allocation, changing
the cache layout or increasing its 1,170 KiB PSRAM footprint. Full sector tags
still validate hits; hash collisions use the existing four-way replacement
policy. Read-ahead sizing, the shared SDMMC lock, transfer clock, write/erase
invalidation and direct-I/O fallback are unchanged.

## Host regression

`sh tools/sdroot/host_test.sh` includes a 512 KiB working set consisting of
1,024 block-aligned headers. The first pass must read 1,024 physical sectors,
and the second must read none. The old calculation fails this test by
re-reading the headers. The new calculation passes, while checking every
returned byte against the simulated card.

The partial-refill failure test finds five actual colliding addresses for the
current calculation, fills a four-way set and injects an overwrite followed by
an I/O failure. A subsequent read of the evicted sector must fetch it again;
the old tag cannot expose the partially overwritten data. Existing 64-cycle
trim/recreate, 32 allocation-failure, open-handle continuity, write invalidation,
root-owner, COW and M5FS checks remain in the host suite under ASan/UBSan.

The full host suite, clean-source simulator replay and Tab5 build pass. The
simulator establishes application behavior, not SD timing. Physical before/after
results are recorded separately with firmware and transcript hashes.

## Physical Tab5 comparison, 2026-10-25

[The device receipt](../tools/sdroot/sector-cache-device-20261025.json) retains
all three network/CRC runs, the lifecycle result and the initial failed lifecycle
attempt. Raw serial transcripts remain private; their SHA-256 hashes are included.

Before firmware `1358fd6` and after firmware `0f2c419` used identical
`sdkconfig.tab5` files (SHA-256
`2aaa7f1e3484ca57df92e655093a677af58e392832f033d2cf418421ed03654f`):
360 MHz CPU, 200 MHz PSRAM, native four-bit SD at 20 MHz and the same card.
Both include the same native timing instrumentation. The only runtime source
change between these firmware versions is the sector-set calculation. The
after image is 1,773,952 bytes, 16 bytes larger, with the same cache allocation.

Each uniform run held serial open for a 60-second settle, made two quiet CRC
reads, then two CRC reads at each requested traffic rate with 1,400-byte payloads,
followed by the same 24 px font benchmark and unsaved Write size/layout changes.
`sdroot io-reset` cleared the asset caches before every CRC read. The payload
was the existing 18,689,235-byte PAK, CRC32 `74e3b084`.

| Workload | CRC before, seconds | CRC after, seconds | Font load before / after, ms |
| --- | ---: | ---: | ---: |
| Quiet, two reads | 38.775–38.846 | 11.414–11.463 | — |
| 25 pps, two reads | 38.915–39.056 | 11.309–11.325 | 644 / 245 |
| 100 pps, two reads | 40.895–40.973 | 10.916–10.982 | 684 / 253 |

The quiet means are 38.8105 and 11.4385 seconds: **3.393 times the speed** for
this CRC workload. Each quiet read went from 28,886 to **306 single-sector
requests**, a 98.94% reduction. Total native read requests fell from 29,180 to
600. The 286 full 128-sector requests and eight smaller multi-sector requests
were unchanged. Single-sector lock occupancy fell from about 27.6 seconds to
0.24–0.25 seconds; the bulk occupancy remained about 3.1 seconds. This supports
cache conflicts as the source of the repeated small physical reads.

Font first-draw times also fell from 451/479 ms to 318/339 ms at the respective
traffic rates, with the same glyph misses/hits and font I/O byte counts. These
are fixed benchmark samples, not a guarantee for arbitrary documents or fonts.

A separate after-firmware run at requested 100 pps in bursts of 12 completed
one quiet CRC in 11.475 seconds and a loaded CRC in 12.204 seconds. Across the
three network runs, all **14 PAK CRCs matched**, every observed ICMP reply was
received, and native SD errors, Wi-Fi transport/bad-frame errors, invalid replies
and device reply failures were zero. Each run returned from 23,135 to 23,119 KiB
free PSRAM after trimming, within the unchanged 32 KiB allowance. No native
write calls were observed in the measurement windows.

### Remaining latency and qualification boundaries

Wi-Fi's peak bus wait did **not** improve: at 25 pps it changed from 75.857 to
88.206 ms, and at 100 pps from 60.074 to 68.854 ms. Loaded peak SD lock occupancy
remained approximately 14–22 ms in these paired runs. Write-phase RTT p95 was
41.314 / 167.571 ms before/after at 25 pps and 126.864 / 122.277 ms at 100 pps.
The faster phases sent fewer packets, so maxima and percentiles have different
sample populations. These runs establish a throughput improvement, not a
tail-latency improvement. Scheduling and lock-wait tails remain a separate
investigation.

Quiet native counters cover one PAK CRC; uniform loaded counters cover two
CRCs plus font/Write, and burst loaded counters cover one CRC plus font/Write.
They must not be compared as if they were equal-length single-file samples.
Stage intervals include preemption. CRC times include checksum CPU work, copying
and voluntary scheduling, rather than measuring maximum raw SD throughput.
The comparison covers one card and two quiet samples per firmware; boot speed,
write performance, hot removal and power loss remain unqualified.

## Lifecycle validation and startup memory

The first after-firmware lifecycle attempt retained a **FAIL** result. Its memory
baseline was sampled before Tailscale startup completed. Two cycles passed; on
the third font run, free PSRAM was 21,497 KiB with the 1,170 KiB cache resident,
versus a trimmed baseline of 23,201 KiB. The 516 KiB increase from the preceding
cycle coincided with the Tailscale boot receipt. This did not establish a cache
leak; it showed that startup allocations were still changing during the test.

The harness now accepts `--settle-seconds 0..120` (default zero). With
`--boot-contention`, it still releases the cache during dictionary loading and
requires the successful load receipt **before** this optional wait. It then holds
the same serial connection open before taking the memory baseline; reopening
serial could reset the board. Memory and other failure limits are unchanged.

```sh
caffeinate -i python3 tools/sdroot/cache_lifecycle_device.py /dev/cu.usbmodemXXXX \
  --boot-contention --settle-seconds 60 --cycles 20 \
  --log /tmp/meos-sector-cache-lifecycle.log
```

The settled rerun passed **20/20** font/Write cycles at 20/24/28/32 px and
**201 verified cache releases during dictionary loading**. Every post-cycle
sample had 21,949 KiB free with the same 1,170 KiB cache resident. Final trimming
restored 23,119 KiB versus a 23,135 KiB baseline, within the original 32 KiB limit.
The failed attempt's transcript/output hashes and observed memory values remain
in the device receipt alongside the passing rerun.

Fresh preflight confirmed the original `db56a6c-dirty` factory image, A/B map
and active SD root before any selection/write. The old OTA1 image was verified
against the baseline binary, then OTA1 alone was replaced with the after image
and its flash hash verified. Factory, bootloader and partition table were
preserved. After all tests, default factory selection was restored, and a fresh
boot verified the original factory version and active native SD root. No SD
format or asset mutation command was issued; normal background services may
persist their own state.
