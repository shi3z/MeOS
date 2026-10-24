/*
 * M5OS shell on /dev/uart.
 *
 * Input : one byte at a time from vfs_read("/dev/uart"), with a blocking read.
 * Output: everything goes through vfs_write("/dev/uart"), so it is serialized
 *         with kprintf() by the console lock.
 * Line editing: Backspace, Ctrl-U (kill line), Ctrl-C (cancel); arrow keys and
 * other ESC sequences are swallowed.  No history yet.
 */
#include "../filer/filer.h"
#include "../tui/dqwin.h"
#include "../edit/edit.h"
#include "../camera/camera_app.h"
#include "../wifi/wifi_app.h"
#include "../ssh/ssh_app.h"
#include "../tailscale/tailscale_app.h"
#include "../web/web_app.h"
#include <m5os/wasm.h>
#if M5OS_BOARD_TAB5
#include "../wasmapps/wasmapps.h"
#include "../wasmapps/qrinstall.h"
#endif
#include "../../drivers/lcd/fbcon.h"

#define FILER_PAUSE_SETTLE_MS 100u
#include "shell.h"
#include <m5os/crash.h>
#include <m5os/health.h>
#include <m5os/wall_time.h>
#include <m5os/hal_i2c.h>
#include "hardware.h"
#include "../basic/basic.h"
#include "../basic/file.h"
#include "../basic/console.h"
#include "../basic/graphics.h"
#include <m5os/program_fs.h>
#include "../../apps/calculator/expr.h"
#include "sd.h"
#include <m5os/audio.h>
#include <m5os/player.h>
#include <m5os/microphone.h>
#include <m5os/vfs.h>
#include <m5os/task.h>
#include <m5os/kprintf.h>
#include <m5os/kmalloc.h>
#include <m5os/hal_time.h>
#include <m5os/errno.h>
#include <m5os/dev_uart.h>
#include <m5os/kernel.h>
#include <m5os/app.h>
#include <m5os/m5ex.h>
#include <m5os/crc32.h>
#include <m5os/msg.h>
#include <m5os/gui.h>
#include <m5os/wifi.h>
#include <m5os/board_config.h>
#if M5OS_HAS_KEYBOARD
#include "../../drivers/keyboard/keyboard.h"
#if M5OS_BOARD_TAB5
#include "../../drivers/tab5kbd/osk.h"
#include "../../drivers/tab5kbd/tab5touch.h"
#endif
#endif

#define LINE_MAX     (VFS_PATH_MAX+32)
#define ARG_MAX      16
#define OUT_BUF      160
#define CAT_CHUNK    128
#define PS_MAX       TASK_MAX

#define CH_BACKSPACE 0x08
#define CH_DEL       0x7F
#define CH_CTRL_C    0x03
#define CH_CTRL_U    0x15
#define CH_ESC       0x1B

static int  s_out = -1, s_in = -1;
static char s_cwd[VFS_PATH_MAX] = "/";

static bool sh_streq(const char *a, const char *b);

/* ---------------------------------------------------------------- output */

static void sh_write(const char *s, size_t n) { if (s_out >= 0) vfs_write(s_out, s, n); }
static void sh_puts(const char *s) { size_t n = 0; while (s[n]) n++; sh_write(s, n); }

/* Format into a local buffer, then one vfs_write (so the whole message is one console-locked write). */
#define SH_PRINTF(...) do { char _b[OUT_BUF]; int _n = ksnprintf(_b, sizeof _b, __VA_ARGS__); sh_write(_b, (size_t)_n); } while (0)

static const char *err_text(int rc)
{
    switch (rc) {
    case ERR_NODEV:       return "no such device";
    case ERR_BADF:        return "bad file handle";
    case ERR_INVAL:       return "invalid argument";
    case ERR_NOSYS:       return "not supported";
    case ERR_BUSY:        return "busy / table full";
    case ERR_IO:          return "I/O error";
    case ERR_NOMEM:       return "out of memory";
    case ERR_EXIST:       return "already exists";
    case ERR_NOTDIR:      return "not a directory";
    case ERR_ISDIR:       return "is a directory";
    case ERR_NOSPC:       return "no space left";
    case ERR_NAMETOOLONG: return "name too long";
    case ERR_NOENT:       return "no such file or directory";
    case ERR_NOTEMPTY:    return "directory not empty";
    case ERR_CORRUPT:     return "corrupt (checksum / filesystem)";
    case ERR_PERM:        return "permission denied";
    default:              return "error";
    }
}

static int fail(const char *cmd, const char *what, int rc)
{
    SH_PRINTF("%s: %s: %s (%d)\n", cmd, what, err_text(rc), rc);
    return 1;
}

/* ------------------------------------------------------------------ paths */

static bool resolve(const char *arg, char *out)       /* relative to cwd -> normalized absolute */
{
    char tmp[VFS_PATH_MAX * 2];
    int n = 0;
    if (arg[0] != '/') {
        for (const char *p = s_cwd; *p && n < (int)sizeof tmp - 2; p++) tmp[n++] = *p;
        if (n > 1) tmp[n++] = '/';
    }
    for (const char *p = arg; *p && n < (int)sizeof tmp - 1; p++) tmp[n++] = *p;
    tmp[n] = 0;
    if (n >= VFS_PATH_MAX) return false;
    return vfs_normalize(tmp, out) == ERR_OK;
}

/* --------------------------------------------------------------- commands */

static int read_line_ex(char *line, bool echo);
static int read_byte(void);

typedef struct { const char *name; int (*fn)(int, char **); const char *help; } command_t;
static const command_t s_commands[];

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (const command_t *c = s_commands; c->name; c++) SH_PRINTF("  %-8s %s\n", c->name, c->help);
    return 0;
}

static int cmd_i2c(int argc,char **argv) {
    (void)argc; (void)argv;hal_i2c_stats_t stats;hal_i2c_get_stats(&stats);
    kprintf("I2C transactions: ok %u, NACK %u, timeout %u, invalid %u\n",stats.ok,stats.nack,stats.timeout,stats.invalid);return 0;
}
#if M5OS_BOARD_TAB5
static int cmd_play(int argc,char **argv) {
    char path[VFS_PATH_MAX];
    if(argc!=2 || !resolve(argv[1],path)){sh_puts("usage: play \"/sd/path/track.mp3\"\n");return 1;}
    int rc=mp3_play_file(path);if(rc<0)SH_PRINTF("play: error %d\n",rc);return rc<0?1:0;
}
#endif
static int cmd_crash(int argc,char **argv) { (void)argc; (void)argv; crash_print_previous(); return 0; }

#if M5OS_BOARD_TAB5
static calc_context s_calc = { .degrees = 1 };
static int cmd_calc(int argc, char **argv)
{
    if (argc == 2 && (sh_streq(argv[1], "deg") || sh_streq(argv[1], "rad"))) {
        s_calc.degrees = sh_streq(argv[1], "deg");
        SH_PRINTF("calc: %s\n", s_calc.degrees ? "DEG" : "RAD"); return 0;
    }
    char text[LINE_MAX], output[24];
    size_t len = 0, error_pos = 0;
    if (argc < 2) { sh_puts("usage: calc EXPR | deg | rad   (sin cos tan sqrt log ln exp abs; pi e Ans)\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            if (len + 1 >= sizeof text) { sh_puts("calc: expression too long\n"); return 1; }
            text[len++] = ' ';
        }
        for (const char *p = argv[i]; *p; p++) {
            if (len + 1 >= sizeof text) { sh_puts("calc: expression too long\n"); return 1; }
            text[len++] = *p;
        }
    }
    text[len] = 0;
    double value;
    const char *error = calc_eval_context(text, &s_calc, &value, &error_pos);
    if (error) { SH_PRINTF("calc: %s (column %u)\n", error, (unsigned)error_pos + 1); return 1; }
    if (!calc_format(value, output, sizeof output)) { sh_puts("calc: cannot format result\n"); return 1; }
    SH_PRINTF("%s\n", output);
    return 0;
}
#endif

static void basic_write(const char *buf, size_t len, void *opaque)
{
    (void)opaque; sh_write(buf, len);
}
typedef struct {
    basic_session *session;
    unsigned ticks;
    basic_console console;
    basic_graphics graphics;
    char last_path[VFS_PATH_MAX];
} basic_shell;
static int basic_cancel(void *opaque)
{
    basic_shell *ctx = opaque;
    if (!(++ctx->ticks % 32)) task_sleep(1);
    return basic_console_feed(&ctx->console, kconsole_getc());
}
static const char *basic_draw(void *opaque, int op, const int32_t args[5], const char *text, size_t len)
{
    basic_shell *ctx = opaque;
    return basic_graphics_command(&ctx->graphics, op, args, text, len, basic_cancel, opaque);
}
static int basic_input_key(void *opaque)
{
    (void)opaque; return read_byte();
}
static int basic_input(void *opaque, char *buffer, size_t capacity)
{
    basic_shell *ctx = opaque;
    return basic_console_readline(&ctx->console, buffer, capacity, basic_input_key, basic_write, opaque);
}
static int basic_files(void *opaque, int action, const char *filename)
{
    basic_shell *ctx = opaque;
    char path[VFS_PATH_MAX];
    if (!*filename) filename = ctx->last_path;
    if (!*filename) { sh_puts("basic: specify a file path first\n"); return ERR_INVAL; }
    if (!resolve(filename, path)) { fail("basic", "bad path", ERR_INVAL); return ERR_INVAL; }
    if (action == BASIC_EDIT) {
#if M5OS_HAS_KEYBOARD
        vfs_info_t info;
        int rc = vfs_stat(path, &info);
        if (rc == ERR_OK && info.type != FS_TYPE_FILE) rc = ERR_INVAL;
        if (rc != ERR_OK && rc != ERR_NOENT) { fail("basic", path, rc); return rc; }
        size_t len = basic_session_export(ctx->session);
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        bool saved = edit_file_with_text(path, ctx->session->program, len);
        fbcon_pause(false);
        if (!saved) { sh_puts("basic: editor returned; current program kept\n"); return ERR_OK; }
        action = BASIC_LOAD;
#else
        sh_puts("basic: EDIT requires a keyboard\n"); return ERR_NOSYS;
#endif
    }
    basic_file_result result;
    int rc = action == BASIC_LOAD ? basic_file_load(ctx->session, path, &result)
                                 : basic_file_save(ctx->session, path, &result);
    if (rc < 0) {
        if (result.line) SH_PRINTF("basic: %s: source line %u: %s\n", path, result.line, result.detail);
        else SH_PRINTF("basic: %s: %s: %s (%d)\n", path, result.detail, err_text(rc), rc);
        return rc;
    }
    ksnprintf(ctx->last_path, sizeof ctx->last_path, "%s", path);
    SH_PRINTF("basic: %s %s (%u bytes)\n", result.detail, path, (unsigned)result.bytes);
    return ERR_OK;
}
static int cmd_basic(int argc, char **argv)
{
#if !M5OS_BOARD_TAB5
    (void)argc; (void)argv;
    sh_puts("basic: floating-point BASIC currently requires Tab5\n"); return 1;
#else

    if (argc > 2) { sh_puts("usage: basic [FILE.bas]\n"); return 1; }
    basic_session *session = kmalloc_psram(sizeof *session);
    if (!session) session = kmalloc(sizeof *session);
    if (!session) { sh_puts("basic: out of memory\n"); return 1; }
    basic_shell ctx = { .session = session };
    basic_session_init(session, basic_write, basic_cancel, &ctx);
    session->file = basic_files;
    session->input = basic_input;
    session->graphics = basic_draw;
    if (argc == 2) {
        int rc = basic_files(&ctx, BASIC_LOAD, argv[1]);
        if (rc == ERR_OK) rc = basic_session_run(session);
        kfree(session);
        return rc == 0 ? 0 : 1;
    }
    sh_puts("MeOS numeric BASIC. HELP for commands; BYE exits. SAVE writes via VFS.\n");
    char line[LINE_MAX];
    for (;;) {
        ctx.console = (basic_console){0};
        sh_puts("Ready> ");
        if (read_line_ex(line, true) <= 0) continue;
        if (basic_session_submit(session, line)) break;
    }
    kfree(session);
    return 0;
#endif
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    task_info_t t[PS_MAX];
    int n = task_snapshot(t, PS_MAX);
    sh_puts("PID  NAME             PRI E/B STATE       APP  STACK used~/size  GUARD\n");
    for (int i = 0; i < n; i++) {
        SH_PRINTF("%-4u %-16s %u/%u  %-11s ", t[i].id, t[i].name, t[i].priority, t[i].base_priority, task_state_name(t[i].state));
        if (t[i].app_id) SH_PRINTF("%u  ", t[i].app_id);
        else sh_puts("-  ");
        task_stack_info_t stack;
        int rc = task_stack_inspect(t[i].id, &stack);
        if (rc == ERR_OK) SH_PRINTF("%u/%u  %s\n", (unsigned)stack.used, (unsigned)stack.size,
                                   stack.guard_ok ? "OK" : "DAMAGED");
        else sh_puts(rc == ERR_NOSYS ? "n/a (boot stack)\n" : "n/a (task exited)\n");
    }
    return 0;
}

static bool parse_uint(const char *s, uint32_t *out)
{
    if (!*s) return false;
    uint32_t v = 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return false; v = v * 10 + (uint32_t)(*s - '0'); }
    *out = v;
    return true;
}

static int cmd_kill(int argc, char **argv)
{
    uint32_t pid;
    if (argc != 2 || !parse_uint(argv[1], &pid)) { sh_puts("usage: kill PID\n"); return 1; }
    int rc = task_kill(pid);
    if (rc == ERR_INVAL) { sh_puts("kill: refusing to kill idle (pid 1) or the shell itself\n"); return 1; }
    if (rc == ERR_NOENT) { SH_PRINTF("kill: no such process: %u\n", pid); return 1; }
    if (rc != ERR_OK) return fail("kill", argv[1], rc);
    SH_PRINTF("killed %u\n", pid);
    return 0;
}

struct ls_ctx { int n; };

static int ls_cb(const vfs_info_t *i, void *c)
{
    ((struct ls_ctx *)c)->n++;
    if (i->type == FS_TYPE_DIR)      SH_PRINTF("%s/\n", i->name);
    else if (i->type == FS_TYPE_DEV) SH_PRINTF("%-16s [device]\n", i->name);
    else                             SH_PRINTF("%-16s %u\n", i->name, i->size);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (!resolve(argc > 1 ? argv[1] : s_cwd, path)) return fail("ls", "bad path", ERR_INVAL);
    struct ls_ctx ctx = { 0 };
    int rc = vfs_readdir(path, ls_cb, &ctx);
    if (rc < 0) {
        vfs_info_t st;                                         /* a plain file: show it like `ls FILE` */
        if (rc == ERR_NOTDIR && vfs_stat(path, &st) == ERR_OK) { ls_cb(&st, &ctx); return 0; }
        return fail("ls", path, rc);
    }
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) { sh_puts("usage: cat FILE...\n"); return 1; }
    for (int a = 1; a < argc; a++) {
        char path[VFS_PATH_MAX];
        if (!resolve(argv[a], path)) return fail("cat", argv[a], ERR_INVAL);
        int fd = vfs_open(path, VFS_O_READ);
        if (fd < 0) return fail("cat", path, fd);
        char buf[CAT_CHUNK];
        int n;
        while ((n = vfs_read(fd, buf, sizeof buf)) > 0) sh_write(buf, (size_t)n);
        vfs_close(fd);
        if (n < 0) return fail("cat", path, n);
    }
    return 0;
}

static int cmd_echo(int argc, char **argv)
{
    int end = argc;
    const char *redirect = NULL;
    uint32_t flags = 0;
    if (argc >= 3 && (argv[argc - 2][0] == '>') && (argv[argc - 2][1] == 0 || (argv[argc - 2][1] == '>' && argv[argc - 2][2] == 0))) {
        redirect = argv[argc - 1];
        flags = VFS_O_WRITE | VFS_O_CREATE | (argv[argc - 2][1] == '>' ? VFS_O_APPEND : VFS_O_TRUNC);
        end = argc - 2;
    }
    int fd = s_out;
    if (redirect) {
        char path[VFS_PATH_MAX];
        if (!resolve(redirect, path)) return fail("echo", redirect, ERR_INVAL);
        fd = vfs_open(path, flags);
        if (fd < 0) return fail("echo", path, fd);
    }
    for (int i = 1; i < end; i++) {
        size_t n = 0;
        while (argv[i][n]) n++;
        vfs_write(fd, argv[i], n);
        if (i + 1 < end) vfs_write(fd, " ", 1);
    }
    vfs_write(fd, "\n", 1);
    if (redirect) vfs_close(fd);
    return 0;
}

static void print_heap(const char *name, const kmem_stats_t *st)
{
    if (!st->total) { SH_PRINTF("%-9s (not present)\n", name); return; }
    SH_PRINTF("%-9s total %u KB   used %u KB   free %u KB   largest %u KB   peak used %u KB   fails %u\n", name,
              (uint32_t)(st->total / 1024u), (uint32_t)(st->used / 1024u), (uint32_t)(st->free / 1024u),
              (uint32_t)(st->largest_free / 1024u), (uint32_t)(st->peak_used / 1024u), st->fail_count);
}

static int cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    kmem_stats_t mi, mp;
    kmem_internal_stats(&mi);
    kmem_psram_stats(&mp);
    print_heap("internal", &mi);
    print_heap("psram", &mp);
    return 0;
}

static int cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint32_t ms = (uint32_t)(hal_time_now_us() / 1000u);
    SH_PRINTF("up %u:%02u:%02u.%03u   (%u scheduler ticks)\n", ms / 3600000u, (ms / 60000u) % 60u,
              (ms / 1000u) % 60u, ms % 1000u, sched_ticks());
    return 0;
}

static void print_started(const char *what, uint32_t id)
{
    app_info_t all[APP_MAX];
    int n = app_list(all, APP_MAX);
    for (int i = 0; i < n; i++) {
        if (all[i].app_id != id) continue;
        char perms[8];
        app_perm_string(all[i].permissions, perms);
        SH_PRINTF("started %s as app %u  (permissions %s)\n", what, id, perms);
        return;
    }
    SH_PRINTF("started %s as app %u\n", what, id);
}

static int list_m5ex_cb(const vfs_info_t *i, void *ctx)
{
    size_t n = 0;
    while (i->name[n]) n++;
    if (i->type == FS_TYPE_FILE && n > 5 && i->name[n - 5] == '.' && i->name[n - 4] == 'm' && i->name[n - 3] == '5' &&
        i->name[n - 2] == 'e' && i->name[n - 1] == 'x') {
        char base[FS_NAME_MAX + 1];                    /* name without ".m5ex" (kprintf has no %.*s) */
        for (size_t k = 0; k < n - 5; k++) base[k] = i->name[k];
        base[n - 5] = 0;
        SH_PRINTF("  %-14s (%u bytes)\n", base, i->size);
        (*(int *)ctx)++;
    }
    return 0;
}

static bool has_slash(const char *s) { for (; *s; s++) if (*s == '/') return true; return false; }

static int cmd_run(int argc, char **argv)
{
    if (argc < 2) {
        sh_puts("usage: run APP | run PATH.m5ex\n(permissions: C=console D=display I=input r/w=files s=spawn m=messages)\nbuilt in:\n");
        for (int i = 0; app_def_at(i); i++) {
            char perms[8];
            app_perm_string(app_def_at(i)->permissions, perms);
            SH_PRINTF("  %-8s %s  %s\n", app_def_at(i)->name, perms, app_def_at(i)->description);
        }
        sh_puts("loadable (/apps/NAME.m5ex):\n");
        int found = 0;
        vfs_readdir("/apps", list_m5ex_cb, &found);
        if (!found) sh_puts("  (none)\n");
        return 1;
    }
    uint32_t id = 0;
    char path[VFS_PATH_MAX];
    int rc;
    if (has_slash(argv[1])) {                                    /* an explicit file */
        if (!resolve(argv[1], path)) return fail("run", argv[1], ERR_INVAL);
        size_t pl = 0;
        while (path[pl]) pl++;
        bool wasm = pl > 5 && path[pl - 5] == '.' && path[pl - 4] == 'w' && path[pl - 3] == 'a' && path[pl - 2] == 's' && path[pl - 1] == 'm';
        rc = wasm ? app_start_wasm_file(path, &id) : app_start_file(path, &id);
        if (rc != ERR_OK) return fail("run", path, rc);
        print_started(path, id);
        return 0;
    }
    rc = app_start(argv[1], &id);                                /* built-in first ... */
    if (rc == ERR_NOENT) {                                       /* ... then /apps/NAME.m5ex */
        ksnprintf(path, sizeof path, "/apps/%s.m5ex", argv[1]);
        rc = app_start_file(path, &id);
        if (rc == ERR_NOENT) {                                   /* ... then a WebAssembly app */
            ksnprintf(path, sizeof path, "/apps/%s.wasm", argv[1]);
            rc = app_start_wasm_file(path, &id);
        }
        if (rc == ERR_NOENT) { SH_PRINTF("run: no such app: %s\n", argv[1]); return 1; }
        if (rc != ERR_OK) return fail("run", path, rc);
        print_started(path, id);
        return 0;
    }
    if (rc != ERR_OK) return fail("run", argv[1], rc);
    print_started(argv[1], id);
    return 0;
}

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* put PATH: receive a file as base64 lines (no echo) ended by a line holding ".".
 * tools/m5ex_push.py drives this; the reply carries a CRC so the host can verify. */
static int cmd_put(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (argc != 2 || !resolve(argv[1], path)) { sh_puts("usage: put PATH   (then base64 lines, ending with a line \".\")\n"); return 1; }
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return fail("put", path, fd);

    char line[LINE_MAX];
    uint8_t out[LINE_MAX];
    uint32_t total = 0, crc = 0;
    int rc = ERR_OK;
    for (;;) {
        int n = read_line_ex(line, false);
        if (n < 0) { rc = ERR_IO; break; }                       /* Ctrl-C */
        if (n == 1 && line[0] == '.') break;
        uint32_t acc = 0; int bits = 0, k = 0;
        for (int i = 0; i < n && line[i] != '='; i++) {
            int v = b64val((unsigned char)line[i]);
            if (v < 0) continue;
            acc = (acc << 6) | (uint32_t)v;
            bits += 6;
            if (bits >= 8) { bits -= 8; out[k++] = (uint8_t)(acc >> bits); }
        }
        if (k) {
            int w = vfs_write(fd, out, (size_t)k);
            if (w != k) { rc = w < 0 ? w : ERR_NOSPC; break; }
            crc = crc32_update(crc, out, (size_t)k);
            total += (uint32_t)k;
        }
    }
    vfs_close(fd);
    if (rc != ERR_OK) { vfs_unlink(path); return fail("put", path, rc); }
    SH_PRINTF("put: wrote %u bytes, crc32 %08x\n", total, crc);
    return 0;
}

static int cmd_send(int argc, char **argv)
{
    uint32_t pid;
    if (argc < 3 || !parse_uint(argv[1], &pid)) { sh_puts("usage: send PID TEXT...   (text up to 23 characters)\n"); return 1; }
    msg_t m = { .type = 1 };
    int n = 0;
    for (int a = 2; a < argc; a++) {
        if (a > 2 && n < MSG_DATA_SIZE - 1) m.data[n++] = ' ';
        for (const char *p = argv[a]; *p && n < MSG_DATA_SIZE - 1; p++) m.data[n++] = (uint8_t)*p;
    }
    int rc = msg_send_timeout(pid, &m, 200);
    if (rc == ERR_NOENT) { SH_PRINTF("send: no such process: %u\n", pid); return 1; }
    if (rc == ERR_TIMEOUT) { sh_puts("send: its mailbox is full and nobody is reading it\n"); return 1; }
    if (rc != ERR_OK) return fail("send", argv[1], rc);
    SH_PRINTF("sent to pid %u: \"%s\"   (this shell is pid %u: replies arrive in its mailbox, see: recv)\n", pid, (char *)m.data, task_current()->id);
    return 0;
}

static int cmd_recv(int argc, char **argv)
{
    uint32_t ms = 1000;
    if (argc > 1 && !parse_uint(argv[1], &ms)) { sh_puts("usage: recv [MS]   wait up to MS for a message (default 1000, 0 = don't wait)\n"); return 1; }
    int got = 0;
    for (;;) {
        msg_t m;
        int rc = msg_receive_timeout(&m, got ? 0 : ms);           /* first one may wait; then drain what is already there */
        if (rc == ERR_TIMEOUT || rc == ERR_NOMSG) break;
        if (rc != ERR_OK) return fail("recv", "mailbox", rc);
        m.data[MSG_DATA_SIZE - 1] = 0;
        SH_PRINTF("from pid %u (type 0x%x): \"%s\"\n", m.sender, m.type, (char *)m.data);
        got++;
    }
    if (!got) sh_puts("(no message)\n");
    return 0;
}

#define TAP_HOLD_MS  60u

static int cmd_tap(int argc, char **argv)
{
    uint32_t x, y;
    if (argc != 3 || !parse_uint(argv[1], &x) || !parse_uint(argv[2], &y) || x >= GUI_SCREEN_W || y >= GUI_SCREEN_H) {
        sh_puts("usage: tap X Y   simulate a touch (screen 320x240; y >= 200 is the Home/Next/Close strip)\n");
        return 1;
    }
    gui_inject_touch(GUI_TOUCH_DOWN, (int)x, (int)y);
    task_sleep(TAP_HOLD_MS);
    gui_inject_touch(GUI_TOUCH_UP, (int)x, (int)y);
    return 0;
}

static int cmd_gui(int argc, char **argv)
{
    (void)argc; (void)argv;
    gui_print_windows();
    return 0;
}

#if M5OS_HAS_KEYBOARD
#if M5OS_BOARD_TAB5
static int cmd_osk(int argc, char **argv)
{
    if (!osk_available()) { sh_puts("osk: no touch controller\n"); return 0; }
    if (argc > 1 && sh_streq(argv[1], "on"))  { osk_show(true);  return 0; }
    if (argc > 1 && sh_streq(argv[1], "off")) { osk_show(false); return 0; }
    if (argc > 1 && sh_streq(argv[1], "debug")) { static bool on; on = !on; osk_set_debug(on); SH_PRINTF("osk: touch debug %s\n", on ? "on" : "off"); return 0; }
    if (argc > 2 && sh_streq(argv[1], "map")) { tab5_touch_set_map(argv[2][0] - '0'); SH_PRINTF("osk: touch map %d\n", tab5_touch_map()); return 0; }
    uint32_t frames, errors; int rx, ry, mx, my;
    tab5_touch_stats(&frames, &errors, &rx, &ry, &mx, &my);
    SH_PRINTF("osk: %s, touch map %d, %u frames, %u i2c errors, last raw %d,%d, controller resolution %d x %d\n",
              osk_visible() ? "visible" : "hidden", tab5_touch_map(), frames, errors, rx, ry, mx, my);
    return 0;
}
#endif

static int cmd_keys(int argc, char **argv)
{
    (void)argc; (void)argv;
    keyboard_stats_t st;
    keyboard_stats(&st);
    SH_PRINTF("keyboard: %s, %u key presses, last key (x=%d,y=%d), last char 0x%02x, i2c errors %u\n",
              keyboard_type(), st.presses, st.last_x, st.last_y, st.last_char < 0 ? 0 : st.last_char, st.i2c_errors);
    return 0;
}
#endif

static int cmd_apps(int argc, char **argv)
{
#if M5OS_BOARD_TAB5
    if (argc < 2 || !(argv[1][0] == '-' && argv[1][1] == 'l' && !argv[1][2])) {                  /* the list of installed apps: pick one to run */
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        wasmapps_run();
        dq_clear();
        fbcon_pause(false);
        return 0;
    }
#else
    (void)argc; (void)argv;
#endif
    app_info_t a[APP_MAX];
    int n = app_list(a, APP_MAX);
    sh_puts("ID   NAME     STATE    PID  PERMS    TASKS  MEMORY\n");
    for (int i = 0; i < n; i++) {
        char perms[8];
        app_perm_string(a[i].permissions, perms);
        if (a[i].state == APP_RUNNING)
            SH_PRINTF("%-4u %-8s %-8s %-4u %s  %-5u  %u B held\n", a[i].app_id, a[i].name, app_state_name(a[i].state), a[i].owner_task, perms, a[i].tasks, a[i].mem_bytes);
        else
            SH_PRINTF("%-4u %-8s %-8s %-4u %s  %-5u  %u B reclaimed\n", a[i].app_id, a[i].name, app_state_name(a[i].state), a[i].owner_task, perms, a[i].tasks, a[i].reclaimed_bytes);
        if(a[i].cleanup_error) SH_PRINTF("     cleanup retry pending: %d; owner memory retained\n",a[i].cleanup_error);
    }
    return 0;
}

static int cmd_stop(int argc, char **argv)
{
    uint32_t id;
    if (argc != 2 || !parse_uint(argv[1], &id)) { sh_puts("usage: stop APP_ID   (see: apps)\n"); return 1; }
    int rc = app_stop(id);
    if (rc == ERR_NOENT) { SH_PRINTF("stop: app %u is not running\n", id); return 1; }
    if (rc != ERR_OK) return fail("stop", argv[1], rc);
    SH_PRINTF("stopped app %u\n", id);
    return 0;
}

#if M5OS_HAS_KEYBOARD
static int cmd_files(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (!resolve(argc > 1 ? argv[1] : s_cwd, path)) return fail("files", "bad path", ERR_INVAL);
    fbcon_pause(true);                                  /* the filer owns the screen while it runs */
    task_sleep(FILER_PAUSE_SETTLE_MS);                  /* let a drawing fbcon pass finish */
    dq_clear();
    filer_run(path);
    fbcon_pause(false);
    return 0;
}

static int cmd_edit(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (argc < 2) { SH_PRINTF("usage: edit FILE\n"); return 1; }
    if (!resolve(argv[1], path)) return fail("edit", "bad path", ERR_INVAL);
    fbcon_pause(true);
    task_sleep(FILER_PAUSE_SETTLE_MS);
    dq_clear();
    edit_file(path);
    fbcon_pause(false);
    return 0;
}

static int cmd_camera(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    const char *dir = (argc > 1) ? argv[1] : ((s_cwd[0] == '/' && s_cwd[1] == 0) ? "/home" : s_cwd);
    if (!resolve(dir, path)) return fail("camera", "bad path", ERR_INVAL);
    fbcon_pause(true);
    task_sleep(FILER_PAUSE_SETTLE_MS);
    dq_clear();
    char saved[32] = "";
    camera_app_run(path, saved);
    dq_clear();
    fbcon_pause(false);
    if (saved[0]) SH_PRINTF("camera: last photo saved as %s/%s\n", path, saved);
    return 0;
}

static int cmd_snap(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    const char *target = (argc > 1) ? argv[1] : "/home/snap.bmp";
    if (!resolve(target, path)) return fail("snap", "bad path", ERR_INVAL);
    SH_PRINTF("snap: capturing %dx%d BMP to %s ...\n", CAM_PHOTO_W, CAM_PHOTO_H, path);
    if (!camera_capture_bmp_file(path)) {
        sh_puts("snap: capture failed\n");
        return 1;
    }
    vfs_info_t st;
    if (vfs_stat(path, &st) == ERR_OK) {
        SH_PRINTF("snap: saved %s (%u bytes)\n", path, st.size);
    }
    return 0;
}
#endif

static bool sh_streq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void print_wifi_status(void)
{
    wifi_status_t st;
    wifi_get_status(&st);
    SH_PRINTF("State       : %s (%s)\n", wifi_state_name(st.state), st.last_msg);
    SH_PRINTF("MAC         : %02X:%02X:%02X:%02X:%02X:%02X\n",
              st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    if (st.state == WIFI_STATE_CONNECTED) {
        SH_PRINTF("Connected AP: %s (ch %u, %d dBm, %s)\n",
                  st.ssid, st.channel, (int)st.rssi, wifi_auth_name(st.authmode));
        SH_PRINTF("IPv4        : %u.%u.%u.%u  netmask %u.%u.%u.%u  gw %u.%u.%u.%u\n",
                  st.ip[0], st.ip[1], st.ip[2], st.ip[3],
                  st.netmask[0], st.netmask[1], st.netmask[2], st.netmask[3],
                  st.gateway[0], st.gateway[1], st.gateway[2], st.gateway[3]);
    }
    SH_PRINTF("Saved AP    : %s   (auto-connect: %s)\n",
              st.has_saved ? st.saved_ssid : "(none)",
              st.auto_connect ? "ON" : "OFF");
    SH_PRINTF("Packets     : RX %u   TX %u\n", st.rx_packets, st.tx_packets);
}

static int cmd_wifi(int argc, char **argv)
{
#if M5OS_HAS_KEYBOARD
    if (argc < 2 || sh_streq(argv[1], "gui")) {
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        wifi_app_run();
        dq_clear();
        fbcon_pause(false);
        print_wifi_status();
        return 0;
    }
#else
    if (argc < 2) {
        print_wifi_status();
        return 0;
    }
#endif
    if (sh_streq(argv[1], "status") || sh_streq(argv[1], "st")) {
        wifi_init();
        print_wifi_status();
        return 0;
    }
    if (sh_streq(argv[1], "scan")) {
        sh_puts("wifi: scanning 2.4 GHz networks...\n");
        static wifi_ap_t aps[WIFI_MAX_SCAN_APS];
        int n = wifi_scan(aps, WIFI_MAX_SCAN_APS);
        if (n < 0) { sh_puts("wifi: scan failed\n"); return 1; }
        SH_PRINTF("Found %d network(s):\n", n);
        sh_puts("  #   RSSI     CH  SECURITY  SSID                              STATUS\n");
        for (int i = 0; i < n; i++) {
            const char *tag = aps[i].connected ? "[CONNECTED]" : (aps[i].saved ? "[SAVED]" : "");
            SH_PRINTF("  %2d  %4d dBm %2u  %-8s  %-32s  %s\n",
                      i + 1, (int)aps[i].rssi, aps[i].channel,
                      wifi_auth_name(aps[i].authmode), aps[i].ssid, tag);
        }
        return 0;
    }
    if (sh_streq(argv[1], "connect") || sh_streq(argv[1], "conn")) {
        char ssid[WIFI_SSID_MAX + 1] = "";
        char pass[WIFI_PASS_MAX + 1] = "";
        if (argc >= 3) {
            int i = 0;
            for (; argv[2][i] && i < WIFI_SSID_MAX; i++) ssid[i] = argv[2][i];
            ssid[i] = 0;
            if (argc >= 4) {
                int j = 0;
                for (; argv[3][j] && j < WIFI_PASS_MAX; j++) pass[j] = argv[3][j];
                pass[j] = 0;
            } else {
                char saved_s[WIFI_SSID_MAX + 1] = "";
                char saved_p[WIFI_PASS_MAX + 1] = "";
                if (wifi_load_saved(saved_s, saved_p, NULL) && sh_streq(saved_s, ssid)) {
                    for (int j = 0; saved_p[j] && j < WIFI_PASS_MAX; j++) { pass[j] = saved_p[j]; pass[j + 1] = 0; }
                }
            }
        } else if (!wifi_load_saved(ssid, pass, NULL) || !ssid[0]) {
            sh_puts("usage: wifi connect <SSID> [PASSWORD]\n");
            return 1;
        }
        SH_PRINTF("wifi: connecting to \"%s\" ...\n", ssid);
        if (wifi_connect(ssid, pass, true)) {
            print_wifi_status();
            return 0;
        }
        sh_puts("wifi: connection failed\n");
        print_wifi_status();
        return 1;
    }
    if (sh_streq(argv[1], "disconnect") || sh_streq(argv[1], "dc")) {
#if M5OS_HAS_KEYBOARD
        if (argc >= 3 && (sh_streq(argv[2], "gui") || sh_streq(argv[2], "-g") || sh_streq(argv[2], "--gui"))) {
            fbcon_pause(true);
            task_sleep(FILER_PAUSE_SETTLE_MS);
            dq_clear();
            wifi_app_disconnect_gui();
            dq_clear();
            fbcon_pause(false);
            print_wifi_status();
            return 0;
        }
#endif
        wifi_disconnect();
        sh_puts("wifi: disconnected\n");
        return 0;
    }
    if (sh_streq(argv[1], "auto")) {
        if (argc >= 3) {
            bool en = sh_streq(argv[2], "on") || sh_streq(argv[2], "1") || sh_streq(argv[2], "true");
            wifi_set_auto_connect(en);
        }
        wifi_status_t st;
        wifi_get_status(&st);
        SH_PRINTF("wifi: auto-connect is %s (saved AP: %s)\n",
                  st.auto_connect ? "ON" : "OFF", st.has_saved ? st.saved_ssid : "(none)");
        return 0;
    }
    if (sh_streq(argv[1], "forget")) {
        wifi_forget();
        sh_puts("wifi: cleared saved credentials\n");
        return 0;
    }
    sh_puts("usage:\n"
            "  wifi                       launch graphical Wi-Fi configuration app\n"
            "  wifi gui                   launch graphical Wi-Fi configuration app\n"
            "  wifi status                show Wi-Fi connection & IPv4 status\n"
            "  wifi scan                  scan nearby Wi-Fi networks\n"
            "  wifi connect <SSID> [PASS] connect & save credentials for auto-connect\n"
            "  wifi disconnect [gui]      disconnect Wi-Fi (or open GUI disconnect modal)\n"
            "  wifi auto [on|off]         enable/disable auto-connect\n"
            "  wifi forget                clear saved Wi-Fi credentials\n");
    return 1;
}

static int cmd_ssh(int argc, char **argv)
{
    return ssh_cmd_main(argc, argv);
}

static int cmd_tailscale(int argc, char **argv)
{
#if M5OS_HAS_KEYBOARD
    if (argc < 2 || sh_streq(argv[1], "gui")) {
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        tailscale_app_main();
        dq_clear();
        fbcon_pause(false);
        return 0;
    }
#endif
    tailscale_cmd_main(argc, argv);
    return 0;
}

static int cmd_web(int argc, char **argv)
{
#if M5OS_HAS_KEYBOARD
    if (argc < 2 || (!sh_streq(argv[1], "--dump") && !sh_streq(argv[1], "--help") && !sh_streq(argv[1], "-h"))) {
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        web_cmd_main(argc, argv);
        dq_clear();
        fbcon_pause(false);
        return 0;
    }
#endif
    return web_cmd_main(argc, argv);
}

static int cmd_install(int argc, char **argv)
{
    char path[VFS_PATH_MAX], name[16];
#if M5OS_BOARD_TAB5
    if (argc == 1) {                                             /* no file: scan a QR code with the camera */
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        bool ok = qrinstall_run(name);
        dq_clear();
        fbcon_pause(false);
        if (ok) SH_PRINTF("installed %s\n", name);
        return ok ? 0 : 1;
    }
#endif
    if (argc == 2 && (argv[1][0] == 'h' || argv[1][0] == 'm') && (has_slash(argv[1]) || argv[1][1] == 'e')) {     /* an https:// link or a meos:// code */
        fbcon_pause(true);
        task_sleep(FILER_PAUSE_SETTLE_MS);
        dq_clear();
        bool ok = qrinstall_code(argv[1], name);
        dq_clear();
        fbcon_pause(false);
        if (ok) SH_PRINTF("installed %s\n", name);
        return ok ? 0 : 1;
    }
    if (argc != 2 || !resolve(argv[1], path)) { sh_puts("usage: install [FILE.wasm | URL]   (no file: scan an app's QR code)\n"); return 1; }
    int rc = wasm_install_file(path, name);
    if (rc != ERR_OK) return fail("install", path, rc);
    SH_PRINTF("installed %s\n", name);
    return 0;
}

static int cmd_pwd(int argc, char **argv) { (void)argc; (void)argv; SH_PRINTF("%s\n", s_cwd); return 0; }


static int cmd_cd(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (!resolve(argc > 1 ? argv[1] : "/", path)) return fail("cd", "bad path", ERR_INVAL);
    vfs_info_t st;
    int rc = vfs_stat(path, &st);
    if (rc != ERR_OK) return fail("cd", path, rc);
    if (st.type == FS_TYPE_FILE) return fail("cd", path, ERR_NOTDIR);
    for (int i = 0; ; i++) { s_cwd[i] = path[i]; if (!path[i]) break; }
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (argc != 2 || !resolve(argv[1], path)) { sh_puts("usage: mkdir DIR\n"); return 1; }
    int rc = vfs_mkdir(path);
    return rc == ERR_OK ? 0 : fail("mkdir", path, rc);
}

static int cmd_rm(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (argc != 2 || !resolve(argv[1], path)) { sh_puts("usage: rm FILE|EMPTYDIR\n"); return 1; }
    size_t al = 0;
    while (argv[1][al]) al++;
    if (!has_slash(argv[1]) && al < 16 && wasm_is_installed(argv[1])) {          /* rm snake: uninstall an app (not a file called so) */
        vfs_info_t st;
        if (vfs_stat(path, &st) != ERR_OK) {
            int r = wasm_uninstall(argv[1]);
            if (r == ERR_OK) SH_PRINTF("uninstalled %s\n", argv[1]);
            return r == ERR_OK ? 0 : fail("rm", argv[1], r);
        }
    }
    size_t pl = 0;
    while (path[pl]) pl++;
    if (pl > 11 && path[0] == '/' && path[1] == 'a' && path[2] == 'p' && path[3] == 'p' && path[4] == 's' && path[5] == '/' &&
        path[pl - 5] == '.' && path[pl - 4] == 'w' && path[pl - 3] == 'a' && path[pl - 2] == 's' && path[pl - 1] == 'm') {   /* /apps/NAME.wasm */
        char name[16];
        size_t k = 0;
        for (size_t i = 6; i < pl - 5 && k < 15; i++) name[k++] = path[i];
        name[k] = 0;
        if (wasm_uninstall(name) == ERR_OK) { SH_PRINTF("uninstalled %s\n", name); return 0; }
    }
    int rc = vfs_unlink(path);
    return rc == ERR_OK ? 0 : fail("rm", path, rc);
}

static int cmd_mount(int argc, char **argv)
{
    (void)argc; (void)argv;
    vfs_print_mounts();
    fs_stat_t st;
    if (fs_statfs(&st) == ERR_OK)
        SH_PRINTF("M5FS: %u KB total, %u KB free\n", st.total_blocks * st.block_size / 1024u, st.free_blocks * st.block_size / 1024u);
    return 0;
}

#if M5OS_BOARD_TAB5
static int cmd_devices(int argc, char **argv)
{
    (void)argc; (void)argv; hardware_print_devices(); return 0;
}
static int cmd_power(int argc, char **argv)
{
    (void)argc; (void)argv; hardware_print_power(); return 0;
}
static int cmd_storage(int argc, char **argv)
{
    (void)argc; (void)argv; hardware_print_storage(); return 0;
}
#endif

/* ----------------------------------------------------------- line editing */

#define HIST_MAX     32
#define CH_CTRL_A    0x01
#define CH_CTRL_B    0x02
#define CH_CTRL_D    0x04
#define CH_CTRL_E    0x05
#define CH_CTRL_F    0x06
#define CH_CTRL_K    0x0B
#define CH_CTRL_N    0x0E
#define CH_CTRL_P    0x10

#if M5OS_BOARD_TAB5
static char (*s_history)[LINE_MAX]; /* long command history belongs in PSRAM */
#else
static char s_history[HIST_MAX][LINE_MAX];
#endif
static int  s_hist_count = 0;

static bool str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int str_copy_len(char *dst, const char *src, int max_len)
{
    int i = 0;
    while (src[i] && i < max_len - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return i;
}

static void history_add(const char *line)
{
    int i = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    if (!line[i]) return;
    if (s_hist_count > 0 && str_eq(line, s_history[(s_hist_count - 1) % HIST_MAX])) return;
#if M5OS_BOARD_TAB5
    if(!s_history)return;
#endif
    str_copy_len(s_history[s_hist_count % HIST_MAX], line, LINE_MAX);
    s_hist_count++;
}

static int cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    int first = (s_hist_count > HIST_MAX) ? (s_hist_count - HIST_MAX) : 0;
    for (int i = first; i < s_hist_count; i++) {
        SH_PRINTF("  %3d  %s\n", i + 1, s_history[i % HIST_MAX]);
    }
    return 0;
}

static const command_t s_commands[] = {
    { "basic", cmd_basic, "basic [FILE.bas] numeric BASIC" },
    { "help",    cmd_help,    "list commands" },
#if M5OS_BOARD_TAB5
#if M5OS_BOARD_TAB5
    { "mic", microphone_command, "mic test [SECONDS] bounded microphone input test" },
    { "play", cmd_play, "play PATH       MP3 player (Q stops)" },
    { "audio", audio_command, "audio test      low-volume speaker test" },
#endif
    { "programs", programs_command, "programs [status|mount|init|unmount] opt-in source store" },
    { "calc", cmd_calc, "calc EXPR | deg | rad  numeric expressions" },
    { "devices", cmd_devices, "board devices: detected, untested, or unsupported (read-only)" },
    { "power",   cmd_power,   "battery voltage/current, estimated level and charger state (read-only)" },
    { "sd", shell_sd, "sd [status|probe|mount [ro|rw]|unmount] microSD" },
    { "storage", cmd_storage, "root backing, persistence and microSD support (read-only)" },
#endif
    { "history", cmd_history, "show command history (or use Up/Down arrows)" },
    { "date", time_command, "UTC time: date [sync|set ISO8601Z]" },
    { "rtc", rtc_command, "RTC read or explicit set ISO8601Z" },
    { "i2c", cmd_i2c, "I2C transaction error counters (no probe)" },
    { "health", health_command, "progress leases and watchdog state" },
    { "crash", cmd_crash, "previous retained MeOS panic record" },
    { "ps",      cmd_ps,      "list tasks" },
    { "kill",    cmd_kill,    "kill PID        terminate one task (an app: its owner task)" },
    { "ls",      cmd_ls,      "ls [PATH]       list a directory (/dev lists devices)" },
    { "cat",     cmd_cat,     "cat FILE...     print files" },
    { "echo",    cmd_echo,    "echo TEXT [> FILE | >> FILE]" },
    { "mem",     cmd_mem,     "heap usage" },
    { "uptime",  cmd_uptime,  "time since boot" },
    { "run",     cmd_run,     "run [APP]       start an application (no argument: list them)" },
    { "put",     cmd_put,     "put PATH        receive a file as base64 (see tools/m5ex_push.py)" },
    { "send",    cmd_send,    "send PID TEXT   send a message to a task" },
    { "recv",    cmd_recv,    "recv [MS]       receive messages (waits up to MS, default 1000)" },
    { "tap",     cmd_tap,     "tap X Y         simulate a touch on the screen" },
    { "gui",     cmd_gui,     "list windows (front first)" },
#if M5OS_HAS_KEYBOARD
    { "files",   cmd_files,   "files [DIR]     full-screen file manager (filer)" },
    { "edit",    cmd_edit,    "edit FILE       full-screen text editor (^S save, ^Q quit)" },
    { "camera",  cmd_camera,  "camera [DIR]    full-screen camera viewfinder (Space:capture, Q:quit)" },
    { "cam",     cmd_camera,  "cam [DIR]       alias for camera" },
    { "snap",    cmd_snap,    "snap [FILE.bmp] capture one 640x360 BMP photo from camera" },
    { "keys",    cmd_keys,    "keyboard statistics (bring-up diagnostics)" },
#if M5OS_BOARD_TAB5
    { "osk",     cmd_osk,     "osk [on|off|map N|debug]  on-screen keyboard (touch); no argument: status" },
#endif
#endif
    { "wifi",      cmd_wifi,      "wifi [gui|scan|connect|disconnect|status|auto|forget]" },
    { "ssh",       cmd_ssh,       "ssh [user@]host [-p port] [-pw pass] [cmd...]   SSH-2 client" },
    { "tailscale", cmd_tailscale, "tailscale [up|status|ip|ping|down|gui]          Tailscale Mesh VPN" },
    { "ts",        cmd_tailscale, "ts [up|status|ip|ping|down|gui]                 alias for tailscale" },
    { "web",       cmd_web,       "web [url | --dump url]                          HTTP/HTTPS Web Browser" },
    { "browser",   cmd_web,       "browser [url]                                   alias for web" },
    { "apps",      cmd_apps,      "apps [-l]       installed WebAssembly apps (pick one to run); -l: running and finished applications" },
    { "install",   cmd_install,   "install [FILE]  install a WebAssembly app from a file, or (no file) from a QR code the camera sees" },

    { "stop",    cmd_stop,    "stop APP_ID     stop an application and reclaim its resources" },
    { "pwd",     cmd_pwd,     "print working directory" },
    { "cd",      cmd_cd,      "cd [DIR]" },
    { "mkdir",   cmd_mkdir,   "mkdir DIR" },
    { "rm",      cmd_rm,      "rm FILE|EMPTYDIR" },
    { "mount",   cmd_mount,   "show mounts and free space" },
    { NULL, NULL, NULL },
};

static int read_byte(void)
{
    uint8_t c;
    for (;;) {
        int n = vfs_read(s_in, &c, 1);
        if (n == 1) return c;
        if (n < 0) task_sleep(100);               /* console gone? do not spin */
    }
}

static void prompt(void)
{
    sh_puts("> ");
}

/* The line is UTF-8 (the Japanese input method types it); every character is one cell on the console. */
static int u8_prev(const char *line, int pos)                 /* start of the character before pos */
{
    do pos--; while (pos > 0 && ((unsigned char)line[pos] & 0xC0) == 0x80);
    return pos;
}

static int u8_len(const char *line, int pos, int len)         /* bytes of the character starting at pos */
{
    int n = 1;
    while (pos + n < len && ((unsigned char)line[pos + n] & 0xC0) == 0x80) n++;
    return n;
}

static int u8_count(const char *line, int a, int b)           /* characters in [a, b) */
{
    int n = 0;
    for (int i = a; i < b; i++) if (((unsigned char)line[i] & 0xC0) != 0x80) n++;
    return n;
}

static void line_delete(char *line, int *len, int at, int n)  /* removes n bytes at `at` */
{
    for (int i = at; i + n < *len; i++) line[i] = line[i + n];
    *len -= n;
    line[*len] = 0;
}

static void refresh_line(const char *line, int len, int pos)
{
    char buf[LINE_MAX * 2 + 16];
    int n = 0;
    buf[n++] = '\r';
    buf[n++] = '>';
    buf[n++] = ' ';
    for (int i = 0; i < len && n < (int)sizeof(buf) - 8; i++) {
        buf[n++] = line[i];
    }
    buf[n++] = '\x1b';
    buf[n++] = '[';
    buf[n++] = 'K';
    for (int i = u8_count(line, pos, len); i > 0 && n < (int)sizeof(buf) - 1; i--) {
        buf[n++] = '\b';
    }
    sh_write(buf, (size_t)n);
}

/* Returns the line length, or -1 for a cancelled line (Ctrl-C).  echo=false is used for `put`. */
static int read_line_ex(char *line, bool echo)
{
    int len = 0;
    int pos = 0;
    int last = 0;
    int hist_pos = s_hist_count;
    int first_hist = (s_hist_count > HIST_MAX) ? (s_hist_count - HIST_MAX) : 0;
    char saved_line[LINE_MAX];
    saved_line[0] = 0;
    line[0] = 0;

    for (;;) {
        int c = read_byte();
        int prev = last;
        last = c;
        if (c == '\n' && prev == '\r') continue;             /* CR LF from the terminal: one Enter */
        if (c == '\r' || c == '\n') {
            if (echo) sh_puts("\n");
            line[len] = 0;
            if (echo && len > 0) history_add(line);
            return len;
        }
        if (c == CH_CTRL_C) { if (echo) sh_puts("^C\n"); line[0] = 0; return -1; }

        if (c == CH_BACKSPACE || c == CH_DEL) {
            if (pos > 0) {
                int start = u8_prev(line, pos);
                line_delete(line, &len, start, pos - start);
                pos = start;
                if (echo) {
                    if (pos == len) sh_puts("\b \b");
                    else            refresh_line(line, len, pos);
                }
            }
            continue;
        }
        if (c == CH_CTRL_D) {
            if (pos < len) {
                line_delete(line, &len, pos, u8_len(line, pos, len));
                if (echo) refresh_line(line, len, pos);
            }
            continue;
        }
        if (c == CH_CTRL_U) {
            if (len > 0) {
                len = 0;
                pos = 0;
                line[0] = 0;
                if (echo) refresh_line(line, len, pos);
            }
            continue;
        }
        if (c == CH_CTRL_K) {
            if (pos < len) {
                len = pos;
                line[len] = 0;
                if (echo) refresh_line(line, len, pos);
            }
            continue;
        }
        if (c == CH_CTRL_A) {
            if (pos > 0) { pos = 0; if (echo) refresh_line(line, len, pos); }
            continue;
        }
        if (c == CH_CTRL_E) {
            if (pos < len) { pos = len; if (echo) refresh_line(line, len, pos); }
            continue;
        }
        if (c == CH_CTRL_B) {
            if (pos > 0) { pos = u8_prev(line, pos); if (echo) sh_puts("\b"); }
            continue;
        }
        if (c == CH_CTRL_F) {
            if (pos < len) { int n = u8_len(line, pos, len); if (echo) sh_write(&line[pos], (size_t)n); pos += n; }
            continue;
        }
        if (c == CH_CTRL_P || c == CH_CTRL_N) {
            if (!echo) continue;
            if (c == CH_CTRL_P && hist_pos > first_hist) {
                if (hist_pos == s_hist_count) { line[len] = 0; str_copy_len(saved_line, line, LINE_MAX); }
                hist_pos--;
                len = str_copy_len(line, s_history[hist_pos % HIST_MAX], LINE_MAX);
                pos = len;
                refresh_line(line, len, pos);
            } else if (c == CH_CTRL_N && hist_pos < s_hist_count) {
                hist_pos++;
                if (hist_pos == s_hist_count) len = str_copy_len(line, saved_line, LINE_MAX);
                else                          len = str_copy_len(line, s_history[hist_pos % HIST_MAX], LINE_MAX);
                pos = len;
                refresh_line(line, len, pos);
            }
            continue;
        }
        if (c == CH_ESC) {
            int c2 = read_byte();
            if (c2 == '[' || c2 == 'O') {
                char seq[16];
                int slen = 0;
                int c3;
                do {
                    c3 = read_byte();
                    if (slen < (int)sizeof(seq) - 1) seq[slen++] = (char)c3;
                } while ((c3 >= '0' && c3 <= '9') || c3 == ';');
                seq[slen] = 0;

                if (!echo) continue;

                if (c3 == 'A') { /* Up arrow: previous command in history */
                    if (hist_pos > first_hist) {
                        if (hist_pos == s_hist_count) { line[len] = 0; str_copy_len(saved_line, line, LINE_MAX); }
                        hist_pos--;
                        len = str_copy_len(line, s_history[hist_pos % HIST_MAX], LINE_MAX);
                        pos = len;
                        refresh_line(line, len, pos);
                    }
                } else if (c3 == 'B') { /* Down arrow: next command in history */
                    if (hist_pos < s_hist_count) {
                        hist_pos++;
                        if (hist_pos == s_hist_count) len = str_copy_len(line, saved_line, LINE_MAX);
                        else                          len = str_copy_len(line, s_history[hist_pos % HIST_MAX], LINE_MAX);
                        pos = len;
                        refresh_line(line, len, pos);
                    }
                } else if (c3 == 'C') { /* Right arrow */
                    if (pos < len) { int n = u8_len(line, pos, len); sh_write(&line[pos], (size_t)n); pos += n; }
                } else if (c3 == 'D') { /* Left arrow */
                    if (pos > 0) { pos = u8_prev(line, pos); sh_puts("\b"); }
                } else if (c3 == 'H') { /* Home */
                    if (pos > 0) { pos = 0; refresh_line(line, len, pos); }
                } else if (c3 == 'F') { /* End */
                    if (pos < len) { pos = len; refresh_line(line, len, pos); }
                } else if (c3 == '~') {
                    if (seq[0] == '3') { /* Delete (ESC [ 3 ~) */
                        if (pos < len) {
                            line_delete(line, &len, pos, u8_len(line, pos, len));
                            refresh_line(line, len, pos);
                        }
                    } else if (seq[0] == '1' || seq[0] == '7') { /* Home */
                        if (pos > 0) { pos = 0; refresh_line(line, len, pos); }
                    } else if (seq[0] == '4' || seq[0] == '8') { /* End */
                        if (pos < len) { pos = len; refresh_line(line, len, pos); }
                    }
                }
            }
            continue;
        }
        if (c >= 0x20 && c != 0x7F && !(c >= 0x80 && c < 0xC0)) {        /* ASCII, or the lead byte of a UTF-8 character */
            char ch[4];
            int n = 1;
            ch[0] = (char)c;
            if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
            for (int i = 1; i < n; i++) ch[i] = (char)read_byte();
            if (len + n < LINE_MAX) {
                for (int i = len + n - 1; i >= pos + n; i--) line[i] = line[i - n];
                for (int i = 0; i < n; i++) line[pos + i] = ch[i];
                pos += n;
                len += n;
                line[len] = 0;
                if (echo) {
                    if (pos == len) sh_write(ch, (size_t)n);
                    else refresh_line(line, len, pos);
                }
            }
        }
    }
}

static int read_line(char *line) { return read_line_ex(line, true); }

/* Split in place on spaces; "double quotes" group words. */
static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line;
    while (*p && argc < ARG_MAX) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

static void execute(char *line)
{
    char *argv[ARG_MAX];
    int argc = tokenize(line, argv);
    if (argc == 0) return;
    for (const command_t *c = s_commands; c->name; c++) {
        if (str_eq(argv[0], c->name)) { c->fn(argc, argv); return; }
    }
    uint32_t app_id;                                                /* a name that is an installed WebAssembly app: run it */
    if (!has_slash(argv[0]) && wasm_is_installed(argv[0])) {
        int rc = wasm_run_installed(argv[0], &app_id);
        if (rc != ERR_OK) fail(argv[0], "cannot start", rc);
        return;
    }
    SH_PRINTF("%s: command not found (try: help)\n", argv[0]);
}

void shell_task(void *arg)
{
    (void)arg;
#if M5OS_BOARD_TAB5
    if(!s_history)s_history=kmalloc_psram(HIST_MAX*sizeof *s_history);
#endif
    s_out = vfs_open("/dev/uart", VFS_O_WRITE);
    s_in  = vfs_open("/dev/uart", VFS_O_READ);
    if (s_out < 0 || s_in < 0) { kprintf("shell: cannot open /dev/uart\n"); return; }
    int32_t forever = -1;
    vfs_ioctl(s_in, UART_IOC_SET_RX_TIMEOUT, &forever);      /* read() now waits for input */

    tailscale_boot_start();

    sh_puts("\nMeOS " M5OS_VERSION_STRING "\nBoard: " M5OS_BOARD_NAME "\nCPU: " M5OS_CPU_NAME "\nScheduler: MeOS\nFreeRTOS: disabled\n\n");
    char line[LINE_MAX];
    for (;;) {
        prompt();
        if (read_line(line) > 0) execute(line);
    }
}
