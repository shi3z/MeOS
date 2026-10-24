#pragma once
#include <m5os/types.h>
#include <m5os/blockdev.h>

/*
 * M5FS public API (one mounted volume for now; the VFS of Phase 7 will wrap
 * these calls).  Paths are absolute: "/home/notes.txt".  Functions return
 * >= 0 on success and a negative ERR_* (m5os/errno.h) on failure.
 */
#define FS_O_READ    0x01
#define FS_O_WRITE   0x02
#define FS_O_CREATE  0x04            /* create the file if it does not exist */
#define FS_O_TRUNC   0x08            /* discard existing contents (needs FS_O_WRITE) */
#define FS_O_APPEND  0x10            /* every write goes to the end */

#define FS_SEEK_SET  0
#define FS_SEEK_CUR  1
#define FS_SEEK_END  2

#define FS_NAME_MAX  23              /* characters per path component */

typedef enum { FS_TYPE_FILE = 1, FS_TYPE_DIR = 2, FS_TYPE_DEV = 3 /* devfs only */ } fs_type_t;

typedef struct {
    char      name[FS_NAME_MAX + 1];
    fs_type_t type;
    uint32_t  size;                  /* bytes; 0 for directories */
} fs_info_t;

typedef struct {
    uint32_t block_size, total_blocks, free_blocks;
} fs_stat_t;

/* Create a fresh volume on blocks [base_lba, base_lba+nblocks) of bd.
 * DESTROYS everything in that range.  Creates /system, /apps and /home. */
int  fs_format(blockdev_t *bd, uint32_t base_lba, uint32_t nblocks);
int  fs_mount(blockdev_t *bd, uint32_t base_lba);
int  fs_unmount(void);
int  fs_sync(void);                              /* flush metadata to the device */
int  fs_statfs(fs_stat_t *out);

int  fs_open(const char *path, uint32_t flags);  /* returns fd >= 0 */
int  fs_read(int fd, void *buf, size_t len);     /* bytes read, 0 at end of file */
int  fs_write(int fd, const void *buf, size_t len);   /* bytes written (may be short if full) */
int  fs_seek(int fd, int32_t offset, int whence);     /* new position */
int  fs_close(int fd);
int  fs_stat(const char *path, fs_info_t *out);
int  fs_mkdir(const char *path);
int  fs_delete(const char *path);                /* a file, or an empty directory */
/* Call cb for every entry of the directory; stop early if cb returns non-zero. */
int  fs_list(const char *path, int (*cb)(const fs_info_t *info, void *ctx), void *ctx);

bool fs_selftest(void);                          /* runs on a RAM disk; never touches real media */

/* Optional transactional backend. Durable callbacks return zero on success.
 * recover abandons pending writes and selects authoritative durable state; after
 * an uncertain commit that may be the old OR new complete transaction. */
typedef struct {
    void *ctx;
    int (*begin)(void *);
    int (*commit)(void *);
    int (*recover)(void *);
    /* Optional no-IO headroom hint (data + metadata + commit records). */
    int (*reserve)(void *, uint32_t records);
} fs_tx_backend_t;
int fs_mount_atomic(blockdev_t *, uint32_t base_lba, const fs_tx_backend_t *);
int fs_mount_readonly(blockdev_t *, uint32_t base_lba);
/* Discard descriptors and volatile metadata without issuing writes. */
int fs_discard(void);

/* Callback-scoped API, all operations under the single filesystem mutex.
 * Callback returns 0 to commit or a negative ERR_* to abort. Any failed API call
 * (or short write) poisons the group even if callback ignores its result.
 * Use ONLY this table inside the callback: public fs_* would deadlock. Do not
 * retain the table or block indefinitely. Abandoned transactional owners are
 * recovered at next lock acquisition after kernel mutex release; real-device
 * task-kill qualification is still required.
 * Abort/recovery invalidates ALL open descriptors; reopen after any failed
 * transactional operation. Successful normal fs_* mutations commit individually.
 * Use fs_transaction to group truncate+write+close into one durable save. */
typedef struct {
    int (*open)(const char *, uint32_t);
    int (*close)(int);
    int (*read)(int, void *, size_t);
    int (*write)(int, const void *, size_t);
    int (*seek)(int, int32_t, int);
    int (*stat)(const char *, fs_info_t *);
    int (*mkdir)(const char *);
    int (*unlink)(const char *);
} fs_tx_ops_t;
int fs_transaction(int (*callback)(const fs_tx_ops_t *, void *), void *);
/* Whole-buffer replacement under the volume lock. Atomic with a transactional
 * backend; raw RAM volumes preserve legacy close/flush behavior. No fallback
 * after failure and no full-file copy. Recovery invalidates existing descriptors. */
int fs_replace_file(const char *path, const void *data, size_t len, size_t *written);
