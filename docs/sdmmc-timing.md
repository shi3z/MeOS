# Native SDMMC stage timing

Wi-Fi's `bus_wait_us` includes both controller-lock contention and the delay
before the waiting task runs again. Native SD timings help distinguish DMA,
cache/copy work and scheduling effects before changing transfer sizing or lock
scope. The measurements are wall time, including preemption, rather than CPU
usage or a hardware-only transfer duration.

`sd stats` reports separate read/write buckets for transfers of up to 1, 8, 32
and 128 sectors. Larger requests are already split by the driver. Each bucket
contains calls, sectors, errors, cumulative milliseconds and peak microseconds:

- `wait`: before controller-lock acquisition to its return
- `prepare`: staging writes, cache/descriptor preparation, idle/FIFO waits and
  controller setup, up to command launch
- `dma`: command launch through observation of completion/error, including any
  time the polling task is preempted
- `finish`: DMA stop, status cleanup, write-busy wait or read-cache invalidation
  and copying into the caller's buffer
- `hold`: lock acquisition through transfer completion, before recording counters
  and unlocking

`max_polls` is a loop-iteration count, not elapsed microseconds. Peak stage
values may come from different requests and must not be added together.
Cumulative values are rounded down to milliseconds for console output. Counters
are bounded by their integer representation and should be reset between trials.

`sd stats reset` resets diagnostics only. Both commands are permitted while SD
is the active root and perform no card probe, media write or transfer. The SPI
transport reports these native diagnostics as unavailable. Existing root-owner
guards still prohibit raw probe, FAT mount/unmount and raw benchmarks.

## Paired SD/network workload

With an idle MeOS board and the existing PAK/font assets:

```sh
python3 tools/sdroot/wifi_coexistence_device.py /dev/cu.usbmodemXXXX \
  --reset --settle-seconds 60 --pps 100 --reads 1 \
  --diagnostics --sd-diagnostics \
  --log /tmp/meos-sd-stages.log --report /tmp/meos-sd-stages.json
```

The harness records native counters after each quiet CRC read and after each
network/graphics trial. Diagnostic resets/snapshots occur outside the traffic
phases. Complete read/write bucket receipts are required; missing or truncated
rows and any native transfer errors fail the measurement. Existing packet,
CRC, host-clock and memory checks remain active. On macOS, prefix the command
with `caffeinate -i` to prevent idle sleep only for that process's duration.
Reset discards unsaved RAM; raw transcripts may contain network identity.

## Validation boundary

Host root-owner checks cover diagnostic access/reset while mounted without
probing or mounting the card. Python tests reject incomplete/duplicate class
receipts. Full host tests, clean-source simulator replay and a Tab5 build pass.
The simulator does not emulate the controller or these physical timings.

## Tab5 measurements, 2026-10-25

[The device receipt](../tools/sdroot/sdmmc-stage-device-20261025.json) contains
complete bucket counters, packet counts, clock checks and private-transcript
hashes. The instrumented firmware is `1358fd6`, SHA-256
`488e88b9ac0e7b4cf53ffed5231bb44a8460af88460b9bc220f5988e07af466a`,
1,773,936 bytes. The harness ran from `c9e545c` against a persistent native
SD root, 4-bit at 20 MHz, on an ESP32-P4 revision 1.3 Tab5.

Before selecting a measurement image, fresh boot output confirmed MeOS
`db56a6c-dirty` in factory at `0x10000`, the A/B partition map and active SD
root. The existing OTA1 image at `0x7f0000` matched the profiling binary by
`esptool verify_flash`; no new app write was needed. Only boot-selection data
was changed for this session. Afterwards, empty/default factory selection was
restored and a fresh boot confirmed the original factory version, partition
map and active native SD root. The serial connection was released.

Port availability alone does not establish firmware or partition identity.
Check both before any future flash; the workload guard does not validate a
flash operation's target.

Each process waited 60 seconds for background services. Both read the same
18,689,235-byte PAK with CRC32 `74e3b084`. Network trials also ran a 24 px BIZ UD
font benchmark and unsaved Write size/layout changes. Traffic used a 1,400-byte
payload, with the requested average rate shown below. Actual packet counts
are retained in the receipt; rates are pacing targets, not wire-rate claims.

| Trial | PAK seconds | KiB/s | 1-sector hold, s | 128-sector hold, s | Peak SD hold, ms | Peak Wi-Fi bus wait, ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Quiet, first process | 38.846 | 469.8 | 27.607 | 3.128 | 18.978 | — |
| Quiet, second process | 38.845 | 469.8 | 27.651 | 3.140 | 18.406 | — |
| 25 pps, single packets | 39.016 | 467.8 | 29.107 | 4.463 | 88.683 | 82.243 |
| 100 pps, single packets | 40.875 | 446.5 | 28.995 | 4.385 | 17.669 | 68.104 |
| 100 pps, bursts of 12 | 41.146 | 443.6 | 29.168 | 4.380 | 60.434 | 60.091 |

**Counter scopes differ:** quiet counters cover one PAK CRC; network counters
cover the combined PAK, font and Write trial. They cannot be subtracted to
estimate the effect of traffic on PAK alone. Every request in the 128-sector
bucket in these runs was a full 64 KiB request. Peak SD hold is the maximum
across read buckets; it need not coincide with peak Wi-Fi wait.

The first quiet CRC made 28,886 single-sector requests, eight requests in the
up-to-eight-sector bucket and 286 full 128-sector requests. Single-sector
requests accounted for 27.607 of 30.744 seconds of total SD lock occupancy
(89.8%). Their cumulative preparation/DMA/finish times were
0.877/25.959/0.724 seconds; the corresponding bulk times were
0.010/2.239/0.877 seconds. The second quiet sample reproduced this distribution.
The full read issued 65,556 physical sectors for an 18.7 MB payload, so request
provenance and repeated small reads are the first throughput investigation
target. These counters do not yet identify the filesystem call sites.

The 25 pps trial included a 128-sector peak DMA interval of 75.168 ms and lock
occupancy of 88.683 ms. The burst trial instead had a single-sector peak DMA
interval of 60.359 ms and occupancy of 60.434 ms. Wall-time intervals include
scheduling, so this is evidence to investigate long shared-lock occupancy;
it does not establish card wire time or identify the cause of either outlier.
`max_polls` is not a CPU-time measurement, and independent stage maxima must
not be summed.

All five PAK CRCs matched, all native SD errors and Wi-Fi transport/bad-frame
errors were zero, and there were no invalid ICMP replies or device reply
failures. The 25 pps idle phase received 69/70 replies (1.429% loss); every
other phase received all sent replies, including all loaded PAK/font/Write
phases. Both processes passed the existing 5% per-phase loss limit. Wi-Fi
partial RX frames were observed and handled without bad frames or I/O errors.
Both clock checks passed; after trimming, PSRAM free changed from 23,135 to
23,119 KiB, within the existing 32 KiB allowance, with font and SD caches empty.

No native write calls were observed in the measured windows. This qualifies
read/graphics/network coexistence, not write performance, boot speed or power
loss recovery. There are two quiet samples and one sample per loaded setting;
the tails need repeated trials before choosing a scheduling/lock change.
Instrumentation overhead has not been calibrated. This PR adds diagnostics
and a measured baseline; it makes no throughput or latency improvement claim.
