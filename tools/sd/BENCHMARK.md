# Tab5 microSD benchmark

Run on a mounted read/write FAT card:

```
sd mount
sd bench                         # 128 x 1KiB files, then an 8MiB file
sd bench 16 1024 1                # short qualification run
sd bench 256 4096 16              # larger small-file and sequential workloads
sd unmount
```

Arguments are file count (1..512), small-file bytes (1..65536), large-file MiB
(1..64). Peak usage is files*small_bytes + large_MiB*1048576 plus FAT/directory
metadata. No formatting or repartitioning occurs. The command creates a fresh
`/sd/MEOSBENCH-...` directory; existing files/directories are never reused or
removed. Small files have VFAT long filenames. Successful runs remove all their
files and their directory. Errors, Ctrl-C or timeout retain artifacts for manual
inspection and print the directory. A media error may require explicit unmount
and reprobe before those artifacts can be removed.

Five phases report elapsed time, KiB/s and files/s: small-file create/write/close,
small-file open/read/verify/close, large write/close, large read/verify/close, and
small-file deletion. Large-file deletion and final directory removal are outside
those timing phases. A deterministic per-file/offset pattern checks every byte
and EOF; the command only prints PASS after both read phases and cleanup succeed.
Write phases report `verified=0` until their subsequent read phase verifies them.

This is end-to-end VFS performance on software SPI using CMD17/CMD24, with 4KiB
transfers and synchronous data/metadata flush on every VFS write. Pattern work,
file/directory operations, scheduling and occasional progress output are included.
It is not raw SD bus bandwidth or a claim about native SDMMC performance. There
is no OS page cache; card-internal caching, fragmentation and existing card usage
can still affect the result. Each phase is limited to 10 minutes and can be
aborted with Ctrl-C. Power loss during a write is not made atomic by this test.

To capture and validate a physical run (opening USB serial may reset the board):

```
python3 tools/sd/benchmark_device.py /dev/cu.usbmodem101 \
  --files 128 --small-bytes 1024 --large-mib 8 --output /tmp/sd-bench.log
```

The Python runner needs pyserial and firmware containing `sd bench`; it requires
an initially unmounted card, mounts read/write, records complete output plus JSON
metrics, unmounts and captures heap/task diagnostics. Host tests use generated
memory-backed images to verify the benchmark's data checks, read-only rejection,
argument limits, abort behavior and cleanup; their timing is not device evidence.

## Physical result: 2026-10-24

One SDSC card, 3,911,680 sectors (1,910 MiB), Tab5 ESP32-P4 at 360MHz.
Firmware source `3f80a4ab7799`, software SPI, 4KiB VFS writes with synchronization.
The same command first passed a 16-file/1MiB qualification run, then:

| Phase | Workload | Elapsed | Throughput |
|---|---|---:|---:|
| Create/write/close | 128 x 1KiB VFAT files | 46.450 s | 2.7 files/s |
| Open/read/verify/close | same files | 13.596 s | 9.4 files/s |
| Sequential write/close | 8MiB file | 241.393 s | 33.9 KiB/s |
| Sequential read/verify/close | 8MiB file | 110.527 s | 74.1 KiB/s |
| Delete small files | 128 files | 17.238 s | 7.4 files/s |

All bytes/EOF matched; cleanup and unmount succeeded in both runs. All task
stack guards remained OK; the coarse post-run heap totals matched (112KiB internal
and 8279KiB PSRAM used). The 16-file/1MiB run measured 4.8 file creates/s,
23.3 file reads/s, 33.1KiB/s sequential writes, 75.5KiB/s sequential reads.
Directory/name lookup and synchronization costs are visible in the small-file
results; this is an observation on this implementation/card, not a raw bus limit.
Aggregate metrics and firmware digest are in `device-benchmark-2026-10-24.json`.
The original app partition was backed up before testing; no partition table,
bootloader, dictionary, app store or existing card file was intentionally changed.
Power-failure behavior and other card models remain unqualified.
