#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <m5os/arch.h>
#include <m5os/kprintf.h>
#include <m5os/app.h>

typedef struct { bool used; char path[VFS_PATH_MAX]; const vfs_fs_t *fs; void *fsdata; uint16_t len; } mount_t;
typedef struct { bool used, pending; const mount_t *m; void *file; uint32_t owner_app; } vfd_t;

static mount_t s_mounts[VFS_MOUNT_MAX];
static vfd_t   s_fds[VFS_FD_MAX];

static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

/* ---- path handling ------------------------------------------------------ */

/* Collapse "//", ".", ".." into out ("/a/b" form, "/" for the root). */
int vfs_normalize(const char *path, char *out)
{
    if (!path || path[0] != '/') return ERR_INVAL;
    size_t len = 1;
    out[0] = '/'; out[1] = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *c = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - c);
        if (n == 1 && c[0] == '.') continue;
        if (n == 2 && c[0] == '.' && c[1] == '.') {                 /* drop the last component */
            while (len > 1 && out[len - 1] != '/') len--;
            if (len > 1) len--;
            out[len] = 0;
            if (len == 0) { out[0] = '/'; out[1] = 0; len = 1; }
            continue;
        }
        if (n > VFS_NAME_MAX || len + n + 1 >= VFS_PATH_MAX) return ERR_NAMETOOLONG;
        if (len > 1) out[len++] = '/';
        for (size_t i = 0; i < n; i++) out[len++] = c[i];
        out[len] = 0;
    }
    return ERR_OK;
}

/* Longest mount prefix on a component boundary. */
static const mount_t *find_mount(const char *norm, const char **rel)
{
    const mount_t *best = NULL;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        const mount_t *m = &s_mounts[i];
        if (!m->used) continue;
        size_t pl = m->len;
        bool match = true;                           /* the root mount "/" matches everything */
        if (pl > 1) {
            size_t k = 0;
            while (k < pl && norm[k] == m->path[k]) k++;
            match = (k == pl) && (norm[pl] == 0 || norm[pl] == '/');   /* whole components only */
        }
        if (match && (!best || m->len > best->len)) best = m;
    }
    if (best) *rel = (best->len == 1) ? norm : (norm[best->len] ? norm + best->len : "/");
    return best;
}

static int resolve(const char *path, char *norm, const mount_t **m, const char **rel)
{
    int rc = vfs_normalize(path, norm);
    if (rc != ERR_OK) return rc;
    *m = find_mount(norm, rel);
    return *m ? ERR_OK : ERR_NOENT;
}

/* ---- mounting ----------------------------------------------------------- */

int vfs_mount(const char *target, const vfs_fs_t *fs, void *fsdata)
{
    char norm[VFS_PATH_MAX];
    if (!fs || vfs_normalize(target, norm) != ERR_OK) return ERR_INVAL;
    uint32_t irq = arch_irq_disable();
    int slot = -1, rc = ERR_OK;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        if (s_mounts[i].used) {
            size_t k = 0;
            while (norm[k] && norm[k] == s_mounts[i].path[k]) k++;
            if (norm[k] == s_mounts[i].path[k]) rc = ERR_BUSY;          /* already mounted there */
        } else if (slot < 0) slot = i;
    }
    if (rc == ERR_OK && slot < 0) rc = ERR_BUSY;
    if (rc == ERR_OK) {
        mount_t *m = &s_mounts[slot];
        size_t n = slen(norm);
        for (size_t i = 0; i <= n; i++) m->path[i] = norm[i];
        m->len = (uint16_t)n; m->fs = fs; m->fsdata = fsdata; m->used = true;
    }
    arch_irq_restore(irq);
    return rc;
}

int vfs_umount(const char *target)
{
    char norm[VFS_PATH_MAX];
    if (vfs_normalize(target, norm) != ERR_OK) return ERR_INVAL;
    uint32_t irq = arch_irq_disable();
    int rc = ERR_NOENT;
    for (int i = 0; i < VFS_MOUNT_MAX; i++) {
        mount_t *m = &s_mounts[i];
        if (!m->used) continue;
        size_t k = 0;
        while (norm[k] && norm[k] == m->path[k]) k++;
        if (norm[k] != m->path[k]) continue;
        rc = ERR_OK;
        for (int f = 0; f < VFS_FD_MAX; f++) if (s_fds[f].used && s_fds[f].m == m) rc = ERR_BUSY;
        if (rc == ERR_OK) m->used = false;
        break;
    }
    arch_irq_restore(irq);
    return rc;
}

void vfs_init(void)
{
    vfs_mount("/dev", &vfs_devfs, NULL);
}

void vfs_print_mounts(void)
{
    for (int i = 0; i < VFS_MOUNT_MAX; i++)
        if (s_mounts[i].used) kprintf("%-8s on %s\n", s_mounts[i].fs->name, s_mounts[i].path);
}

/* ---- file operations ---------------------------------------------------- */

static vfd_t *fd_get(int fd)
{
    if (fd < 0 || fd >= VFS_FD_MAX || (!s_fds[fd].used || s_fds[fd].pending)) return NULL;
    return &s_fds[fd];
}

int vfs_open(const char *path, uint32_t flags)
{
    char norm[VFS_PATH_MAX]; const mount_t *m; const char *rel;
    int rc = resolve(path, norm, &m, &rel);
    if (rc != ERR_OK) return rc;
    if (!m->fs->open) return ERR_NOSYS;

    int fd = ERR_BUSY;
    uint32_t irq = arch_irq_disable();                       /* reserve a slot */
    for (int i = 0; i < VFS_FD_MAX; i++) if (!s_fds[i].used) { s_fds[i].used = true; s_fds[i].pending = false; s_fds[i].m = m; s_fds[i].file = NULL; s_fds[i].owner_app = task_current() && task_current()->app ? task_current()->app->app_id : 0; fd = i; break; }
    arch_irq_restore(irq);
    if (fd < 0) return fd;

    void *file = NULL;
    rc = m->fs->open(m->fsdata, rel, flags, &file);
    if (rc < 0) { s_fds[fd].used = false; return rc; }
    s_fds[fd].file = file;
    return fd;
}

int vfs_close(int fd)
{
    vfd_t *f = fd_get(fd);
    if (!f) return ERR_BADF;
    int rc = f->m->fs->close ? f->m->fs->close(f->file) : ERR_OK;
    f->used = false;
    return rc;
}

int vfs_read(int fd, void *buf, size_t len)
{
    vfd_t *f = fd_get(fd);
    if (!f) return ERR_BADF;
    return f->m->fs->read ? f->m->fs->read(f->file, buf, len) : ERR_NOSYS;
}

int vfs_write(int fd, const void *buf, size_t len)
{
    vfd_t *f = fd_get(fd);
    if (!f) return ERR_BADF;
    return f->m->fs->write ? f->m->fs->write(f->file, buf, len) : ERR_NOSYS;
}

int vfs_seek(int fd, int32_t off, int whence)
{
    vfd_t *f = fd_get(fd);
    if (!f) return ERR_BADF;
    return f->m->fs->seek ? f->m->fs->seek(f->file, off, whence) : ERR_NOSYS;
}

int vfs_ioctl(int fd, uint32_t cmd, void *arg)
{
    vfd_t *f = fd_get(fd);
    if (!f) return ERR_BADF;
    return f->m->fs->ioctl ? f->m->fs->ioctl(f->file, cmd, arg) : ERR_NOSYS;
}

int vfs_stat(const char *path, vfs_info_t *out)
{
    char norm[VFS_PATH_MAX]; const mount_t *m; const char *rel;
    int rc = resolve(path, norm, &m, &rel);
    if (rc != ERR_OK) return rc;
    if (!m->fs->stat) return ERR_NOSYS;
    rc = m->fs->stat(m->fsdata, rel, out);
    if (rc == ERR_OK && norm[0] == '/' && norm[1] == 0) { out->name[0] = '/'; out->name[1] = 0; }
    return rc;
}

int vfs_mkdir(const char *path)
{
    char norm[VFS_PATH_MAX]; const mount_t *m; const char *rel;
    int rc = resolve(path, norm, &m, &rel);
    if (rc != ERR_OK) return rc;
    return m->fs->mkdir ? m->fs->mkdir(m->fsdata, rel) : ERR_NOSYS;
}

int vfs_unlink(const char *path)
{
    char norm[VFS_PATH_MAX]; const mount_t *m; const char *rel;
    int rc = resolve(path, norm, &m, &rel);
    if (rc != ERR_OK) return rc;
    return m->fs->unlink ? m->fs->unlink(m->fsdata, rel) : ERR_NOSYS;
}

/* Wraps the caller's callback so that "stop" from it is remembered: the
 * filesystem driver only reports a count, and we must not go on to report the
 * synthesized mount points after the caller asked to stop. */
struct walk { int (*cb)(const vfs_info_t *, void *); void *ctx; bool stopped; };

static int walk_cb(const vfs_info_t *info, void *c)
{
    struct walk *w = c;
    if (w->cb(info, w->ctx)) w->stopped = true;
    return w->stopped;
}

/* Directory listing = what the filesystem has + mount points directly below it
 * (so "ls /" shows "dev" even though M5FS does not contain it). */
int vfs_readdir(const char *path, int (*cb)(const vfs_info_t *, void *), void *ctx)
{
    char norm[VFS_PATH_MAX]; const mount_t *m; const char *rel;
    int rc = resolve(path, norm, &m, &rel);
    if (rc != ERR_OK) return rc;
    int count = 0;
    struct walk w = { cb, ctx, false };
    if (!m->fs->readdir) return ERR_NOSYS;
    rc = m->fs->readdir(m->fsdata, rel, walk_cb, &w);
    if (rc < 0) return rc;
    count = rc;
    bool stopped = w.stopped;
    size_t nl = slen(norm);
    for (int i = 0; i < VFS_MOUNT_MAX && !stopped; i++) {
        const mount_t *mp = &s_mounts[i];
        if (!mp->used || mp->len <= 1) continue;
        /* mount path = norm + "/" + name   (or "/" + name when listing the root) */
        size_t prefix = (nl == 1) ? 1 : nl + 1;
        if (mp->len <= prefix) continue;
        size_t k = 0;
        while (k < nl && mp->path[k] == norm[k]) k++;
        if (k != nl) continue;
        if (nl > 1 && mp->path[nl] != '/') continue;
        const char *name = mp->path + prefix;
        bool nested = false;
        for (const char *c = name; *c; c++) if (*c == '/') nested = true;
        if (nested) continue;
        vfs_info_t info = { .type = FS_TYPE_DIR, .size = 0 };
        size_t j = 0;
        while (name[j] && j < VFS_NAME_MAX) { info.name[j] = name[j]; j++; }
        info.name[j] = 0;
        count++;
        if (cb(&info, ctx)) stopped = true;
    }
    return count;
}

/* Close every handle an application left open (called by appd when the app ends). */
int vfs_close_owned(uint32_t app_id)
{
    int n = 0;
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (s_fds[i].used && s_fds[i].owner_app == app_id) {
            if (s_fds[i].pending) {
                uint32_t irq = arch_irq_disable();
                s_fds[i].used = false;
                s_fds[i].pending = false;
                arch_irq_restore(irq);
            } else vfs_close(i);
            n++;
        }
    }
    return n;
}

int vfs_count_owned(uint32_t app_id)
{
    int n = 0;
    for (int i = 0; i < VFS_FD_MAX; i++) if (s_fds[i].used && s_fds[i].owner_app == app_id) n++;
    return n;
}


int vfs_replace_file(const char *path,const void *data,size_t len,size_t *written)
{
    size_t ignored;
    if (!written) written = &ignored;
    *written = 0;
    if ((!data && len) || len > 0x7fffffffu) return ERR_INVAL;
    char norm[VFS_PATH_MAX];
    int rc = vfs_normalize(path,norm);
    if (rc != ERR_OK) return rc;
    const char *rel;
    /* A transient owned descriptor pins the mount while the callback runs.
     * appd can reclaim it after task termination; ordinary fd operations cannot
     * treat it as an opened file. Never hold interrupts masked across IO. */
    uint32_t irq = arch_irq_disable();
    const mount_t *m = find_mount(norm,&rel);
    int pin = ERR_BUSY;
    if (m && m->fs->replace) {
        for (int i = 0; i < VFS_FD_MAX; i++) if (!s_fds[i].used) {
            s_fds[i] = (vfd_t){ .used = true, .pending = true, .m = m,
                .owner_app = task_current() && task_current()->app ? task_current()->app->app_id : 0 };
            pin = i;
            break;
        }
    }
    arch_irq_restore(irq);
    if (!m) return ERR_NOENT;
    if (!m->fs->replace) return vfs_write_file(norm,data,len,written);
    if (pin < 0) return pin;
    rc = m->fs->replace(m->fsdata,rel,data,len,written);
    if (rc > 0 || (rc == ERR_OK && *written != len)) rc = ERR_IO;
    irq = arch_irq_disable();
    s_fds[pin].pending = false;
    s_fds[pin].used = false;
    arch_irq_restore(irq);
    return rc;
}
