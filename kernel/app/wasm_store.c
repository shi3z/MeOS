/*
 * Installed WebAssembly apps: a small append-only log in the flash region "wasmapps" (0xA00000, 2 MB), mirrored into
 * /apps/NAME.wasm on the RAM-disk root at every boot.  Concept: kernel/include/m5os/wasm.h.
 *
 *   record = 32-byte header | data padded to 4 bytes
 *   header = magic "WAPP" | state | size | crc32 | name[16]
 *   state 0xFFFFFFFF = installed, 0 = removed.  Flash can turn 1-bits into 0-bits without an erase, so uninstalling is
 *   one word written in place; the space comes back when the log is compacted (erase + rewrite) because it is full.
 *   The magic word is written LAST: a record cut short by a power failure looks like the end of the log.
 *
 * Flash is touched with the ROM routines and interrupts off for one small chunk or one sector at a time (an erase
 * takes ~45 ms), like the SSH key store.
 */
#include <m5os/wasm.h>
#include <m5os/vfs.h>
#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/mutex.h>
#include <m5os/crc32.h>
#include <m5os/arch.h>
#include <m5os/task.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_rom_spiflash.h"

#define BASE       0x00A00000u
#define SIZE       0x00200000u
#define SECT       4096u
#define HDR        32u
#define MAGIC      0x50504157u
#define LIVE       0xFFFFFFFFu
#define MAX_APP    (512u * 1024u)
#define FORMAT_SECTORS 64u                       /* erased up front when the log is new; more are erased as it grows */
#define SEEDED     "\x01seeded"

typedef struct { uint32_t magic, state, size, crc; char name[16]; } rec_t;
_Static_assert(sizeof(rec_t) == HDR, "record header is 32 bytes");

typedef struct { const char *name; const uint8_t *data; uint32_t size; } wasm_image_t;
extern const wasm_image_t g_wasm_images[];        /* apps/wasm/wasm_images.c, made by tools/wasm_embed.py */
extern const int g_wasm_image_count;

extern esp_rom_spiflash_result_t esp_rom_spiflash_unlock(void);

static mutex_t s_lock = MUTEX_INIT;
static bool s_ready;
static uint32_t s_end, s_erased;                  /* where the next record goes; flash from s_end to s_erased is known erased */

/* ---- flash ---------------------------------------------------------------------------------- */

static bool IRAM_ATTR fl_read(uint32_t off, void *dst, uint32_t n)
{
    uint32_t f = arch_irq_disable();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_read(BASE + off, (uint32_t *)dst, n);
    arch_irq_restore(f);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}

static bool IRAM_ATTR fl_write(uint32_t off, const void *src, uint32_t n)
{
    uint32_t f = arch_irq_disable();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_write(BASE + off, (const uint32_t *)src, n);
    arch_irq_restore(f);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}

static bool IRAM_ATTR fl_erase(uint32_t sector_off)
{
    uint32_t f = arch_irq_disable();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_erase_sector((BASE + sector_off) / SECT);
    arch_irq_restore(f);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}

/* in pieces, so that interrupts are never off for long */
static bool read_big(uint32_t off, void *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 1024u) {
        uint32_t c = n - i < 1024u ? n - i : 1024u;
        if (!fl_read(off + i, (uint8_t *)dst + i, (c + 3u) & ~3u)) return false;
    }
    return true;
}

static bool write_big(uint32_t off, const uint8_t *src, uint32_t n)       /* n and off multiples of 4 */
{
    for (uint32_t i = 0; i < n; i += 1024u) {
        uint32_t c = n - i < 1024u ? n - i : 1024u;
        if (!fl_write(off + i, src + i, c)) return false;
    }
    return true;
}

static uint32_t rec_len(uint32_t size) { return HDR + ((size + 3u) & ~3u); }

/* ---- the log -------------------------------------------------------------------------------- */

static bool scan(void)
{
    uint32_t off = 0;
    for (;;) {
        rec_t r;
        if (off + HDR > SIZE) { s_end = SIZE; return true; }
        if (!fl_read(off, &r, HDR)) return false;
        if (r.magic == 0xFFFFFFFFu) { s_end = off; return true; }
        if (r.magic != MAGIC || r.size > MAX_APP || off + rec_len(r.size) > SIZE) return false;
        off += rec_len(r.size);
    }
}

static bool format(void)
{
    for (uint32_t i = 0; i < FORMAT_SECTORS; i++) {
        if (!fl_erase(i * SECT)) return false;
        if (i % 8 == 7) task_sleep(1);
    }
    s_end = 0;
    s_erased = FORMAT_SECTORS * SECT;
    return true;
}

static bool find(const char *name, uint32_t *off_out, rec_t *out)
{
    uint32_t off = 0;
    while (off < s_end) {
        rec_t r;
        if (!fl_read(off, &r, HDR)) return false;
        if (r.state == LIVE && strncmp(r.name, name, sizeof r.name) == 0) { *off_out = off; if (out) *out = r; return true; }
        off += rec_len(r.size);
    }
    return false;
}

static bool ensure_erased(uint32_t upto)
{
    while (s_erased < upto) {
        if (s_erased >= SIZE || !fl_erase(s_erased)) return false;
        s_erased += SECT;
    }
    return true;
}

static int append_raw(const char *name, const uint8_t *data, uint32_t size)
{
    uint32_t total = rec_len(size);
    if (s_end + total > SIZE) return ERR_NOSPC;
    if (!ensure_erased(s_end + total)) return ERR_IO;
    rec_t r;
    memset(&r, 0, sizeof r);
    r.magic = MAGIC; r.state = LIVE; r.size = size; r.crc = crc32(data, size);
    strncpy(r.name, name, sizeof r.name - 1);
    if (!fl_write(s_end + 4, (const uint8_t *)&r + 4, HDR - 4)) return ERR_IO;
    uint32_t whole = size & ~3u;
    if (whole && !write_big(s_end + HDR, data, whole)) return ERR_IO;
    if (size & 3u) {
        uint8_t tail[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        memcpy(tail, data + whole, size & 3u);
        if (!fl_write(s_end + HDR + whole, tail, 4)) return ERR_IO;
    }
    if (!fl_write(s_end, &r.magic, 4)) return ERR_IO;                 /* the record exists from this word on */
    s_end += total;
    return ERR_OK;
}

static int mark_removed(uint32_t off)
{
    uint32_t zero = 0;
    return fl_write(off + 4, &zero, 4) ? ERR_OK : ERR_IO;
}

/* Copies the live records to PSRAM, erases the log and writes them back. */
static int compact(void)
{
    uint32_t live = 0, off = 0;
    while (off < s_end) {
        rec_t r;
        if (!fl_read(off, &r, HDR)) return ERR_IO;
        if (r.state == LIVE) live += rec_len(r.size);
        off += rec_len(r.size);
    }
    uint8_t *buf = live ? kmalloc_psram(live) : NULL;
    if (live && !buf) return ERR_NOMEM;
    uint32_t used = 0;
    off = 0;
    while (off < s_end) {
        rec_t r;
        fl_read(off, &r, HDR);
        if (r.state == LIVE && !read_big(off, buf + used, rec_len(r.size))) { kfree(buf); return ERR_IO; }
        if (r.state == LIVE) used += rec_len(r.size);
        off += rec_len(r.size);
    }
    for (uint32_t s = 0; s < s_end; s += SECT) { if (!fl_erase(s)) { kfree(buf); return ERR_IO; } }
    s_erased = (s_end + SECT - 1) & ~(SECT - 1);
    if (s_erased < FORMAT_SECTORS * SECT) s_erased = s_end;
    s_end = 0;
    for (uint32_t p = 0; p < used;) {
        const rec_t *r = (const rec_t *)(buf + p);
        int rc = append_raw(r->name, buf + p + HDR, r->size);
        if (rc != ERR_OK) { kfree(buf); return rc; }
        p += rec_len(r->size);
    }
    kfree(buf);
    return ERR_OK;
}

/* ---- /apps mirror --------------------------------------------------------------------------- */

static void app_path(const char *name, char out[VFS_PATH_MAX]) { ksnprintf(out, VFS_PATH_MAX, "/apps/%s.wasm", name); }

static int put_file(const char *name, const uint8_t *data, uint32_t size)
{
    char path[VFS_PATH_MAX];
    app_path(name, path);
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return fd;
    int n = size ? vfs_write(fd, data, size) : 0;
    vfs_close(fd);
    return n == (int)size ? ERR_OK : (n < 0 ? n : ERR_NOSPC);
}

/* ---- public --------------------------------------------------------------------------------- */

int wasm_install_image(const uint8_t *file, size_t len, char name_out[16])
{
    wasm_manifest_t m;
    if (len > MAX_APP || !wasm_read_manifest(file, len, &m)) return ERR_INVAL;
    if (m.name[0] < 0x20) return ERR_INVAL;
    mutex_lock(&s_lock);
    int rc = s_ready ? ERR_OK : ERR_IO;
    uint32_t old = 0;
    bool had = rc == ERR_OK && find(m.name, &old, NULL);
    if (rc == ERR_OK) {
        rc = append_raw(m.name, file, (uint32_t)len);
        if (rc == ERR_NOSPC) {                                       /* full: squeeze out the removed apps and try again */
            if (had) { mark_removed(old); had = false; }
            rc = compact();
            if (rc == ERR_OK) rc = append_raw(m.name, file, (uint32_t)len);
        } else if (rc == ERR_OK && had) {
            mark_removed(old);                                       /* the new copy is in; retire the old one */
        }
    }
    mutex_unlock(&s_lock);
    if (rc == ERR_OK) rc = put_file(m.name, file, (uint32_t)len);
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
    mutex_lock(&s_lock);
    uint32_t off;
    bool had = s_ready && find(name, &off, NULL);
    int rc = had ? mark_removed(off) : ERR_NOENT;
    mutex_unlock(&s_lock);
    vfs_info_t st;
    bool file = vfs_stat(path, &st) == ERR_OK;
    if (file) vfs_unlink(path);
    return rc == ERR_OK || file ? ERR_OK : ERR_NOENT;
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
    if (i->type != FS_TYPE_FILE || n < 6 || strcmp(i->name + n - 5, ".wasm") != 0 || l->nn >= 16 || i->size > MAX_APP) return 0;
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
    esp_rom_spiflash_unlock();
    mutex_lock(&s_lock);
    if (!scan()) {
        kprintf("wasm: app store is new or damaged, formatting it\n");
        if (!format()) { kprintf("wasm: cannot use the flash app store\n"); mutex_unlock(&s_lock); return; }
    } else {
        s_erased = SIZE;                                              /* an intact log: everything after its end is erased */
    }
    s_ready = true;
    uint32_t off;
    bool seeded = find(SEEDED, &off, NULL);
    mutex_unlock(&s_lock);

    if (!seeded) {                                                    /* first boot with this store: install the samples once */
        for (int i = 0; i < g_wasm_image_count; i++) {
            char nm[16];
            int rc = wasm_install_image(g_wasm_images[i].data, g_wasm_images[i].size, nm);
            if (rc != ERR_OK) kprintf("wasm: cannot install %s (%d)\n", g_wasm_images[i].name, rc);
        }
        mutex_lock(&s_lock);
        append_raw(SEEDED, (const uint8_t *)"", 0);
        mutex_unlock(&s_lock);
    }
    /* rebuild /apps from flash */
    mutex_lock(&s_lock);
    off = 0;
    int restored = 0;
    while (off < s_end) {
        rec_t r;
        if (!fl_read(off, &r, HDR)) break;
        if (r.state == LIVE && r.name[0] >= 0x20 && r.size && r.size <= MAX_APP) {
            uint8_t *buf = kmalloc_psram(r.size + 4);
            if (buf && read_big(off + HDR, buf, r.size) && crc32(buf, r.size) == r.crc && put_file(r.name, buf, r.size) == ERR_OK) restored++;
            kfree(buf);
        }
        off += rec_len(r.size);
    }
    mutex_unlock(&s_lock);
    kprintf("wasm: %d app(s) installed\n", restored);
}
