# M5FS transactions (review slice 2)

Depends on `pr/cow-core`. This slice extracts production M5FS/API changes from
`cd95cc15e6eea542550402c05a0d44bfd523e20f` without SD, littlefs, root selection,
Quake, app-saving or user-visible path changes. Raw `fs_mount` remains the legacy
entry point; no boot caller switches to `fs_mount_atomic` in this branch.

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
between them. Existing VFS/app save paths have NOT yet been migrated to group saves.

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


## Validation and limits

`sh tools/cow/host_test.sh` retains the COW core/fault tests and adds 210 crashes
through real M5FS transaction groups. Independent processes discard volatile
state, compare every recovered sector against complete old/new images, then
mount M5FS read-only. Sanitizer tests cover the legacy M5FS selftest, grouped
rollback, ignored operation failures/short writes, uncertain commits, descriptor
invalidation, full descriptor tables, abandoned callbacks, concurrent reader
exclusion, failed recovery staying offline, read-only/no-write teardown, repeated
collection and tracked heap cleanup.

Callback-boundary abandonment is simulated. Arbitrary-instruction app termination,
real SD sync/FTL ordering, physical power cuts and collection latency remain
unqualified. Applications must reopen handles invalidated by recovery. Whole-file
atomic replacement through VFS and language/editor save paths is a later PR;
individual open/truncate/write/close operations are not one atomic save.

Review hardening: mount rejects wrapped and out-of-range extents using subtraction
bounds, before later operations can use their sector ranges. A malformed-metadata
regression checks six invalid ranges, an exact end-of-volume range and allocation
cleanup on rejected mounts. The on-disk format is unchanged.
