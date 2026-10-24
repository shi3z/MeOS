# SD littlefs/COW root (review slice 3)

Depends on [M5FS transactions](https://github.com/masanork/MeOS/pull/2), stacked on
[COW core](https://github.com/masanork/MeOS/pull/1). The review stack is anchored at
`fb1accf831114e63f84baf382eb3379e63ddf568`; the live `masanork` branch already has the
combined implementation. This slice extracts the SD-root layer from
`d10b54bb9a0a0fe54df26ca1f80e9efc26d95978`, excluding Quake and unrelated SDK changes.
It is for independent review, not a duplicate merge of SD-root into `masanork`.

## Layout and ownership

The kernel and bootloader stay in Flash. On Tab5, an existing card-wide littlefs
v2.11.3 volume supplies read-only `base.img` and two COW delta logs for an 8 MiB
logical M5FS root. The sparse base reads as zero beyond EOF; each log has a 12 MiB
logical capacity. A caller-owned 64 KiB PSRAM index avoids an image-sized RAM disk.
Only explicit initialization writes the base. The pinned unmodified littlefs
sources and BSD-3-Clause license are a separate vendoring commit.

Static 512-byte littlefs caches avoid newlib allocation/logging. The adapter uses
512-byte sector IO and logical 4096-byte erase units implemented as explicit 0xff
writes. Existing synchronous SD CMD24 transport waits for ready and checks CMD13;
real-controller/FTL durability still needs physical power-loss qualification.

Boot probes at most three times, mounts only an existing store and otherwise
uses the prior RAM-root fallback. It never formats a card. Boot imports installed
WASM apps from Flash; byte-identical existing files are read/compared and reused
rather than truncated at every boot. Read/close errors cannot establish equality,
and replacement propagates close errors. App-save atomicity is a later change.

Mount requests also reject mounted FAT or an already-open outer littlefs volume
before probing/resetting the SD bus. This protects direct API callers as well as
the shell paths. An active littlefs root excludes `sd probe`, FAT mount/unmount and SD benchmarks;
`sd status` remains available. Explicit SD-root formatting refuses active/mounted
roots and mounted FAT media. Mount selection and these ownership guards are kept
in the same PR to avoid exposing the live root through another filesystem path.
Legacy SD-root and non-Tab5 RAM/legacy root behavior remain covered separately.

## Explicit commands and compatibility

- `sdroot` reports state, card geometry, bank/epoch and committed length.
- `sdroot format EXPECTED_SECTORS` destroys/replaces the card-wide filesystem only
  after matching current geometry. This must never be used on a card containing
  data that should be kept; there is no boot-time or automatic format/repair.
- `sdroot save TEXT` is a grouped atomic qualification probe at
  `/home/sd-boot.txt`, available only with the SD root active.

This PR changes Tab5 root selection for a recognized store. It does not change
Flash partitions, VFS APIs, M5FS disk/name/extent limits, language source defaults,
legacy Flash `/data` records or general editor save implementations. BASIC help
and Scheme save status now describe persistence according to mounted storage.
FAT cards keep the existing RAM-root plus explicit FAT mount path. The outer
littlefs namespace is not exposed as a general asset mount: Doom/WAD, MP3 and
Quake/PAK placement on such a card is separate work. No game engine or game assets
are added by this slice.

## Verification and boundaries

`sh tools/sdroot/host_test.sh` uses ASan/UBSan with a simulated 32 MiB SD and the
actual vendored littlefs, COW and M5FS implementation. It checks explicit format,
wrong geometry, bounded probes, no-format mount failure, indexed remount/save/
collection and continued writes. Separate production-code tests check active-root
FAT/probe/benchmark exclusion and identical/short/error WASM-cache reads.

The boot-selection harness links production `kernel/boot/rootfs.c`, with stubbed
storage boundaries: valid SD avoids RAM allocation/formatting; missing/corrupt/IO
failures format only a new RAM device, never the SD; legacy mounts, RAM sizes and
allocation/format/VFS failures are checked on Tab5 and CoreS3 configurations.
`sh tools/host_checks.sh` includes both COW and SD-root suites. Normal/watchdog
and WAD-free Doom firmware builds are separate integration checks.

This review branch has not been flashed or run against a physical card. Historical
SD reboot/collection/BASIC receipts on the source integration branch identify other
exact firmware revisions and do not qualify this extracted build. Abrupt physical
power loss, arbitrary-instruction app termination, card removal/rescue behavior,
collection latency, write amplification and broad app coexistence remain open.
The disk format is experimental. No device session or private backup is included.
