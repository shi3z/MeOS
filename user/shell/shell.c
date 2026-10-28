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
#include "../viewer/imgview.h"
#include "../../drivers/lcd/fbcon.h"
#define FILER_PAUSE_SETTLE_MS 100u
#include "shell.h"
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
#include <m5os/board_config.h>
#if M5OS_HAS_KEYBOARD
#include "../../drivers/keyboard/keyboard.h"
#endif

#define LINE_MAX     128
#define ARG_MAX      8
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

typedef struct { const char *name; int (*fn)(int, char **); const char *help; } command_t;
static const command_t s_commands[];

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (const command_t *c = s_commands; c->name; c++) SH_PRINTF("  %-8s %s\n", c->name, c->help);
    return 0;
}

static int cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    task_info_t t[PS_MAX];
    int n = task_snapshot(t, PS_MAX);
    sh_puts("PID  NAME             PRI  STATE       APP\n");
    for (int i = 0; i < n; i++) {
        if (t[i].app_id) SH_PRINTF("%-4u %-16s %-4u %-11s %u\n", t[i].id, t[i].name, t[i].priority, task_state_name(t[i].state), t[i].app_id);
        else             SH_PRINTF("%-4u %-16s %-4u %-11s -\n", t[i].id, t[i].name, t[i].priority, task_state_name(t[i].state));
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
        rc = app_start_file(path, &id);
        if (rc != ERR_OK) return fail("run", path, rc);
        print_started(path, id);
        return 0;
    }
    rc = app_start(argv[1], &id);                                /* built-in first ... */
    if (rc == ERR_NOENT) {                                       /* ... then /apps/NAME.m5ex */
        ksnprintf(path, sizeof path, "/apps/%s.m5ex", argv[1]);
        rc = app_start_file(path, &id);
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
    (void)argc; (void)argv;
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

static int cmd_view(int argc, char **argv)
{
    char path[VFS_PATH_MAX];
    if (argc < 2) { SH_PRINTF("usage: view IMAGE (BMP or PPM)\n"); return 1; }
    if (!resolve(argv[1], path)) return fail("view", "bad path", ERR_INVAL);
    fbcon_pause(true);
    task_sleep(FILER_PAUSE_SETTLE_MS);
    dq_clear();
    bool ok = imgview_show(path);
    fbcon_pause(false);
    if (!ok) SH_PRINTF("view: %s: %s\n", path, imgview_last_error());
    return ok ? 0 : 1;
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
#endif

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

static const command_t s_commands[] = {
    { "help",   cmd_help,   "list commands" },
    { "ps",     cmd_ps,     "list tasks" },
    { "kill",   cmd_kill,   "kill PID        terminate one task (an app: its owner task)" },
    { "ls",     cmd_ls,     "ls [PATH]       list a directory (/dev lists devices)" },
    { "cat",    cmd_cat,    "cat FILE...     print files" },
    { "echo",   cmd_echo,   "echo TEXT [> FILE | >> FILE]" },
    { "mem",    cmd_mem,    "heap usage" },
    { "uptime", cmd_uptime, "time since boot" },
    { "run",    cmd_run,    "run [APP]       start an application (no argument: list them)" },
    { "put",    cmd_put,    "put PATH        receive a file as base64 (see tools/m5ex_push.py)" },
    { "send",   cmd_send,   "send PID TEXT   send a message to a task" },
    { "recv",   cmd_recv,   "recv [MS]       receive messages (waits up to MS, default 1000)" },
    { "tap",    cmd_tap,    "tap X Y         simulate a touch on the screen" },
    { "gui",    cmd_gui,    "list windows (front first)" },
#if M5OS_HAS_KEYBOARD
    { "files",  cmd_files,  "files [DIR]     full-screen file manager (filer)" },
    { "view",   cmd_view,   "view IMAGE      show a BMP/PPM image" },
    { "edit",   cmd_edit,   "edit FILE       full-screen text editor (^S save, ^Q quit)" },
    { "keys",   cmd_keys,   "keyboard statistics (bring-up diagnostics)" },
#endif
    { "apps",   cmd_apps,   "list running and finished applications" },
    { "stop",   cmd_stop,   "stop APP_ID     stop an application and reclaim its resources" },
    { "pwd",    cmd_pwd,    "print working directory" },
    { "cd",     cmd_cd,     "cd [DIR]" },
    { "mkdir",  cmd_mkdir,  "mkdir DIR" },
    { "rm",     cmd_rm,     "rm FILE|EMPTYDIR" },
    { "mount",  cmd_mount,  "show mounts and free space" },
    { NULL, NULL, NULL },
};

/* ----------------------------------------------------------- line editing */

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

/* Returns the line length, or -1 for a cancelled line (Ctrl-C).  echo=false is used for `put`. */
static int read_line_ex(char *line, bool echo)
{
    int len = 0;
    int last = 0;
    for (;;) {
        int c = read_byte();
        int prev = last;
        last = c;
        if (c == '\n' && prev == '\r') continue;             /* CR LF from the terminal: one Enter */
        if (c == '\r' || c == '\n') { if (echo) sh_puts("\n"); line[len] = 0; return len; }
        if (c == CH_CTRL_C) { if (echo) sh_puts("^C\n"); return -1; }
        if (c == CH_BACKSPACE || c == CH_DEL) {
            if (len > 0) { len--; if (echo) sh_puts("\b \b"); }
            continue;
        }
        if (c == CH_CTRL_U) {
            while (len > 0) { len--; if (echo) sh_puts("\b \b"); }
            continue;
        }
        if (c == CH_ESC) {                                   /* swallow ESC [ x  /  ESC O x */
            int c2 = read_byte();
            if (c2 == '[' || c2 == 'O') {
                int c3;
                do { c3 = read_byte(); } while ((c3 >= '0' && c3 <= '9') || c3 == ';');
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len < LINE_MAX - 1) {
            line[len++] = (char)c;
            char ch = (char)c;
            if (echo) sh_write(&ch, 1);
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

static bool str_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void execute(char *line)
{
    char *argv[ARG_MAX];
    int argc = tokenize(line, argv);
    if (argc == 0) return;
    for (const command_t *c = s_commands; c->name; c++) {
        if (str_eq(argv[0], c->name)) { c->fn(argc, argv); return; }
    }
    SH_PRINTF("%s: command not found (try: help)\n", argv[0]);
}

void shell_task(void *arg)
{
    (void)arg;
    s_out = vfs_open("/dev/uart", VFS_O_WRITE);
    s_in  = vfs_open("/dev/uart", VFS_O_READ);
    if (s_out < 0 || s_in < 0) { kprintf("shell: cannot open /dev/uart\n"); return; }
    int32_t forever = -1;
    vfs_ioctl(s_in, UART_IOC_SET_RX_TIMEOUT, &forever);      /* read() now waits for input */

    sh_puts("\nM5OS " M5OS_VERSION_STRING "\nBoard: " M5OS_BOARD_NAME "\nCPU: " M5OS_CPU_NAME "\nScheduler: M5OS\nFreeRTOS: disabled\n\n");
    char line[LINE_MAX];
    for (;;) {
        prompt();
        if (read_line(line) > 0) execute(line);
    }
}
