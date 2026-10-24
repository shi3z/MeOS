#pragma once
#include <m5os/types.h>
#include <m5os/fs.h>

/*
 * Virtual file system: one namespace for files and devices.
 *
 *     int fd = vfs_open("/dev/buttons", VFS_O_READ);
 *     uint8_t state; vfs_read(fd, &state, 1);          // bit0=A bit1=B bit2=C
 *     vfs_close(fd);
 *
 *     fd = vfs_open("/home/notes.txt", VFS_O_WRITE | VFS_O_CREATE);
 *     vfs_write(fd, "hi", 2);
 *
 * Mount points are matched by longest path prefix on component boundaries:
 *     /       -> M5FS (SD card, or a RAM disk when there is no formatted card)
 *     /dev    -> devfs (every device registered with device_register())
 * The names are vfs_*() because open/read/write/close are libc symbols; define
 * M5OS_POSIX_NAMES before including this header to get the short names.
 *
 * Paths must be absolute; "." , ".." and repeated slashes are normalized.
 * All functions return >= 0 or a negative ERR_* (m5os/errno.h).
 */
#define VFS_O_READ    FS_O_READ
#define VFS_O_WRITE   FS_O_WRITE
#define VFS_O_CREATE  FS_O_CREATE
#define VFS_O_TRUNC   FS_O_TRUNC
#define VFS_O_APPEND  FS_O_APPEND
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#define VFS_NAME_MAX 767 /* UTF-8 bytes: all 255 UTF-16-unit VFAT names fit */
#define VFS_PATH_MAX 1024
#else
#define VFS_NAME_MAX FS_NAME_MAX
#define VFS_PATH_MAX 96
#endif
#define VFS_FD_MAX    16
#define VFS_MOUNT_MAX 4

typedef struct {
    char name[VFS_NAME_MAX + 1];
    fs_type_t type;
    uint32_t size;
} vfs_info_t;            /* type: FS_TYPE_FILE / FS_TYPE_DIR / FS_TYPE_DEV */

/* What a filesystem driver provides.  `rel` is the path inside the mount,
 * always starting with '/'.  Any operation may be NULL (-> ERR_NOSYS). */
typedef struct {
    const char *name;
    int (*open)(void *fsdata, const char *rel, uint32_t flags, void **file);
    int (*close)(void *file);
    int (*read)(void *file, void *buf, size_t len);
    int (*write)(void *file, const void *buf, size_t len);
    int (*seek)(void *file, int32_t offset, int whence);
    int (*ioctl)(void *file, uint32_t cmd, void *arg);
    int (*stat)(void *fsdata, const char *rel, vfs_info_t *out);
    int (*readdir)(void *fsdata, const char *rel, int (*cb)(const vfs_info_t *, void *), void *ctx);
    int (*mkdir)(void *fsdata, const char *rel);
    int (*unlink)(void *fsdata, const char *rel);
} vfs_fs_t;

void vfs_init(void);                                      /* mounts devfs at /dev */
/* Collapse ".", ".." and repeated slashes of an absolute path into out (>= VFS_PATH_MAX bytes). */
int  vfs_normalize(const char *path, char *out);
int  vfs_mount(const char *target, const vfs_fs_t *fs, void *fsdata);
int  vfs_umount(const char *target);

int  vfs_open(const char *path, uint32_t flags);
int  vfs_close(int fd);
int  vfs_read(int fd, void *buf, size_t len);
int  vfs_write(int fd, const void *buf, size_t len);
int  vfs_seek(int fd, int32_t offset, int whence);
int  vfs_ioctl(int fd, uint32_t cmd, void *arg);
int  vfs_stat(const char *path, vfs_info_t *out);
int  vfs_readdir(const char *path, int (*cb)(const vfs_info_t *, void *), void *ctx);   /* returns entry count */
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);                        /* file or empty directory */
void vfs_print_mounts(void);
int  vfs_close_owned(uint32_t app_id);     /* close the handles an app left open; returns how many */
int  vfs_count_owned(uint32_t app_id);

extern const vfs_fs_t vfs_devfs;                          /* fs/vfs/devfs.c   */
extern const vfs_fs_t vfs_m5fs;                           /* fs/vfs/m5fs_vfs.c */

bool vfs_selftest(void);

#ifdef M5OS_POSIX_NAMES
#define open   vfs_open
#define close  vfs_close
#define read   vfs_read
#define write  vfs_write
#endif
