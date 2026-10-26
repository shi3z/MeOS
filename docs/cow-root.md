# Immutable system images and transactional block overlays

Status: transactional core plus the Tab5 SD/littlefs boot adapter.
See `sd-root-device.md` for exact device evidence and remaining qualification.

## Durable block model

`fs/cow/cow.c` exposes 512-byte sectors over an immutable base reader and an
append-only preallocated log. Data and commit records have CRC32, base identity,
geometry and transaction generation; commit records bind the ordered data-record
CRCs. Data is synced before a commit record is issued, and that record is synced
before success is returned. Recovery exposes only complete committed generations.
An IO/sync failure can have an uncertain outcome: the caller receives failure,
but recovery may select the old OR new complete transaction. Never assume a failed
commit means a rollback. Live writes are sealed until authoritative recovery.

The caller must bind the exact immutable base to a stable identity (prefer a
verified cryptographic manifest). The 64-bit identity here checks association; it
is not authentication. CRCs detect accidental corruption, not hostile modification.
Arbitrary post-commit media corruption may lose previously acknowledged data.

Callbacks transfer exactly the requested length or return nonzero. `sync` must
make all prior writes durable and ordered. Failure must be reported, not silently
acknowledged. The SD controller/FTL must actually meet this contract; software
fault tests cannot establish that. Byte-addressed records are 552 bytes and need
a file adapter or explicit aligned storage layer before direct SD use.

On Tab5 the outer littlefs volume contains a sparse read-only `base.img` and two
log files, `delta-a.log` and `delta-b.log`. The exposed M5FS device is 8 MiB
(16,384 sectors). Each bank has a logical capacity of 12 MiB, but littlefs files
grow only as records are appended; capacity is not reserved as RAM or eagerly
written SD space. Bulk `/assets` files are siblings in littlefs and bypass this
M5FS COW path entirely.

Every log record is exactly 552 bytes. Its 32-byte prefix identifies the record
type, immutable-base identity, transaction generation, sector number, running
transaction digest and geometry. A data record then carries one 512-byte sector;
the final four bytes are CRC32. A commit record uses the same fixed layout but no
sector payload. One logical transaction therefore costs
`552 * (changed sectors + 1)` bytes. Filesystem metadata means a small user write
can change several sectors; it is not a byte-for-byte journal.

## Two-bank collection and publication

`fs/cow/store.c` manages two independent banks. Each has a 512-byte header with
base identity, geometry, epoch and CRC, followed by its COW log. `open` never
formats. Only the explicitly destructive `cow_store_format` resets both banks.

Compaction, including recovery from an uncommitted/torn tail:

1. Keep the selected committed bank untouched.
2. Reset only the other bank, then sync that reset before reusing it.
3. Write its header with the next epoch.
4. Copy each sector that differs from the immutable base into a fresh transaction.
5. Sync the snapshot data, append its commit and sync again.
6. Select the new bank only after that commit completes. Retain the old bank until
   the next collection needs to reuse it.

At restart, a bank without its first committed snapshot is unpublished. Headers
are checked first and the newest epoch is scanned. The older bank is scanned only
if the newest has no published transaction. Equal epochs, incompatible identities
and unreadable bank IO are rejected. The scan validates every record through the
last commit and simultaneously builds a 16,384-entry sector-to-record index
(64 KiB in PSRAM). A torn or otherwise invalid tail seals the bank; recovery
rebuilds the index only through the last commit and compacts before permitting
another write.

Compaction requires spare space: each bank must fit the current changed-sector
snapshot PLUS the desired transaction headroom. `cow_store_reserve` configures the
minimum free record budget before beginning an operation (including its commit).
If a larger operation still exhausts capacity, abort/recover and report failure;
never rerun arbitrary application callbacks automatically. Oversized snapshots and
IO errors leave the old bank intact. An epoch never wraps.

No image-sized snapshot buffer is allocated. Normal reads use the PSRAM index and
touch either the latest data record or the immutable base. Boot recovery is still
linear in the selected bank's physical log length because the index is rebuilt on
every boot. Compaction scans all 16,384 logical sectors, compares current contents
with the base, and writes only differing sectors to the inactive bank. This bounds
steady-state history but can be a long, write-heavy operation. A future persistent
checkpoint could shorten boot only if it is bound to the bank epoch, generation,
committed offset and checksums; an unverified cached index would turn corruption
or a power cut into silent misdirection. Two banks consume SD storage, not two
image-sized RAM buffers.

## M5FS transaction and error boundaries

`fs_mount_atomic` installs begin/commit/recover callbacks. Ordinary mutating `fs_*`
operations each acquire the single filesystem mutex, begin a transaction, perform
the operation, flush data/metadata/superblock and commit before returning success.
A reader or second writer cannot enter while a group is running. Raw `fs_mount`
keeps the legacy behavior; merely using a COW blockdev does not enable transactions.

`fs_transaction(callback, context)` groups multiple calls using a callback-scoped
`fs_tx_ops_t` table. Use that table inside the callback; recursive public `fs_*`
calls would deadlock. Return zero to commit or a negative error to abort. Any failed
operation or short write poisons the entire group even if the callback ignores it.
The table is a trusted internal API, not an untrusted-app capability boundary.

A full atomic save must group open/truncate, writes and close. Separate ordinary
open/truncate and write calls are separate commits and can leave an empty file
between them. Editor save paths now use `vfs_replace_file`; see `atomic-editor-save.md`.

```c
static int save(const fs_tx_ops_t *fs, void *context)
{
    const char *text = context;
    int fd = fs->open("/home/note", FS_O_WRITE | FS_O_CREATE | FS_O_TRUNC);
    if (fd < 0) return fd;
    size_t n = strlen(text);
    int rc = fs->write(fd, text, n);
    if (rc < 0) return rc;
    if ((size_t)rc != n) return ERR_NOSPC;
    return fs->close(fd);
}
/* Once mounted with a transactional backend: */
/* int rc = fs_transaction(save, "new complete contents"); */
```

On abort or uncertain commit, discard ALL in-memory M5FS metadata and descriptors,
recover authoritative block state and remount it. File handles have non-reused tokens,
so old handles cannot alias newly opened files. Applications must reopen after failed
transactional operations. If recovery/remount fails, leave M5FS offline; do not
continue with dirty metadata. Reattach explicitly after storage recovery succeeds.

An inflight marker survives an abandoned callback. After the kernel releases its
owner's mutex, the next accessor recovers before exposing data. Host tests simulate
abandonment at a callback boundary; arbitrary-instruction task termination and
allocator interruption still need device qualification. Transaction callbacks
must be bounded. A kernel-owned storage worker is preferred for future app wiring.

`fs_mount_readonly` rejects modifications and supports close/sync/unmount without
writes. `fs_discard` invalidates all handles and frees volatile state without any
storage callbacks, including when abandoning a transaction. The storage owner must
recover pending state before reattaching it. No automatic media formatting occurs.

## Evidence and remaining gates

Run `sh tools/cow/host_test.sh` with ASan/UBSan:

- 6,628 byte-cut cases (indexed and unindexed) for data/commit records, with immediate and buffered durability.
- 987 crash cases through the transaction/whole-buffer VFS APIs: create, grow, truncate,
  delete, mkdir, two open files and 16/32 KiB whole-buffer replacements. Separate processes drop volatile state; compare
  EVERY recovered sector with the complete old/new image, then mount M5FS read-only.
- 17,506 byte cuts across inactive-bank reset, header, snapshot and publication,
  including reuse of a previously populated bank. Verify committed contents and
  successful writes after restart. Both immediate and buffered durability are tested.
- Compaction sync errors before/after persistence, read failure, wrong base, repeated
  bank reuse, discarded tails, repeated capacity recovery and differences returning
  to base contents.
- M5FS legacy selftest, grouped save/rollback, ignored errors, short writes, torn IO,
  uncertain final sync, handle invalidation, full descriptor table, abandoned owner,
  concurrent reader exclusion, failed recovery staying offline, RO/no-write teardown,
  repeated saves/collection and zero tracked heap leaks.

Five changed production/selftest C sources also compile to Tab5 target objects using
the existing RISC-V toolchain and ABI flags. This is not a complete firmware build.
The original M5FS selftest now uses the actual returned handle rather than assuming
slot zero; its unused loop counter was removed for the strict host build.

Before SD deployment: select/implement the outer store and durable adapter, index
reads, bound collection time, migrate save endpoints, wire boot/rescue, integrate
concurrent branches, and qualify actual SD write ordering/power cuts and memory.
The M5FS on-disk file/extent/name limits are unchanged. The volume is still a singleton.

## Tab5 memory baseline (2026-10-24)

Source and an already-existing device-session log were inspected; no serial port
was opened because a concurrent Quake session owned `/dev/cu.usbmodem101`.
The log's most recent heap sample reported KiB (despite its `KB` labels):

- Internal dynamic heap: total 192, used 112, free 79 KiB (integer truncation).
- PSRAM dynamic heap: total 27,431, used 8,869, free 18,562 KiB.
- RAM root backing: fixed 8,192 KiB, allocated in PSRAM irrespective of file usage.

Hardware PSRAM is 32 MiB. The current board reserves 4 MiB for IDF/display use;
external static BSS/alignment is also excluded from the main heap in the primary
checkout. Thus total dynamic heap varies by linked firmware. The latest inspected
sample is ~26.79 MiB total / 8.66 MiB used / 18.13 MiB free, not a fresh live query.
It comes from `/tmp/meos-quake-device.log`, which was still being updated during
inspection. Dynamic allocations and fragmentation depend on applications/history.

Moving the root backing to SD avoids an 8 MiB allocation, not an increase in physical
RAM or the heap arena size. Under the same workload, free PSRAM would approach
26.13 MiB before subtracting new storage overhead. A 128 MiB M5FS image increases
the resident allocation bitmap from 2 KiB to 32 KiB, reducing the gain by ~30 KiB;
the directory table stays 8 KiB. Outer-FS cache/index/compaction buffers still need
budgets. A realistic initial target is roughly 7.5–8 MiB net reclaimed, subject to
measurement; no achieved firmware memory reduction is claimed. Internal SRAM does
not gain 8 MiB, and current heap fragmentation may prevent one large allocation.

Flash already holds the bootloader and kernel. The proposed change is SD-backed
root/data, not copying the kernel from SD into RAM. The full 4 MiB display reserve
is not automatically reclaimable: separating reservation from actual display
allocation is independent tuning work. The 192 KiB heap is not total chip SRAM;
static kernel data, code/IRAM, startup and display/DMA allocations are separate.
