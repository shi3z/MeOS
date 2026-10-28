#include "wasmapps.h"
#include "../tui/dqwin.h"
#include "qrinstall.h"
#include <m5os/wasm.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <string.h>

#define HEADER_ROWS 3
#define FOOTER_ROWS 3
#define MAX_APPS    16

static wasm_manifest_t s_app[MAX_APPS];
static uint32_t s_size[MAX_APPS];
static int s_n, s_sel;
static char s_status[80];

static int collect(const wasm_manifest_t *m, uint32_t size, void *ctx)
{
    (void)ctx;
    if (s_n < MAX_APPS) { s_app[s_n] = *m; s_size[s_n] = size; s_n++; }
    return 0;
}

static void reload(const char *keep)
{
    s_n = 0;
    wasm_list_installed(collect, NULL);
    s_sel = 0;
    for (int i = 0; keep && i < s_n; i++) if (strcmp(s_app[i].name, keep) == 0) s_sel = i;
}

static void padded(int cx, int cy, const char *s, bool rev)
{
    char line[200];
    int n = 0, w = dq_cols() - cx - 2;
    while (s[n] && n < w && n < (int)sizeof line - 1) { line[n] = s[n]; n++; }
    while (n < w && n < (int)sizeof line - 1) line[n++] = ' ';
    line[n] = 0;
    if (rev) dq_text_rev(cx, cy, line); else dq_text(cx, cy, line);
}

static void draw(void)
{
    int w = dq_cols(), fy = dq_rows() - FOOTER_ROWS;
    dq_window(0, 0, w, HEADER_ROWS);
    dq_text(2, 1, "MeOS APPS");
    dq_window(0, HEADER_ROWS, w, fy - HEADER_ROWS);
    dq_window(0, fy, w, FOOTER_ROWS);
    dq_text(2, fy + 1, "Up/Down:select  Enter:run  I:install (scan QR)  X:uninstall  ^C:quit");
    for (int y = HEADER_ROWS + 1; y < fy - 1; y++) padded(2, y, "", false);
    if (s_n == 0) padded(2, HEADER_ROWS + 1, "(no apps - press I to install one)", false);
    for (int i = 0; i < s_n; i++) {
        char perms[8], line[100];
        app_perm_string(s_app[i].perms, perms);
        ksnprintf(line, sizeof line, "%-16s v%-8s %s  %4u KB  mem %u KB", s_app[i].name, s_app[i].version[0] ? s_app[i].version : "-",
                  perms, (unsigned)((s_size[i] + 1023u) / 1024u), (unsigned)s_app[i].mem_kb);
        padded(2, HEADER_ROWS + 1 + i, line, i == s_sel);
    }
    padded(2, fy - 1, s_status, false);
}

static void run_selected(void)
{
    uint32_t id = 0;
    int rc = wasm_run_installed(s_app[s_sel].name, &id);
    if (rc != ERR_OK) { ksnprintf(s_status, sizeof s_status, "cannot start %s (error %d)", s_app[s_sel].name, rc); return; }
    for (;;) {                                           /* the app owns the screen and the keys until it ends */
        app_info_t ai[APP_MAX];
        int n = app_list(ai, APP_MAX);
        bool running = false;
        for (int i = 0; i < n; i++) if (ai[i].app_id == id && ai[i].state == APP_RUNNING) running = true;
        if (!running) break;
        task_sleep(100);
    }
    dq_clear();
    s_status[0] = 0;
}

void wasmapps_run(void)
{
    dq_clear();
    reload(NULL);
    for (;;) {
        draw();
        int k = dq_getkey();
        if (k == 0x03 || k == 'q' || k == 'Q' || k == DQK_ESC) break;           /* ^C quits */
        if (k == DQK_UP && s_sel > 0) s_sel--;
        else if (k == DQK_DOWN && s_sel + 1 < s_n) s_sel++;
        else if ((k == DQK_ENTER || k == ' ') && s_n) run_selected();
        else if ((k == 'x' || k == 'X') && s_n) {
            if (dq_confirm("Uninstall this app?")) {
                char name[16];
                strcpy(name, s_app[s_sel].name);
                int rc = wasm_uninstall(name);
                ksnprintf(s_status, sizeof s_status, rc == ERR_OK ? "uninstalled %s" : "uninstall failed (%d)", name, rc);
                dq_clear();
                reload(NULL);
            } else dq_clear();
        } else if (k == 'i' || k == 'I') {
            char name[16];
            bool added = qrinstall_run(name);
            dq_clear();
            reload(added ? name : NULL);
            if (added) ksnprintf(s_status, sizeof s_status, "installed %s", name);
        }
    }
}
