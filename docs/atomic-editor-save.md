# Whole-buffer editor saves on the transactional SD root

BASIC SAVE, Scheme save-buffer, the Japanese writing editor and the regular text
editor now call `vfs_replace_file`. This includes the text editor launched by BASIC
EDIT. Buffers are borrowed during the call; no extra full-file copy is allocated.
The modified/saved flags change only after the whole operation returns success.

## Storage contract

For transactional M5FS, truncate/create, data writes, close, metadata flush and COW
commit run under one volume mutex. A successful call publishes exactly one
transaction. A write/space/metadata error before commit abandons the new version
and remounts committed state; an existing file retains its old contents and a new
file stays absent. The conservative metadata/data record hint permits collection
before starting a large save rather than running out of log space midway through it.

An error at the final durability barrier has an uncertain outcome: recovery can
select the old file OR the complete new file. The application still reports failure
and retains its buffer. There is no unsafe retry through ordinary open/write after
an atomic-handler error. Guarantees depend on the existing storage sync contract;
physical SD power-loss tolerance is not established by this change.

Any recovery invalidates all open M5FS file handles, as with the existing transaction
API; affected callers must reopen. This is durable whole-file replacement, not a
snapshot guarantee across multiple independent reads on an already-open descriptor.

Raw RAM M5FS volumes keep their prior flush behavior. VFAT, the host simulator and
other filesystems without a replace hook use the shared streaming writer; those
paths preserve prior short-write and close-error handling and do not acquire a new
atomicity guarantee. The existing `/data` store retains its own close/commit model.

## VFS integration

The optional provider `replace` callback is selected after path normalization and
normal mount matching. M5FS enforces filesystem-write permission before touching
the volume; write-only callers do not acquire an additional read requirement.
Callback errors, including ERR_NOSYS, are final. Only an absent callback selects
legacy streaming behavior. This prevents permission or IO failures being retried
through a destructive truncate.

A transient descriptor pins the mount during a provider callback. Interrupts are
restored before IO. Ordinary descriptor operations reject the transient pin, and
application cleanup can reclaim it after its owner terminates. Legacy streaming
saves use only their normal open descriptor, including when the table is nearly
full. The pending flag uses existing structure padding on the target.

`written` is optional. Atomic failures report no acknowledged bytes; legacy failures
may report partial writes. Success always includes close/commit completion.

## Verification

- Real VFS/M5FS tests cover one commit per save, write permission, read-only mounts,
  old-file rollback, new-file absence on failure, zero-byte replacement, short
  writes, both sync-error outcomes, descriptor cleanup and continued saves.
- The real transaction/replacement crash harness covers 987 sampled byte cuts,
  including 16 KiB and 32 KiB replacements and empty/new files. Every recovered
  sector matches the complete old or new image; recovery mounts the volume again.
- The existing 6,628 record cuts and 17,506 collection cuts remain in the COW suite.
- VFS dispatch tests cover mount pinning, owner termination cleanup, no retry after
  callback errors, partial streaming writes, close errors and the last free slot.
- BASIC and regular editor host error cases retain source state. Scheme IDE failure
  cases retain modified text. The clean-source SDL replay exercises Japanese saves
  and six repeated Scheme/writing-editor launches through the new shared interface.

Commands: `sh tools/host_checks.sh`, `sh tools/basic/host_test.sh`,
`sh tools/scheme/host_test.sh`, `sh tools/sim/check-clean.sh`.

The simulator exercises the host streaming adapter; the real VFS/M5FS fault tests
establish the transactional path. These evidence categories are intentionally
separate from physical keyboard/display interaction or actual power removal.

## Tab5 device qualification (2026-10-24)

The application built from `d94c7d8c9b114e7e279a584149774e4c48d7d8e7`
was written at `0x10000` and its Flash hash was verified. A complete 16 MiB
pre-test Flash backup was retained privately. The bootloader, partition table,
dictionary and existing SD filesystem were preserved; the card was not formatted.

Serial-driven device checks passed for BASIC new-file save and replacement,
Scheme save, UTF-8 Japanese writing-editor save, and regular text-editor save.
After reset, all four files matched their expected contents, and the BASIC
program loaded and printed both expected values. These are actual device/VFS
saves; serial injection does not qualify physical keyboard or IME interaction.

The settled heap sample showed 731 KiB PSRAM used and 27,940 KiB free, with
79 KiB internal RAM free and no reported allocation failures. No image-sized
RAM disk was allocated on the persistent SD-root boot.

The sanitized receipt is `tools/atomic-save/device-results-20261024.json`.
Raw serial/build/flash logs and the original Flash backup are private under
`/tmp/meos-atomic-device/`. The device boot/recovery wait exceeded an initial
60-second harness deadline; the corrected readiness probe sends an empty line
and allows 120 seconds. One observed readiness time was 66.8 seconds. Boot
latency needs separate profiling before claiming a performance improvement.

### Interrupted-save reset campaign: incomplete

A 2,058-byte regular-editor buffer was saved and read back exactly. The harness
then inserted a four-byte generation prefix, started saving and asserted EN reset
after a host delay of 0, 5 or 20 ms. Each of those three trials booted the SD root
and recovered the exact old file. These delays are host timing, not measured
positions inside an SD write or a COW commit.

The fourth trial, with a 50 ms host delay, reached the shell in 4.2 seconds but
**used the volatile RAM root**. All three SD probes returned `R1=0xff` before
littlefs could mount. Consequently the test file was absent from the RAM root;
this is not evidence that the file was deleted or corrupted on the SD card.
The fifth trial was not run. The card's contents after the fourth reset remain
unverified pending full power-off/recovery; the device was left in RAM fallback.
The receipt records this failure and does not qualify interrupted-save recovery
as complete. No automatic format or repair was attempted.

Normal SD boots in this campaign took about 67–71 seconds to expose a responsive
shell; the quick RAM fallback must not count as a successful SD recovery.
Neither these EN resets nor the normal save tests establish physical SD
power-loss tolerance. Card absence and a card/controller fault cannot be
distinguished by the current slot probe because there is no card-detect signal.

### Subsequent recovery and exact-content verification

After the user reported switching the device off, waiting and starting it again,
a subsequent serial session confirmed `SDROOT active=1`, the same 30,867,456-sector
card and the persistent SD root. All four normal-save files matched their recorded
contents, lengths and SHA-256 hashes. The interrupted-save file `/home/at-r3.txt`
also matched the complete 2,058-byte old version exactly. No corruption was
observed in these five qualification files, and no reformat or repair was needed.

This resolves the pending content check above; the device is again running with
the SD root. It does not resolve the intermittent probe failure or qualify the
unexecuted fifth reset trial. Actual power-rail state was not measured, and the
serial attachment observed USB reset reason 11, so this is subsequent recovery
evidence, not a controlled physical power-cut experiment.
