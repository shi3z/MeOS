/* WebAssembly applications live in /apps.  On an SD-root boot that directory is
 * persistent and authoritative.  A recovery root is intentionally volatile:
 * built-in samples are seeded on every boot and locally installed apps last
 * until reset.  Keeping one filesystem as the authority avoids a second raw
 * flash log with independent recovery and migration rules. */
#include <m5os/wasm.h>
#include "wasm_cache.h"
#include <m5os/vfs.h>
#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/mutex.h>
#include <m5os/rootfs.h>
#include <string.h>

#define MAX_APP    (512u * 1024u)

typedef struct { const char *name; const uint8_t *data; uint32_t size; } wasm_image_t;
extern const wasm_image_t g_wasm_images[];        /* apps/wasm/wasm_images.c, made by tools/wasm_embed.py */
extern const int g_wasm_image_count;

static mutex_t s_lock = MUTEX_INIT;

/* ---- /apps ---------------------------------------------------------------------------------- */

static void app_path(const char *name, char out[VFS_PATH_MAX]) { ksnprintf(out, VFS_PATH_MAX, "/apps/%s.wasm", name); }

static int put_file(const char *name, const uint8_t *data, uint32_t size)
{
    char path[VFS_PATH_MAX];
    app_path(name, path);
    if(wasm_cache_matches(path,data,size))return ERR_OK;
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return fd;
    int n = size ? vfs_write(fd, data, size) : 0;
    int closed = vfs_close(fd);
    if(closed<0)return closed;
    return n == (int)size ? ERR_OK : (n < 0 ? n : ERR_NOSPC);
}

/* ---- public --------------------------------------------------------------------------------- */

int wasm_install_image(const uint8_t *file, size_t len, char name_out[16])
{
    wasm_manifest_t m;
    if (len > MAX_APP || !wasm_read_manifest(file, len, &m)) return ERR_INVAL;
    if (m.name[0] < 0x20) return ERR_INVAL;
    int rc = put_file(m.name, file, (uint32_t)len);
    if (rc == ERR_OK && name_out) strcpy(name_out, m.name);
    return rc;
}

int wasm_install_file(const char *path, char name_out[16])
{
    vfs_info_t st;
    int rc = vfs_stat(path, &st);
    if (rc != ERR_OK) return rc;
    if (st.type != FS_TYPE_FILE) return ERR_ISDIR;
    if (st.size < 8 || st.size > MAX_APP) return ERR_INVAL;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return fd;
    uint8_t *buf = kmalloc_psram(st.size);
    if (!buf) { vfs_close(fd); return ERR_NOMEM; }
    size_t got = 0;
    int n = 0;
    while (got < st.size && (n = vfs_read(fd, buf + got, st.size - got)) > 0) got += (size_t)n;
    vfs_close(fd);
    rc = n < 0 ? n : (got == st.size ? wasm_install_image(buf, got, name_out) : ERR_IO);
    kfree(buf);
    return rc;
}

int wasm_uninstall(const char *name)
{
    char path[VFS_PATH_MAX];
    app_path(name, path);
    vfs_info_t st;
    if (vfs_stat(path, &st) != ERR_OK) return ERR_NOENT;
    return vfs_unlink(path);
}

bool wasm_is_installed(const char *name)
{
    char path[VFS_PATH_MAX];
    vfs_info_t st;
    if (!name[0] || strlen(name) > 15) return false;
    app_path(name, path);
    return vfs_stat(path, &st) == ERR_OK && st.type == FS_TYPE_FILE;
}

int wasm_run_installed(const char *name, uint32_t *app_id)
{
    char path[VFS_PATH_MAX];
    if (!wasm_is_installed(name)) return ERR_NOENT;
    app_path(name, path);
    return app_start_wasm_file(path, app_id);
}

typedef struct { wasm_manifest_t m[16]; uint32_t size[16]; int n; char names[16][FS_NAME_MAX + 1]; uint32_t fsize[16]; int nn; } list_t;

/* Only collects the names: the file system holds its lock while it calls us, so opening files from in here would deadlock. */
static int list_cb(const vfs_info_t *i, void *ctx)
{
    list_t *l = ctx;
    size_t n = strlen(i->name);
    if (i->type != FS_TYPE_FILE || n < 6 || n > FS_NAME_MAX || strcmp(i->name + n - 5, ".wasm") != 0 || l->nn >= 16 || i->size > MAX_APP) return 0;
    strcpy(l->names[l->nn], i->name);
    l->fsize[l->nn++] = i->size;
    return 0;
}

static void load_manifest(list_t *l, int idx)
{
    char path[VFS_PATH_MAX];
    ksnprintf(path, sizeof path, "/apps/%s", l->names[idx]);
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return;
    uint32_t sz = l->fsize[idx];
    uint8_t *buf = kmalloc_psram(sz);
    if (buf) {
        size_t got = 0;
        int r;
        while (got < sz && (r = vfs_read(fd, buf + got, sz - got)) > 0) got += (size_t)r;
        if (got == sz && wasm_read_manifest(buf, got, &l->m[l->n])) { l->size[l->n] = sz; l->n++; }
        kfree(buf);
    }
    vfs_close(fd);
}

int wasm_list_installed(wasm_list_cb cb, void *ctx)
{
    static list_t l;                                                   /* ~1 KB: keep it off the caller's small stack */
    mutex_lock(&s_lock);
    l.n = 0;
    l.nn = 0;
    vfs_readdir("/apps", list_cb, &l);
    for (int i = 0; i < l.nn; i++) load_manifest(&l, i);
    for (int a = 1; a < l.n; a++)                                      /* insertion sort by name */
        for (int b = a; b > 0 && strcmp(l.m[b].name, l.m[b - 1].name) < 0; b--) {
            wasm_manifest_t tm = l.m[b]; l.m[b] = l.m[b - 1]; l.m[b - 1] = tm;
            uint32_t ts = l.size[b]; l.size[b] = l.size[b - 1]; l.size[b - 1] = ts;
        }
    int n = l.n;
    wasm_manifest_t m[16]; uint32_t sz[16];
    memcpy(m, l.m, sizeof(wasm_manifest_t) * (size_t)n); memcpy(sz, l.size, sizeof(uint32_t) * (size_t)n);
    mutex_unlock(&s_lock);
    for (int i = 0; i < n && cb; i++) if (cb(&m[i], sz[i], ctx)) break;
    return n;
}

void wasm_store_boot(void)
{
    if (rootfs_is_persistent()) {
        kputs("wasm: persistent /apps authoritative\n");
        return;
    }
    int seeded = 0;
    for (int i = 0; i < g_wasm_image_count; i++) {
        int rc = put_file(g_wasm_images[i].name, g_wasm_images[i].data, g_wasm_images[i].size);
        if (rc == ERR_OK) seeded++;
        else kprintf("wasm: cannot seed %s in recovery root (%d)\n", g_wasm_images[i].name, rc);
    }
    kprintf("wasm: volatile recovery /apps seeded with %d app(s)\n", seeded);
}
