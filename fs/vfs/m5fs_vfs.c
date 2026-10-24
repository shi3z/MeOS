/* Adapter: M5FS (fs.h) as a VFS filesystem.  The VFS file handle is the M5FS fd. */
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <m5os/app.h>

static int m_open(void *fsd, const char *rel, uint32_t flags, void **file)
{
    (void)fsd;
    /* Permissions: reading needs FS_READ; anything that can modify needs FS_WRITE. */
    if ((flags & VFS_O_READ) && !task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    if ((flags & (VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC | VFS_O_APPEND)) && !task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    int fd = fs_open(rel, flags);
    if (fd < 0) return fd;
    *file = (void *)(intptr_t)fd;
    return ERR_OK;
}
static int m_close(void *f)                          { return fs_close((int)(intptr_t)f); }
static int m_read(void *f, void *b, size_t n)        { return fs_read((int)(intptr_t)f, b, n); }
static int m_write(void *f, const void *b, size_t n) { return fs_write((int)(intptr_t)f, b, n); }
static int m_seek(void *f, int32_t o, int w)         { return fs_seek((int)(intptr_t)f, o, w); }
static void convert(const fs_info_t *src,vfs_info_t *dst) {
    for(unsigned i=0;i<=FS_NAME_MAX;i++)dst->name[i]=src->name[i];
    dst->type=src->type;dst->size=src->size;
}
static int m_stat(void *data,const char *rel,vfs_info_t *out) {
    (void)data;if(!task_has_perm(APP_PERM_FS_READ))return ERR_PERM;
    fs_info_t info;int rc=fs_stat(rel,&info);if(rc==ERR_OK)convert(&info,out);return rc;
}
struct dir_bridge { int (*cb)(const vfs_info_t *,void *);void *ctx; };
static int bridge(const fs_info_t *src,void *opaque) {
    struct dir_bridge *b=opaque;vfs_info_t info;convert(src,&info);return b->cb?b->cb(&info,b->ctx):0;
}
static int m_readdir(void *data,const char *rel,int (*cb)(const vfs_info_t *,void *),void *ctx) {
    (void)data;if(!task_has_perm(APP_PERM_FS_READ))return ERR_PERM;
    struct dir_bridge b={cb,ctx};return fs_list(rel,bridge,&b);
}
static int m_mkdir(void *fsd, const char *rel)       { (void)fsd; if (!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM; return fs_mkdir(rel); }
static int m_unlink(void *fsd, const char *rel)      { (void)fsd; if (!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM; return fs_delete(rel); }

const vfs_fs_t vfs_m5fs = {
    .name = "m5fs", .open = m_open, .close = m_close, .read = m_read, .write = m_write,
    .seek = m_seek, .stat = m_stat, .readdir = m_readdir, .mkdir = m_mkdir, .unlink = m_unlink,
};
