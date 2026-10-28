/*
 * WebAssembly host: starts a .wasm file as an app and gives it the "meos" imports.  Concept and manifest format:
 * kernel/include/m5os/wasm.h.  The interpreter is wasm3 (third_party/wasm3, MIT).
 */
#include <m5os/wasm.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include <m5os/vfs.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/display.h>
#include <m5os/hal_time.h>
#include <m5os/task.h>
#include <m5os/suspend.h>
#include <string.h>
#include "app_internal.h"
#include "wasm3.h"
#include "../../drivers/lcd/fbcon.h"
#include <m5os/ime.h>

#define WASM_MAX_FILE   (1024u * 1024u)
#define WASM_STACK_SIZE 16384u              /* native task stack: the interpreter and the compiler recurse */
#define WASM_VSTACK     (48u * 1024u)       /* the WebAssembly value stack */
#define MEM_KB_DEFAULT  512u
#define MEM_KB_MAX      8192u
#define HDR             16u                 /* allocation header: keeps 16-byte alignment */

/* ---- memory: owner-tagged, capped per app ------------------------------------------------------ */

typedef struct { uint32_t app_id; uint32_t used, limit; } budget_t;
static budget_t s_budget[APP_MAX];

static budget_t *budget_of_current(void)
{
    task_t *t = task_current();
    if (!t || !t->app) return NULL;
    uint32_t id = *(uint32_t *)t->app;                   /* app_t starts with app_id */
    for (int i = 0; i < APP_MAX; i++) if (s_budget[i].app_id == id) return &s_budget[i];
    return NULL;
}

static void budget_register(uint32_t id, uint32_t limit)
{
    for (int i = 0; i < APP_MAX; i++)
        if (s_budget[i].app_id == id) { s_budget[i].used = 0; s_budget[i].limit = limit; return; }
    for (int i = 0; i < APP_MAX; i++)
        if (s_budget[i].app_id == 0) { s_budget[i] = (budget_t){ id, 0, limit }; return; }
    s_budget[0] = (budget_t){ id, 0, limit };            /* all slots taken by dead apps: reuse the first */
}

void *m5os_wasm_alloc(size_t n)
{
    budget_t *b = budget_of_current();
    if (!b || b->used + n + HDR > b->limit) return NULL;
    uint8_t *p = umalloc_psram(n + HDR);
    if (!p) return NULL;
    memset(p, 0, n + HDR);
    ((uint32_t *)p)[0] = (uint32_t)n;
    ((uint32_t *)p)[1] = (uint32_t)(b - s_budget);
    b->used += (uint32_t)n + HDR;
    return p + HDR;
}

void m5os_wasm_free(void *ptr)
{
    if (!ptr) return;
    uint8_t *p = (uint8_t *)ptr - HDR;
    uint32_t n = ((uint32_t *)p)[0], slot = ((uint32_t *)p)[1];
    if (slot < APP_MAX && s_budget[slot].used >= n + HDR) s_budget[slot].used -= n + HDR;
    kfree(p);
}

/* ---- the "meos" imports ------------------------------------------------------------------------- */

#define NEED(perm)  do { if (!task_has_perm(perm)) { m3ApiReturn(ERR_PERM); } } while (0)

m3ApiRawFunction(h_console_write)           /* i32 console_write(ptr, len) */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, ptr)
    m3ApiGetArg(int32_t, len)
    NEED(APP_PERM_CONSOLE);
    if (len < 0 || len > 4096) m3ApiReturn(ERR_INVAL);
    m3ApiCheckMem(ptr, (uint32_t)len);
    kconsole_write(ptr, (size_t)len);
    m3ApiReturn(len);
}

static void present_now(void);

m3ApiRawFunction(h_console_read)            /* i32 console_read(): a byte, or -1 if none is waiting */
{
    m3ApiReturnType(int32_t)
    NEED(APP_PERM_CONSOLE);
    present_now();                                  /* a game polls its keys once per frame: show the finished frame */
    m3ApiReturn(kconsole_getc());
}

m3ApiRawFunction(h_sleep_ms)
{
    m3ApiGetArg(uint32_t, ms)
    present_now();
    task_sleep(ms > 60000u ? 60000u : ms);
    m3ApiSuccess();
}

m3ApiRawFunction(h_uptime_ms)
{
    m3ApiReturnType(uint32_t)
    m3ApiReturn((uint32_t)(hal_time_now_us() / 1000u));
}

static bool path_arg(const char *p, int32_t len, char out[VFS_PATH_MAX])
{
    if (len <= 0 || len >= VFS_PATH_MAX) return false;
    memcpy(out, p, (size_t)len);
    out[len] = 0;
    return true;
}

m3ApiRawFunction(h_file_open)               /* i32 file_open(path, len, flags): handle or -ERR (permissions: r / w) */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArgMem(const char *, p)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(uint32_t, flags)
    m3ApiCheckMem(p, (uint32_t)(len > 0 ? len : 0));
    char path[VFS_PATH_MAX];
    if (!path_arg(p, len, path)) m3ApiReturn(ERR_INVAL);
    m3ApiReturn(vfs_open(path, flags & 0x1Fu));         /* the VFS checks APP_PERM_FS_READ / FS_WRITE for this app */
}

m3ApiRawFunction(h_file_read)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, h)
    m3ApiGetArgMem(uint8_t *, buf)
    m3ApiGetArg(int32_t, len)
    if (len < 0) m3ApiReturn(ERR_INVAL);
    m3ApiCheckMem(buf, (uint32_t)len);
    m3ApiReturn(vfs_read(h, buf, (size_t)len));
}

m3ApiRawFunction(h_file_write)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, h)
    m3ApiGetArgMem(const uint8_t *, buf)
    m3ApiGetArg(int32_t, len)
    if (len < 0) m3ApiReturn(ERR_INVAL);
    m3ApiCheckMem(buf, (uint32_t)len);
    m3ApiReturn(vfs_write(h, buf, (size_t)len));
}

m3ApiRawFunction(h_file_close)
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, h)
    m3ApiReturn(vfs_close(h));
}

/* The console (fbcon) draws on the same screen; an app that draws itself takes it over with display_open() and
 * gives it back with display_close() - or the kernel does when the app ends. */
static uint32_t s_display_owner;
/* Double buffering: while an app holds the screen everything it draws goes into an off-screen copy, and the changed
 * rectangle is copied to the panel in one go (display_present) - at the app's sleep_ms() / key poll, or when it asks.
 * The picture therefore never shows an erased-but-not-yet-redrawn state, which is what flickered. */
static uint16_t *s_back;
static int s_dx0, s_dy0, s_dx1, s_dy1;                   /* bounding box of what changed since the last present (empty: x1 < x0) */

static void dirty_reset(void) { s_dx0 = DISPLAY_WIDTH; s_dy0 = DISPLAY_HEIGHT; s_dx1 = -1; s_dy1 = -1; }
static void dirty_add(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x < s_dx0) s_dx0 = x;
    if (y < s_dy0) s_dy0 = y;
    if (x + w - 1 > s_dx1) s_dx1 = x + w - 1;
    if (y + h - 1 > s_dy1) s_dy1 = y + h - 1;
}
static void present_now(void)
{
    suspend_wait_if_active();                       /* a suspended screen shows nothing: do not spin the game */
    if (s_back && s_dx1 >= s_dx0 && s_dy1 >= s_dy0) {
        display_present(s_dx0, s_dy0, s_dx1 - s_dx0 + 1, s_dy1 - s_dy0 + 1);
        dirty_reset();
    }
}
static bool s_prev_paused;                   /* fbcon was already paused when the app took the screen (e.g. by the app list) */

static uint32_t current_app_id(void) { task_t *t = task_current(); return t && t->app ? *(uint32_t *)t->app : 0; }

static void release_screen(void)
{
    s_display_owner = 0;
    display_set_target(NULL);                       /* the buffer itself is the app's: freed with it */
    s_back = NULL;
    ime_bypass(false);
    kconsole_set_focus(0);
    fbcon_pause(s_prev_paused);
}

void wasm_app_cleanup(uint32_t app_id)
{
    if (s_display_owner && s_display_owner == app_id) release_screen();
}

m3ApiRawFunction(h_display_open)            /* i32 display_open(): the screen is yours (cleared to black); 0, or -ERR_BUSY */
{
    m3ApiReturnType(int32_t)
    NEED(APP_PERM_DISPLAY);
    if (s_display_owner && s_display_owner != current_app_id()) m3ApiReturn(ERR_BUSY);
    if (!s_display_owner) s_prev_paused = fbcon_is_paused();
    s_display_owner = current_app_id();
    kconsole_set_focus(task_current()->id);          /* the keys are the game's, not the shell's */
    fbcon_pause(true);
    task_sleep(60);                                  /* let fbcon finish the pass it is in */
    ime_bypass(true);                                /* raw keys: a letter is a command here, not the start of a Japanese word */
    s_back = kmalloc_psram_owned(DISPLAY_BUFFER_BYTES, current_app_id());
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0);       /* the screen itself, black */
    if (s_back) {
        memset(s_back, 0, DISPLAY_BUFFER_BYTES);
        display_set_target(s_back);                  /* from here on drawing is off-screen until presented */
        dirty_reset();
    }                                                /* no memory for it: drawing straight to the screen, as before */
    m3ApiReturn(0);
}

m3ApiRawFunction(h_display_close)
{
    m3ApiReturnType(int32_t)
    if (s_display_owner == current_app_id()) release_screen();
    m3ApiReturn(0);
}

m3ApiRawFunction(h_display_blit)            /* i32 display_blit(x, y, w, h, rgb565 pixels, scale): w*h*2 bytes at the pointer */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x) m3ApiGetArg(int32_t, y) m3ApiGetArg(int32_t, w) m3ApiGetArg(int32_t, h)
    m3ApiGetArgMem(const uint16_t *, px)
    m3ApiGetArg(int32_t, scale)
    NEED(APP_PERM_DISPLAY);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || scale < 1 || scale > 8 || ((uintptr_t)px & 1u)) m3ApiReturn(ERR_INVAL);
    if (x < 0 || y < 0 || x + w * scale > DISPLAY_WIDTH || y + h * scale > DISPLAY_HEIGHT) m3ApiReturn(ERR_INVAL);
    m3ApiCheckMem(px, (uint32_t)(w * h * 2));
    display_draw_bitmap_scaled(x, y, w, h, px, scale);
    if (s_back) dirty_add(x, y, w * scale, h * scale);
    m3ApiReturn(0);
}

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

m3ApiRawFunction(h_display_line)            /* i32 display_line(x0, y0, x1, y1, rgb565) */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x0) m3ApiGetArg(int32_t, y0) m3ApiGetArg(int32_t, x1) m3ApiGetArg(int32_t, y1) m3ApiGetArg(uint32_t, color)
    NEED(APP_PERM_DISPLAY);
    display_draw_line(x0, y0, x1, y1, (uint16_t)color);
    if (s_back) dirty_add(imin(x0, x1), imin(y0, y1), imax(x0, x1) - imin(x0, x1) + 1, imax(y0, y1) - imin(y0, y1) + 1);
    m3ApiReturn(0);
}

m3ApiRawFunction(h_display_tri)             /* i32 display_tri(x0, y0, x1, y1, x2, y2, rgb565): a filled triangle */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x0) m3ApiGetArg(int32_t, y0) m3ApiGetArg(int32_t, x1) m3ApiGetArg(int32_t, y1)
    m3ApiGetArg(int32_t, x2) m3ApiGetArg(int32_t, y2) m3ApiGetArg(uint32_t, color)
    NEED(APP_PERM_DISPLAY);
    display_fill_triangle(x0, y0, x1, y1, x2, y2, (uint16_t)color);
    if (s_back) {
        int lx = imin(x0, imin(x1, x2)), hx = imax(x0, imax(x1, x2)), ly = imin(y0, imin(y1, y2)), hy = imax(y0, imax(y1, y2));
        dirty_add(lx, ly, hx - lx + 1, hy - ly + 1);
    }
    m3ApiReturn(0);
}

m3ApiRawFunction(h_display_present)         /* i32 display_present(): show what has been drawn since the last time */
{
    m3ApiReturnType(int32_t)
    NEED(APP_PERM_DISPLAY);
    present_now();
    m3ApiReturn(0);
}

m3ApiRawFunction(h_display_size)            /* i32 display_size(): width in the low 16 bits, height in the high 16 */
{
    m3ApiReturnType(int32_t)
    NEED(APP_PERM_DISPLAY);
    m3ApiReturn((int32_t)(DISPLAY_WIDTH | (DISPLAY_HEIGHT << 16)));
}

m3ApiRawFunction(h_display_fill)            /* i32 display_fill(x, y, w, h, rgb565) */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x) m3ApiGetArg(int32_t, y) m3ApiGetArg(int32_t, w) m3ApiGetArg(int32_t, h) m3ApiGetArg(uint32_t, color)
    NEED(APP_PERM_DISPLAY);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) m3ApiReturn(ERR_INVAL);
    display_fill_rect(x, y, w, h, (uint16_t)color);
    if (s_back) dirty_add(x, y, w, h);
    m3ApiReturn(0);
}

m3ApiRawFunction(h_display_text)            /* i32 display_text(x, y, utf8, len, fg, bg, scale) */
{
    m3ApiReturnType(int32_t)
    m3ApiGetArg(int32_t, x) m3ApiGetArg(int32_t, y)
    m3ApiGetArgMem(const char *, s)
    m3ApiGetArg(int32_t, len)
    m3ApiGetArg(uint32_t, fg) m3ApiGetArg(uint32_t, bg) m3ApiGetArg(int32_t, scale)
    NEED(APP_PERM_DISPLAY);
    if (len < 0 || len > 255 || scale < 1 || scale > 8) m3ApiReturn(ERR_INVAL);
    m3ApiCheckMem(s, (uint32_t)len);
    char buf[256];
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    display_draw_text(x, y, buf, (uint16_t)fg, (uint16_t)bg, scale);
    if (s_back) {
        int chars = 0;
        for (int i = 0; i < len; i++) if (((uint8_t)buf[i] & 0xC0) != 0x80) chars++;
        dirty_add(x, y, chars * 8 * scale, 8 * scale);
    }
    m3ApiReturn(0);
}

static const struct { const char *name, *sig; M3RawCall fn; } IMPORTS[] = {
    { "console_write", "i(ii)",       h_console_write }, { "console_read", "i()",          h_console_read },
    { "sleep_ms",      "v(i)",        h_sleep_ms },      { "uptime_ms",    "i()",          h_uptime_ms },
    { "file_open",     "i(iii)",      h_file_open },     { "file_read",    "i(iii)",       h_file_read },
    { "file_write",    "i(iii)",      h_file_write },    { "file_close",   "i(i)",         h_file_close },
    { "display_open",  "i()",         h_display_open },  { "display_close", "i()",        h_display_close },
    { "display_blit",  "i(iiiiii)",   h_display_blit },  { "display_present", "i()",      h_display_present },
    { "display_line",  "i(iiiii)",    h_display_line },  { "display_tri",   "i(iiiiiii)",  h_display_tri },
    { "display_size",  "i()",         h_display_size },  { "display_fill", "i(iiiii)",     h_display_fill },
    { "display_text",  "i(iiiiiii)",  h_display_text },
};

/* ---- running ------------------------------------------------------------------------------------ */

typedef struct { uint8_t *wasm; uint32_t len, mem_limit; } wasm_ctx_t;

static void wasm_task_entry(void *arg)
{
    const wasm_ctx_t *ctx = arg;
    task_t *t = task_current();
    budget_register(*(uint32_t *)t->app, ctx->mem_limit);

    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = env ? m3_NewRuntime(env, WASM_VSTACK, NULL) : NULL;
    if (!rt) { kprintf("wasm: out of memory\n"); return; }
    IM3Module mod = NULL;
    M3Result r = m3_ParseModule(env, &mod, ctx->wasm, ctx->len);
    if (!r) r = m3_LoadModule(rt, mod);
    if (r) { kprintf("wasm: cannot load module: %s\n", r); return; }
    for (unsigned i = 0; i < sizeof IMPORTS / sizeof IMPORTS[0]; i++)
        m3_LinkRawFunction(mod, "meos", IMPORTS[i].name, IMPORTS[i].sig, IMPORTS[i].fn);   /* not imported: fine */

    IM3Function f = NULL;
    r = m3_FindFunction(&f, rt, "_start");
    if (r) r = m3_FindFunction(&f, rt, "main");
    if (!r) r = m3_CallV(f);
    if (r) {
        M3ErrorInfo info;
        m3_GetErrorInfo(rt, &info);
        kprintf("wasm: %s%s%s\n", r, info.message ? ": " : "", info.message ? info.message : "");
    }
    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);
}

/* ---- manifest + launcher ------------------------------------------------------------------------ */

static bool leb(const uint8_t *p, const uint8_t *end, uint32_t *v, size_t *used)
{
    uint32_t r = 0;
    for (int i = 0; i < 5 && p + i < end; i++) {
        r |= (uint32_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) { *v = r; *used = (size_t)i + 1; return true; }
    }
    return false;
}

bool wasm_read_manifest(const uint8_t *f, size_t len, wasm_manifest_t *m)
{
    static const char NAME[] = "meos.manifest";
    if (len < 8 || memcmp(f, "\0asm\1\0\0\0", 8) != 0) return false;
    memset(m, 0, sizeof *m);
    m->mem_kb = MEM_KB_DEFAULT;
    const uint8_t *p = f + 8, *end = f + len;
    while (p + 2 <= end) {
        uint8_t id = *p++;
        uint32_t size; size_t n;
        if (!leb(p, end, &size, &n)) return false;
        p += n;
        if (size > (size_t)(end - p)) return false;
        if (id == 0) {
            uint32_t nl; size_t nn;
            if (leb(p, p + size, &nl, &nn) && nl == sizeof NAME - 1 && nn + nl <= size && memcmp(p + nn, NAME, nl) == 0) {
                const char *t = (const char *)p + nn + nl, *te = (const char *)p + size;
                bool have_name = false;
                while (t < te) {
                    const char *e = t;
                    while (e < te && *e != '\n') e++;
                    const char *eq = t;
                    while (eq < e && *eq != '=') eq++;
                    if (eq < e) {
                        size_t kl = (size_t)(eq - t), vl = (size_t)(e - eq - 1);
                        const char *v = eq + 1;
                        if (kl == 4 && !memcmp(t, "name", 4)) {
                            if (vl == 0 || vl >= sizeof m->name) return false;
                            for (size_t i = 0; i < vl; i++) {
                                char c = v[i];
                                if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
                                m->name[i] = c;
                            }
                            have_name = true;
                        } else if (kl == 5 && !memcmp(t, "perms", 5)) {
                            static const char L[] = "CDIrwsm";
                            for (size_t i = 0; i < vl; i++) {
                                const char *q = memchr(L, v[i], 7);
                                if (!q) return false;
                                m->perms |= 1u << (q - L);
                            }
                        } else if (kl == 3 && !memcmp(t, "mem", 3)) {
                            uint32_t kb = 0;
                            for (size_t i = 0; i < vl; i++) { if (v[i] < '0' || v[i] > '9') return false; kb = kb * 10 + (uint32_t)(v[i] - '0'); if (kb > MEM_KB_MAX) return false; }
                            if (kb < 64) return false;
                            m->mem_kb = kb;
                        } else if (kl == 7 && !memcmp(t, "version", 7)) {
                            for (size_t i = 0; i < vl && i < sizeof m->version - 1; i++) m->version[i] = v[i];
                        }
                    }
                    t = e + 1;
                }
                return have_name;
            }
        }
        p += size;
    }
    return false;
}

int app_start_wasm_image(const uint8_t *file, size_t len, uint32_t *app_id)
{
    wasm_manifest_t m;
    if (len > WASM_MAX_FILE || !wasm_read_manifest(file, len, &m)) return ERR_INVAL;
    app_t *a = app_reserve(m.name, m.perms);
    if (!a) return ERR_BUSY;
    wasm_ctx_t *ctx = kmalloc_internal_owned(sizeof *ctx, a->app_id);
    uint8_t *copy = kmalloc_psram_owned(len, a->app_id);          /* the interpreter reads the module in place */
    if (!ctx || !copy) { app_abort(a); return ERR_NOMEM; }
    memcpy(copy, file, len);
    ctx->wasm = copy; ctx->len = (uint32_t)len; ctx->mem_limit = m.mem_kb * 1024u;
    int rc = app_launch(a, wasm_task_entry, ctx, TASK_PRIO_DEFAULT, WASM_STACK_SIZE);
    if (rc == ERR_OK && app_id) *app_id = a->app_id;
    return rc;
}

int app_start_wasm_file(const char *path, uint32_t *app_id)
{
    vfs_info_t st;
    int rc = vfs_stat(path, &st);
    if (rc != ERR_OK) return rc;
    if (st.type != FS_TYPE_FILE) return ERR_ISDIR;
    if (st.size < 8 || st.size > WASM_MAX_FILE) return ERR_INVAL;
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return fd;
    uint8_t *buf = kmalloc_psram(st.size);
    if (!buf) { vfs_close(fd); return ERR_NOMEM; }
    size_t got = 0;
    int n = 0;
    while (got < st.size && (n = vfs_read(fd, buf + got, st.size - got)) > 0) got += (size_t)n;
    vfs_close(fd);
    rc = n < 0 ? n : (got == st.size ? app_start_wasm_image(buf, got, app_id) : ERR_IO);
    kfree(buf);
    return rc;
}
