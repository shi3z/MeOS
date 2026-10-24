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
static bool s_mounted, s_readonly, s_faulted;
typedef struct { FIL fil; uint32_t flags; } fat_file;
static unsigned s_users, s_budget;
#define READ_BUDGET 4096u
static void lock(void) { mutex_lock(&s_lock); s_budget = READ_BUDGET; }
static void unlock(void) { mutex_unlock(&s_lock); }

DSTATUS disk_status(BYTE pdrv) { return pdrv || !s_bd || s_faulted ? STA_NOINIT : (s_readonly ? STA_PROTECT : 0); }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }
DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count)
{
    if (pdrv || !s_bd) return RES_NOTRDY;
    if (s_readonly || !s_bd->write) return RES_WRPRT;
    if (s_faulted) return RES_NOTRDY;
    if (!buf || !count || sector>=s_bd->block_count || count>s_bd->block_count-sector) return RES_PARERR;
    if (count>s_budget) { s_faulted=true; return RES_ERROR; }
    s_budget-=count;
    if (s_bd->write(s_bd,sector,count,buf)!=ERR_OK) {s_faulted=true;return RES_ERROR;}
    return RES_OK;
}
DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count)
{
    if (pdrv || !s_bd || s_faulted) return RES_NOTRDY;
    if (!count || sector >= s_bd->block_count || count > s_bd->block_count - sector) return RES_PARERR;
    if (count > s_budget) { s_faulted=true; return RES_ERROR; }
    s_budget -= count;
    if (s_bd->read(s_bd,sector,count,buf)!=ERR_OK) {s_faulted=true;return RES_ERROR;}
    return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    if (pdrv || !s_bd || s_faulted) return RES_NOTRDY;
    /* Block-device writes are synchronous, including the SD programming wait. */
    if (cmd == CTRL_SYNC) return RES_OK;
    if (cmd == GET_SECTOR_COUNT) { *(LBA_t *)buf = s_bd->block_count; return RES_OK; }
    if (cmd == GET_SECTOR_SIZE) { *(WORD *)buf = 512; return RES_OK; }
    if (cmd == GET_BLOCK_SIZE) { *(DWORD *)buf = 1; return RES_OK; }
    return RES_PARERR; /* Includes CTRL_TRIM: never forwarded to the block device. */
}
void *ff_memalloc(UINT n) { return kmalloc(n); }
void ff_memfree(void *p) { kfree(p); }

static int result(FRESULT r)
{
    switch (r) {
    case FR_OK: return ERR_OK;
    case FR_NO_FILE: case FR_NO_PATH: return ERR_NOENT;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: return ERR_INVAL;
    case FR_DENIED: case FR_WRITE_PROTECTED: return ERR_PERM;
    case FR_NOT_READY: return ERR_NODEV;
    case FR_LOCKED: case FR_TOO_MANY_OPEN_FILES: return ERR_BUSY;
    case FR_EXIST: return ERR_EXIST;
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
static int fat_open(void *data, const char *path, uint32_t flags, void **file)
{
    (void)data;
    const uint32_t allowed=VFS_O_READ|VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC|VFS_O_APPEND;
    if ((flags&~allowed) || !(flags&(VFS_O_READ|VFS_O_WRITE)) ||
        ((flags&(VFS_O_CREATE|VFS_O_TRUNC|VFS_O_APPEND)) && !(flags&VFS_O_WRITE))) return ERR_INVAL;
    if ((flags&VFS_O_READ) && !task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    if ((flags&VFS_O_WRITE) && !task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    fat_file *f=kmalloc(sizeof *f);if(!f)return ERR_NOMEM;
    memset(f,0,sizeof *f);f->flags=flags;
    BYTE mode=(flags&VFS_O_READ?FA_READ:0)|(flags&VFS_O_WRITE?FA_WRITE:0);
    if(flags&VFS_O_CREATE)mode|=flags&VFS_O_TRUNC?FA_CREATE_ALWAYS:FA_OPEN_ALWAYS;
    lock();
    int rc=!s_mounted?ERR_NODEV:s_faulted?ERR_IO:
        (s_readonly && (flags&VFS_O_WRITE))?ERR_PERM:result(f_open(&f->fil,path,mode));
    bool opened=rc==ERR_OK;
    if(opened && (flags&VFS_O_TRUNC) && !(flags&VFS_O_CREATE)) {
        rc=result(f_truncate(&f->fil));if(rc==ERR_OK)rc=result(f_sync(&f->fil));
    }
    if(rc==ERR_OK && (flags&VFS_O_APPEND))rc=result(f_lseek(&f->fil,f_size(&f->fil)));
    if(rc==ERR_OK){s_users++;*file=f;}
    else if(opened)f_close(&f->fil);
    unlock();if(rc!=ERR_OK)kfree(f);return rc;
}
static int fat_close(void *file)
{
    fat_file *f=file;lock();int rc=s_faulted?ERR_IO:result(f_close(&f->fil));s_users--;unlock();
    kfree(f);return rc;
}
static int fat_read(void *file,void *buf,size_t len)
{
    if(len>INT32_MAX)return ERR_INVAL;
    fat_file *f=file;UINT got=0;lock();
    int rc=s_faulted?ERR_IO:result(f_read(&f->fil,buf,(UINT)len,&got));
    unlock();return rc==ERR_OK?(int)got:rc;
}
static int fat_write(void *file,const void *buf,size_t len)
{
    if(len>INT32_MAX)return ERR_INVAL;
    fat_file *f=file;UINT written=0;lock();
    int rc=s_readonly?ERR_PERM:s_faulted?ERR_IO:!(f->flags&VFS_O_WRITE)?ERR_PERM:ERR_OK;
    if(rc==ERR_OK && (f->flags&VFS_O_APPEND))rc=result(f_lseek(&f->fil,f_size(&f->fil)));
    if(rc==ERR_OK && (uint64_t)f_tell(&f->fil)+len>INT32_MAX)rc=ERR_INVAL;
    if(rc==ERR_OK)rc=result(f_write(&f->fil,buf,(UINT)len,&written));
    /* Report a save only after both data and metadata reach the block device. */
    if(rc==ERR_OK)rc=result(f_sync(&f->fil));
    unlock();return rc==ERR_OK?(written?(int)written:(len?ERR_NOSPC:0)):rc;
}
static int fat_seek(void *file,int32_t offset,int whence)
{
    fat_file *handle=file;FIL *f=&handle->fil;lock();
    int64_t pos=whence==FS_SEEK_SET?offset:whence==FS_SEEK_CUR?(int64_t)f_tell(f)+offset:
        whence==FS_SEEK_END?(int64_t)f_size(f)+offset:-1;
    int rc=pos<0 || pos>INT32_MAX?ERR_INVAL:s_faulted?ERR_IO:ERR_OK;
    /* A seek never extends a file; subsequent writes may append at its end. */
    if(rc==ERR_OK && (uint64_t)pos>f_size(f))pos=f_size(f);
    if(rc==ERR_OK)rc=result(f_lseek(f,(FSIZE_t)pos));
    if(rc==ERR_OK)rc=(int)f_tell(f);
    unlock();return rc;
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
static int modify(void *data,const char *path,bool mkdir)
{
    (void)data;if(!task_has_perm(APP_PERM_FS_WRITE))return ERR_PERM;
    lock();int rc=!s_mounted?ERR_NODEV:s_readonly?ERR_PERM:s_faulted?ERR_IO:
        result(mkdir?f_mkdir(path):f_unlink(path));unlock();return rc;
}
static int fat_mkdir(void *data,const char *path){return modify(data,path,true);}
static int fat_unlink(void *data,const char *path){return modify(data,path,false);}
static const vfs_fs_t s_vfs = {
    .name = "fat", .open = fat_open, .close = fat_close, .read = fat_read,
    .write = fat_write, .seek = fat_seek, .stat = ro_stat, .readdir = ro_readdir,
    .mkdir = fat_mkdir, .unlink = fat_unlink
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
int fat_mount(blockdev_t *bd, bool readonly)
{
    if (!bd || !bd->read || (!readonly && !bd->write) || bd->block_size != 512 || !bd->block_count) return ERR_INVAL;
    lock();
    if (s_mounted || s_users) { unlock(); return ERR_BUSY; }
    s_bd = bd;s_readonly=readonly;s_faulted=false;
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

int fat_mount_readonly(blockdev_t *bd){return fat_mount(bd,true);}
bool fat_is_readonly(void){lock();bool ro=s_readonly;unlock();return ro;}
