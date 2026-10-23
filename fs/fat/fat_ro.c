#include <m5os/fat.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
#include "ff.h"
#include "diskio.h"
#include <string.h>

/* FatFs is built without OS integration: this mutex serializes every library call.
 * Directory callbacks run outside it. Bound disk reads per operation so corrupt
 * cyclic cluster chains cannot keep a caller inside FatFs indefinitely. */
static mutex_t s_lock = MUTEX_INIT;
static FATFS s_fs;
static blockdev_t *s_bd;
static bool s_mounted;
static unsigned s_users, s_budget;
#define READ_BUDGET 4096u
static void lock(void) { mutex_lock(&s_lock); s_budget = READ_BUDGET; }
static void unlock(void) { mutex_unlock(&s_lock); }

DSTATUS disk_status(BYTE pdrv) { return pdrv || !s_bd ? STA_NOINIT : STA_PROTECT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }
DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    (void)pdrv; (void)buf; (void)sector; (void)count; return RES_WRPRT;
}
DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count)
{
    if (pdrv || !s_bd) return RES_NOTRDY;
    if (!count || sector >= s_bd->block_count || count > s_bd->block_count - sector) return RES_PARERR;
    if (count > s_budget) return RES_ERROR;
    s_budget -= count;
    return s_bd->read(s_bd, sector, count, buf) == ERR_OK ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    if (pdrv || !s_bd) return RES_NOTRDY;
    if (cmd == GET_SECTOR_COUNT) { *(LBA_t *)buf = s_bd->block_count; return RES_OK; }
    if (cmd == GET_SECTOR_SIZE) { *(WORD *)buf = 512; return RES_OK; }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buf = 1; return RES_OK; }
    return RES_PARERR; /* Includes CTRL_TRIM: never forwarded to the block device. */
}
static int result(FRESULT r)
{
    switch (r) {
    case FR_OK: return ERR_OK;
    case FR_NO_FILE: case FR_NO_PATH: return ERR_NOENT;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: return ERR_INVAL;
    case FR_DENIED: case FR_WRITE_PROTECTED: return ERR_PERM;
    case FR_NOT_READY: return ERR_NODEV;
    case FR_NOT_ENOUGH_CORE: return ERR_NOMEM;
    case FR_NO_FILESYSTEM: case FR_INT_ERR: return ERR_CORRUPT;
    default: return ERR_IO;
    }
}
static void info(const FILINFO *fi, vfs_info_t *out)
{
    memset(out, 0, sizeof *out);
    strncpy(out->name, fi->fname, VFS_NAME_MAX);
    out->type = (fi->fattrib & AM_DIR) ? FS_TYPE_DIR : FS_TYPE_FILE;
    out->size = fi->fsize;
}
static int ro_open(void *data, const char *path, uint32_t flags, void **file)
{
    (void)data;
    if (!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    if (flags != VFS_O_READ) return ERR_PERM;
    FIL *f = kmalloc(sizeof *f);
    if (!f) return ERR_NOMEM;
    lock();
    int rc = s_mounted ? result(f_open(f, path, FA_READ)) : ERR_NODEV;
    if (rc == ERR_OK) { s_users++; *file = f; }
    unlock();
    if (rc != ERR_OK) kfree(f);
    return rc;
}
static int ro_close(void *file)
{
    lock(); int rc = result(f_close(file)); s_users--; unlock();
    kfree(file); return rc;
}
static int ro_read(void *file, void *buf, size_t len)
{
    if (len > INT32_MAX) return ERR_INVAL;
    UINT got = 0;
    lock(); int rc = result(f_read(file, buf, (UINT)len, &got)); unlock();
    return rc == ERR_OK ? (int)got : rc;
}
static int ro_write(void *file, const void *buf, size_t len)
{
    (void)file; (void)buf; (void)len; return ERR_PERM;
}
static int ro_seek(void *file, int32_t offset, int whence)
{
    FIL *f = file;
    lock();
    int64_t pos = whence == FS_SEEK_SET ? offset :
                  whence == FS_SEEK_CUR ? (int64_t)f_tell(f) + offset :
                  whence == FS_SEEK_END ? (int64_t)f_size(f) + offset : -1;
    int rc = pos < 0 || pos > INT32_MAX ? ERR_INVAL : result(f_lseek(f, (FSIZE_t)pos));
    if (rc == ERR_OK) rc = (int)f_tell(f);
    unlock(); return rc;
}
static int ro_stat(void *data, const char *path, vfs_info_t *out)
{
    (void)data;
    if (!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    lock();
    int rc = ERR_NODEV;
    if (s_mounted) {
        if (!strcmp(path, "/")) { memset(out, 0, sizeof *out); out->name[0] = '/'; out->type = FS_TYPE_DIR; rc = ERR_OK; }
        else { FILINFO fi; rc = result(f_stat(path, &fi)); if (rc == ERR_OK) info(&fi, out); }
    }
    unlock(); return rc;
}
static int ro_readdir(void *data, const char *path, int (*cb)(const vfs_info_t *, void *), void *ctx)
{
    (void)data;
    if (!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    DIR dir; FILINFO fi;
    lock();
    int rc = s_mounted ? result(f_opendir(&dir, path)) : ERR_NODEV;
    if (rc == ERR_OK) s_users++;
    unlock();
    if (rc != ERR_OK) return rc;
    int n = 0;
    unsigned remaining = READ_BUDGET; /* Also bounds repeated one-entry reads of a cyclic directory. */
    for (;;) {
        lock();
        s_budget = remaining;
        rc = result(f_readdir(&dir, &fi));
        remaining = s_budget;
        unlock();
        if (rc != ERR_OK || !fi.fname[0]) break;
        if (!strcmp(fi.fname, ".") || !strcmp(fi.fname, "..")) continue;
        vfs_info_t entry; info(&fi, &entry); n++;
        if (cb && cb(&entry, ctx)) break;
        /* Bound entry count as well, even if FatFs serves entries from its cache. */
        if (n >= 65536) { rc = ERR_CORRUPT; break; }
    }
    lock(); f_closedir(&dir); s_users--; unlock();
    return rc == ERR_OK ? n : rc;
}
static int ro_modify(void *data, const char *path) { (void)data; (void)path; return ERR_PERM; }
static const vfs_fs_t s_vfs = {
    .name = "fat-ro", .open = ro_open, .close = ro_close, .read = ro_read,
    .write = ro_write, .seek = ro_seek, .stat = ro_stat, .readdir = ro_readdir,
    .mkdir = ro_modify, .unlink = ro_modify
};
static bool geometry_fits(void)
{
    uint8_t boot[512];
    if (disk_read(0, boot, s_fs.volbase, 1) != RES_OK) return false;
    uint32_t total = (uint32_t)boot[19] | ((uint32_t)boot[20] << 8);
    if (!total) total = (uint32_t)boot[32] | ((uint32_t)boot[33] << 8) |
                        ((uint32_t)boot[34] << 16) | ((uint32_t)boot[35] << 24);
    uint64_t end = (uint64_t)s_fs.volbase + total;
    return total && end <= s_bd->block_count && s_fs.n_fatent > 2 &&
           s_fs.fatbase > s_fs.volbase && s_fs.database > s_fs.fatbase &&
           (uint64_t)s_fs.fatbase + (uint64_t)s_fs.fsize * s_fs.n_fats <= s_fs.database &&
           (uint64_t)s_fs.database + (uint64_t)(s_fs.n_fatent - 2) * s_fs.csize <= end;
}
int fat_mount_readonly(blockdev_t *bd)
{
    if (!bd || !bd->read || bd->block_size != 512 || !bd->block_count) return ERR_INVAL;
    lock();
    if (s_mounted || s_users) { unlock(); return ERR_BUSY; }
    s_bd = bd;
    int rc = result(f_mount(&s_fs, "", 1));
    if (rc == ERR_OK && !geometry_fits()) rc = ERR_CORRUPT;
    if (rc == ERR_OK) rc = vfs_mount("/sd", &s_vfs, NULL);
    if (rc == ERR_OK) s_mounted = true;
    else { f_mount(NULL, "", 0); s_bd = NULL; }
    unlock(); return rc;
}
int fat_unmount(void)
{
    lock();
    if (!s_mounted) { unlock(); return ERR_NOENT; }
    if (s_users) { unlock(); return ERR_BUSY; }
    int rc = vfs_umount("/sd");
    if (rc == ERR_OK) { f_mount(NULL, "", 0); s_bd = NULL; s_mounted = false; }
    unlock(); return rc;
}
bool fat_is_mounted(void) { lock(); bool value = s_mounted; unlock(); return value; }
