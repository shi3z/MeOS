#include "filer.h"
#include "name_text.h"
#include "../tui/dqwin.h"
#include "../edit/edit.h"
#include "../camera/camera_app.h"
#include "../wifi/wifi_app.h"
#include "../ssh/ssh_app.h"
#include "../tailscale/tailscale_app.h"
#include "../web/web_app.h"
#include "../../drivers/lcd/fbcon.h"

#include <m5os/display.h>
#include <m5os/player.h>
#include <m5os/kmalloc.h>
#include <m5os/vfs.h>
#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>

#define MAX_ENTRIES   160
#define COL_WIDTH     34             /* cells per column: mark + name + size */
#define COPY_CHUNK    512
#define VIEW_BYTES    8192           /* the viewer shows the first 8 KB */
#define NAME_COLS     (VFS_NAME_MAX + 1)
#define HEADER_ROWS   3              /* title window */
#define FOOTER_ROWS   4              /* key guide + status window */
#define PATH_MAX_     VFS_PATH_MAX

typedef struct { char name[VFS_NAME_MAX + 1]; bool dir; uint32_t size; } entry_t;

/* One filer exists at a time; its state is static so that the (6 KB) shell stack does not carry it. */
static entry_t *s_ent;
static bool    s_mark[MAX_ENTRIES];
static int     s_n, s_sel, s_top;
static char    s_path[PATH_MAX_];
static char    s_status[80];
static uint8_t s_chunk[COPY_CHUNK];
static char   *s_view;

/* ---------------------------------------------------------------- paths */

static void path_join(char *out, const char *dir, const char *name)
{
    char tmp[PATH_MAX_ * 2];
    ksnprintf(tmp, sizeof tmp, "%s%s%s", dir, dir[1] ? "/" : "", name);
    if (vfs_normalize(tmp, out) != ERR_OK) out[0] = 0;
}

static void say(const char *msg) { ksnprintf(s_status, sizeof s_status, "%s", msg); }

/* ---------------------------------------------------------------- directory */

static int collect(const vfs_info_t *info, void *ctx)
{
    (void)ctx;
    if (s_n >= MAX_ENTRIES) return 0;
    entry_t *e = &s_ent[s_n++];
    int i = 0;
    for (; info->name[i] && i < VFS_NAME_MAX; i++) e->name[i] = info->name[i];
    e->name[i] = 0;
    e->dir = info->type == FS_TYPE_DIR;
    e->size = info->size;
    return 0;
}

static bool less(const entry_t *a, const entry_t *b)           /* directories first, then by name */
{
    if (a->dir != b->dir) return a->dir;
    for (int i = 0; ; i++) {
        char x = a->name[i], y = b->name[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return x < y;
        if (!x) return false;
    }
}

static void reload(const char *keep_name)
{
    s_n = 0;
    if (s_path[1]) { entry_t *e = &s_ent[s_n++]; e->name[0] = '.'; e->name[1] = '.'; e->name[2] = 0; e->dir = true; e->size = 0; }
    int first = s_n;
    int rc=vfs_readdir(s_path, collect, NULL);
    if(rc<0)ksnprintf(s_status,sizeof s_status,"directory read error %d",rc);
    else if(s_n==MAX_ENTRIES)say("list limited to 160 entries");
    for (int i = first + 1; i < s_n; i++) {                     /* insertion sort: small n, no extra memory */
        entry_t t = s_ent[i];
        int j = i - 1;
        while (j >= first && less(&t, &s_ent[j])) { s_ent[j + 1] = s_ent[j]; j--; }
        s_ent[j + 1] = t;
    }
    for (int i = 0; i < MAX_ENTRIES; i++) s_mark[i] = false;
    s_sel = 0; s_top = 0;
    if (keep_name) for (int i = 0; i < s_n; i++) {
        const char *a = s_ent[i].name, *b = keep_name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) { s_sel = i; break; }
    }
}

/* ---------------------------------------------------------------- drawing */

static int list_rows(void)  { return dq_rows() - HEADER_ROWS - FOOTER_ROWS - 2; }
static int list_cols(void)  { int c = (dq_cols() - 4) / COL_WIDTH; return c < 1 ? 1 : c; }
static int page_size(void)  { return list_rows() * list_cols(); }

static void entry_cell(int idx, int *cx, int *cy)
{
    int rel = idx - s_top, rows = list_rows();
    *cx = 2 + (rel / rows) * COL_WIDTH;
    *cy = HEADER_ROWS + 1 + rel % rows;
}

static void draw_entry(int idx)
{
    int cx, cy;
    entry_cell(idx, &cx, &cy);
    const entry_t *e = &s_ent[idx];
    char size[12];
    if (e->dir) ksnprintf(size, sizeof size, "<DIR>"); else ksnprintf(size, sizeof size, "%u", (unsigned)e->size);
    char label[96],line[COL_WIDTH+100];
    filer_name_label(e->name,label,23);
    ksnprintf(line,sizeof line,"%c%s %8s ",s_mark[idx]?'*':' ',label,size);
    if (idx == s_sel) dq_text_rev(cx, cy, line); else dq_text(cx, cy, line);
}

static bool s_full = true;                 /* frames are drawn only when needed: see the note on flicker in edit.c */

static void draw_list(void)
{
    if (s_full) dq_window(0, HEADER_ROWS, dq_cols(), list_rows() + 2);
    int end = s_top + page_size();
    for (int i = s_top; i < end; i++) {
        if (i < s_n) { draw_entry(i); continue; }
        int cx, cy;
        entry_cell(i, &cx, &cy);
        char blank[COL_WIDTH + 1];
        for (int k = 0; k < COL_WIDTH; k++) blank[k] = ' ';
        blank[COL_WIDTH] = 0;
        dq_text(cx, cy, blank);
    }
    if (s_n == 0) dq_text(2, HEADER_ROWS + 1, "(empty)  ");
}

static int marked_count(void) { int c = 0; for (int i = 0; i < s_n; i++) if (s_mark[i]) c++; return c; }

static void padded(int cx, int cy, const char *s)         /* write s and blank the rest of the line inside the window */
{
    char line[200];
    int n = 0, w = dq_cols() - cx - 2, cells = 0;
    while (s[n] && cells < w && n < (int)sizeof line - 5) {            /* UTF-8: count characters, never cut one */
        int k = 1;
        unsigned char c = (unsigned char)s[n];
        if (c >= 0xF0) k = 4; else if (c >= 0xE0) k = 3; else if (c >= 0xC0) k = 2;
        for (int i = 0; i < k && s[n]; i++) line[n] = s[n], n++;
        cells++;
    }
    while (cells < w && n < (int)sizeof line - 1) { line[n++] = ' '; cells++; }
    line[n] = 0;
    dq_text(cx, cy, line);
}

static void draw_chrome(void)
{
    int w = dq_cols(), fy = dq_rows() - FOOTER_ROWS;
    if (s_full) {
        dq_window(0, 0, w, HEADER_ROWS);
        dq_text(2, 1, "MeOS FILER");
        dq_window(0, fy, w, FOOTER_ROWS);
        dq_text(2, fy + 1, "Ent/V:view P:cam W:wifi S:ssh B:web BS:up Spc:mark C:cp M:mv D:del K:mkdir N:ren E:edit Q:quit");
    }
    padded(12, 1, s_path);
    char st[120];
    ksnprintf(st, sizeof st, "%d entries, %d marked   %s", s_n, marked_count(), s_status);
    padded(2, fy + 2, st);
}

static void redraw(void)
{
    draw_chrome();
    draw_list();
    s_full = false;
}

static void move_to(int sel)
{
    if (sel < 0) sel = 0;
    if (sel >= s_n) sel = s_n - 1;
    if (sel < 0) sel = 0;
    s_sel = sel;
    int ps = page_size();
    if (s_sel < s_top) s_top = (s_sel / ps) * ps;
    if (s_sel >= s_top + ps) s_top = (s_sel / ps) * ps;
}

/* ---------------------------------------------------------------- file operations */

static bool same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int copy_file(const char *src, const char *dst)
{
    int in = vfs_open(src, VFS_O_READ);
    if (in < 0) return in;
    int out = vfs_open(dst, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (out < 0) { vfs_close(in); return out; }
    int rc = 0;
    for (;;) {
        int n = vfs_read(in, s_chunk, sizeof s_chunk);
        if (n < 0) { rc = n; break; }
        if (n == 0) break;
        int w = vfs_write(out, s_chunk, (size_t)n);
        if (w != n) { rc = w < 0 ? w : ERR_NOSPC; break; }
    }
    vfs_close(in);
    vfs_close(out);
    if (rc) vfs_unlink(dst);                                    /* do not leave a half-written copy behind */
    return rc;
}

/* Target path for entry e given what the user typed: an existing directory -> inside it; else the path itself. */
static bool target_for(const entry_t *e, const char *typed, bool single, char *out)
{
    char abs[PATH_MAX_];
    if (typed[0] == '/') { if (vfs_normalize(typed, abs) != ERR_OK) return false; }
    else path_join(abs, s_path, typed);
    if (!abs[0]) return false;
    vfs_info_t st;
    if (vfs_stat(abs, &st) == ERR_OK && st.type == FS_TYPE_DIR) { path_join(out, abs, e->name); return out[0] != 0; }
    if (!single) return false;                                  /* several files need a directory to go into */
    for (int i = 0; ; i++) { out[i] = abs[i]; if (!abs[i]) break; }
    return true;
}

static void for_targets(int *first, int *count, int *idx_list)
{
    int c = 0;
    for (int i = 0; i < s_n; i++) if (s_mark[i] && s_ent[i].name[0] != '.') idx_list[c++] = i;
    if (c == 0 && s_n > 0 && !(s_ent[s_sel].name[0] == '.' && s_ent[s_sel].name[1] == '.')) idx_list[c++] = s_sel;
    *first = c ? idx_list[0] : -1;
    *count = c;
}

static void do_copy_move(bool move)
{
    int list[MAX_ENTRIES], first, count;
    for_targets(&first, &count, list);
    if (!count) { say(move ? "nothing to move" : "nothing to copy"); return; }
    char typed[PATH_MAX_] = "";
    ksnprintf(typed, sizeof typed, "%s", s_path);
    if (!dq_input(move ? "Move to (directory or new name):" : "Copy to (directory or new name):", typed, sizeof typed)) { say("cancelled"); return; }
    int done = 0, skipped = 0, failed = 0, last_rc = 0;
    for (int k = 0; k < count; k++) {
        const entry_t *e = &s_ent[list[k]];
        char src[PATH_MAX_], dst[PATH_MAX_];
        path_join(src, s_path, e->name);
        if (e->dir || !target_for(e, typed, count == 1, dst) || src[0] == 0) { skipped++; continue; }
        if (same(dst, src)) { skipped++; continue; }
        int rc = copy_file(src, dst);
        if (rc == 0 && move) rc = vfs_unlink(src);
        if (rc) { failed++; last_rc = rc; } else done++;
    }
    if (failed) ksnprintf(s_status, sizeof s_status, "%d done, %d failed (error %d)", done, failed, last_rc);
    else        ksnprintf(s_status, sizeof s_status, "%d %s%s", done, move ? "moved" : "copied", skipped ? " (directories/self skipped)" : "");
}

static void do_delete(void)
{
    int list[MAX_ENTRIES], first, count;
    for_targets(&first, &count, list);
    if (!count) { say("nothing to delete"); return; }
    char q[80];
    if (count == 1) ksnprintf(q, sizeof q, "Delete %s ?", s_ent[list[0]].name);
    else            ksnprintf(q, sizeof q, "Delete %d marked entries ?", count);
    if (!dq_confirm(q)) { say("cancelled"); return; }
    int done = 0, failed = 0, last_rc = 0;
    for (int k = 0; k < count; k++) {
        char p[PATH_MAX_];
        path_join(p, s_path, s_ent[list[k]].name);
        int rc = p[0] ? vfs_unlink(p) : ERR_INVAL;               /* files, and directories that are empty */
        if (rc) { failed++; last_rc = rc; } else done++;
    }
    if (failed) ksnprintf(s_status, sizeof s_status, "%d deleted, %d failed (error %d; a directory must be empty)", done, failed, last_rc);
    else        ksnprintf(s_status, sizeof s_status, "%d deleted", done);
}

static void do_mkdir(void)
{
    char name[VFS_NAME_MAX + 1] = "";
    if (!dq_input("New directory name:", name, sizeof name) || !name[0]) { say("cancelled"); return; }
    char p[PATH_MAX_];
    path_join(p, s_path, name);
    int rc = p[0] ? vfs_mkdir(p) : ERR_INVAL;
    if (rc) ksnprintf(s_status, sizeof s_status, "mkdir failed (error %d)", rc); else say("directory created");
}

static void do_rename(void)
{
    const entry_t *e = &s_ent[s_sel];
    if (s_n == 0 || (e->name[0] == '.' && e->name[1] == '.')) return;
    if (e->dir) { say("renaming a directory is not supported"); return; }
    char name[VFS_NAME_MAX + 1];
    ksnprintf(name, sizeof name, "%s", e->name);
    if (!dq_input("Rename to:", name, sizeof name) || !name[0]) { say("cancelled"); return; }
    char src[PATH_MAX_], dst[PATH_MAX_];
    path_join(src, s_path, e->name);
    path_join(dst, s_path, name);
    if (!src[0] || !dst[0] || same(src, dst)) { say("cancelled"); return; }
    int rc = copy_file(src, dst);                                 /* the file system has no rename: copy, then delete */
    if (rc == 0) rc = vfs_unlink(src);
    if (rc) ksnprintf(s_status, sizeof s_status, "rename failed (error %d)", rc); else say("renamed");
}

/* ---------------------------------------------------------------- viewer */

static void view_image_file(int idx)
{
    bool large_zoom = false;
    for (;;) {
        const entry_t *e = &s_ent[idx];
        char p[PATH_MAX_];
        path_join(p, s_path, e->name);

        uint16_t *pixels = NULL;
        int img_w = 0, img_h = 0;
        if (!p[0] || !camera_load_bmp(p, &pixels, &img_w, &img_h)) {
            ksnprintf(s_status, sizeof s_status, "cannot decode BMP image: %s", e->name);
            return;
        }

        int cols = dq_cols(), rows = dq_rows();
        int fy = rows - FOOTER_ROWS;
        int mid_rows = rows - HEADER_ROWS - FOOTER_ROWS;

        for (;;) {
            dq_window(0, 0, cols, HEADER_ROWS);
            dq_text(2, 1, "IMAGE VIEWER");
            char info[120];
            ksnprintf(info, sizeof info, "%s  (%dx%d RGB565 BMP, %u KB)  [%s]",
                      e->name, img_w, img_h, (unsigned)(e->size / 1024u),
                      large_zoom ? "Fit 960x540" : "1:1 Original");
            padded(16, 1, info);

            dq_window(0, HEADER_ROWS, cols, mid_rows);
            dq_window(0, fy, cols, FOOTER_ROWS);
            dq_text(2, fy + 1, "Z/Space:Zoom  Left/Right(h/l):Prev/Next BMP  P:Camera  Q/ESC/Enter:Close");
            padded(2, fy + 2, p);

            int max_w = DISPLAY_WIDTH - 48;
            int max_h = mid_rows * 16 - 24;
            int dw = large_zoom ? 960 : img_w;
            int dh = large_zoom ? 540 : img_h;
            if (dw > max_w) { dh = (dh * max_w) / dw; dw = max_w; }
            if (dh > max_h) { dw = (dw * max_h) / dh; dh = max_h; }
            int dx = (DISPLAY_WIDTH - dw) / 2;
            int dy = HEADER_ROWS * 16 + (mid_rows * 16 - dh) / 2;

            display_draw_rect(dx - 2, dy - 2, dw + 4, dh + 4, COLOR_WHITE);
            camera_draw_rgb565_fit(dx, dy, dw, dh, pixels, img_w, img_h);

            int k = dq_getkey();
            if (k == 'q' || k == 'Q' || k == DQK_ESC || k == DQK_ENTER) {
                kfree(pixels);
                move_to(idx);
                return;
            }
            if (k == 'z' || k == 'Z' || k == ' ') {
                large_zoom = !large_zoom;
                continue;
            }
            if (k == 'p' || k == 'P') {
                kfree(pixels);
                char saved[32] = "";
                camera_app_run(s_path, saved);
                reload(saved[0] ? saved : e->name);
                return;
            }
            if (k == DQK_RIGHT || k == DQK_DOWN || k == 'l' || k == 'j' || k == 'n') {
                int next = idx;
                for (int step = 1; step < s_n; step++) {
                    int cand = (idx + step) % s_n;
                    if (!s_ent[cand].dir && camera_is_bmp_name(s_ent[cand].name)) { next = cand; break; }
                }
                if (next != idx) { kfree(pixels); idx = next; break; }
            } else if (k == DQK_LEFT || k == DQK_UP || k == 'h' || k == 'k') {
                int prev = idx;
                for (int step = 1; step < s_n; step++) {
                    int cand = (idx + s_n - step) % s_n;
                    if (!s_ent[cand].dir && camera_is_bmp_name(s_ent[cand].name)) { prev = cand; break; }
                }
                if (prev != idx) { kfree(pixels); idx = prev; break; }
            }
        }
    }
}

static void view_file(const entry_t *e)
{
    if(mp3_is_name(e->name)){
        char path[PATH_MAX_];path_join(path,s_path,e->name);
        int rc=path[0]?mp3_play_file(path):ERR_NAMETOOLONG;
        if(rc<0)ksnprintf(s_status,sizeof s_status,"MP3 start failed (%d)",rc);
        return;
    }
    if (camera_is_bmp_name(e->name)) {
        view_image_file(s_sel);
        return;
    }
    char p[PATH_MAX_];
    path_join(p, s_path, e->name);
    int fd = p[0] ? vfs_open(p, VFS_O_READ) : ERR_INVAL;
    if (fd < 0) { ksnprintf(s_status, sizeof s_status, "cannot open (error %d)", fd); return; }
    int len = vfs_read(fd, s_view, VIEW_BYTES);
    vfs_close(fd);
    if (len >= 54 && (uint8_t)s_view[0] == 'B' && (uint8_t)s_view[1] == 'M') {
        view_image_file(s_sel);
        return;
    }
    if (len < 0) len = 0;
    s_view[len] = 0;
    for (int i = 0; i < len; i++) {                               /* show only what the 8x8 font has */
        unsigned char c = (unsigned char)s_view[i];
        if (c != '\n' && c != '\t' && (c < 0x20 || c > 0x7E)) s_view[i] = '.';
    }

    int cols = dq_cols() - 4, rows = dq_rows() - 4, top = 0;
    for (;;) {
        dq_window(0, 0, dq_cols(), dq_rows());
        char title[80];
        ksnprintf(title, sizeof title, "%s  (%d bytes%s)   Up/Down/Space:scroll  Q:close", e->name, (int)e->size, e->size > VIEW_BYTES ? ", first 8192 shown" : "");
        dq_text(2, 1, title);
        /* Split the text into screen lines (at newlines, and wrapped at the window width); draw lines top .. top+rows-1. */
        int pos = 0, line = 0, col = 0, shown = 0, bi = 0;
        char buf[200];
        bool more = false;
        while (pos <= len && !more) {
            bool eol = false, at_end = pos == len;
            char c = 0;
            if (at_end)              eol = col > 0;                    /* last, unterminated line */
            else if ((c = s_view[pos]) == '\n') { eol = true; pos++; }
            else if (col >= cols)        eol = true;                       /* wrap: do not consume the character */
            else { if (bi < (int)sizeof buf - 1) buf[bi++] = c == '\t' ? ' ' : c; col++; pos++; }
            if (!eol) { if (at_end) break; continue; }
            if (line >= top) {
                if (shown < rows) { buf[bi] = 0; dq_text(2, 3 + shown, buf); shown++; } else more = true;
            }
            line++; col = 0; bi = 0;
            if (at_end) break;
        }
        int k = dq_getkey();
        if (k == 'q' || k == 'Q' || k == DQK_ESC || k == DQK_ENTER) return;
        if ((k == DQK_DOWN || k == 'j') && more) top++;
        else if ((k == DQK_UP || k == 'k') && top > 0) top--;
        else if ((k == ' ' || k == DQK_PGDN) && more) top += rows - 1;
        else if (k == DQK_PGUP) top = top > rows - 1 ? top - (rows - 1) : 0;
        else if (k == DQK_HOME) top = 0;
    }
}

/* ---------------------------------------------------------------- main loop */

static void enter_dir(const char *name)
{
    char np[PATH_MAX_];
    if (name[0] == '.' && name[1] == '.' && !name[2]) {
        int n = 0;
        while (s_path[n]) n++;
        while (n > 1 && s_path[n - 1] != '/') n--;               /* drop the last component */
        char last[VFS_NAME_MAX + 1];
        int l = 0;
        for (int i = n; s_path[i] && l < VFS_NAME_MAX; i++) last[l++] = s_path[i];
        last[l] = 0;
        if (n > 1) n--;
        s_path[n] = 0;
        if (n == 0) { s_path[0] = '/'; s_path[1] = 0; }
        reload(last);                                            /* leave the cursor on the directory we came out of */
        return;
    }
    path_join(np, s_path, name);
    if (!np[0]) return;
    for (int i = 0; ; i++) { s_path[i] = np[i]; if (!np[i]) break; }
    reload(NULL);
}

void filer_run(const char *start_dir)
{
    if(s_ent){dq_message("File manager is already open","");return;}
    s_ent=kmalloc_psram(MAX_ENTRIES*sizeof *s_ent);
    if(!s_ent)s_ent=kmalloc(MAX_ENTRIES*sizeof *s_ent);
    if(!s_ent){dq_message("File list allocation failed","");return;}
    s_view=kmalloc_psram(VIEW_BYTES+1);
    if(!s_view)s_view=kmalloc(VIEW_BYTES+1);
    if(!s_view){kfree(s_ent);s_ent=NULL;dq_message("Preview allocation failed","");return;}
    ksnprintf(s_path, sizeof s_path, "%s", start_dir && start_dir[0] ? start_dir : "/");
    s_status[0] = 0;
    reload(NULL);
    s_full = true;
    redraw();
    for (;;) {
        int k = dq_getkey();
        int rows = list_rows();
        s_status[0] = 0;
        switch (k) {
        case 'q': case 'Q': case DQK_ESC: kfree(s_view);s_view=NULL;kfree(s_ent);s_ent=NULL;return;
        case DQK_DOWN: case 'j': move_to(s_sel + 1); break;
        case DQK_UP:   case 'k': move_to(s_sel - 1); break;
        case DQK_RIGHT: case 'l': move_to(s_sel + rows); break;
        case DQK_LEFT:  case 'h': move_to(s_sel - rows); break;
        case DQK_PGDN: move_to(s_sel + page_size()); break;
        case DQK_PGUP: move_to(s_sel - page_size()); break;
        case DQK_HOME: move_to(0); break;
        case DQK_END:  move_to(s_n - 1); break;
        case DQK_BACKSPACE: enter_dir(".."); break;
        case DQK_ENTER:
            if (s_n == 0) break;
            if (s_ent[s_sel].dir) enter_dir(s_ent[s_sel].name); else { view_file(&s_ent[s_sel]); s_full = true; }
            break;
        case 'e': case 'E':
            if (s_n && !s_ent[s_sel].dir) {
                char p[PATH_MAX_];
                path_join(p, s_path, s_ent[s_sel].name);
                if (p[0] && edit_file(p)) say("saved");
                dq_clear();
                s_full = true;
                reload(s_ent[s_sel].name);
            }
            break;
        case 'f': case 'F': {                                       /* new file */
            char name[VFS_NAME_MAX + 1] = "";
            if (dq_input("New file name:", name, sizeof name) && name[0]) {
                char p[PATH_MAX_];
                path_join(p, s_path, name);
                if (p[0]) edit_file(p);
                dq_clear();
                s_full = true;
                reload(name);
            }
            break;
        }
        case 'p': case 'P': {                                       /* launch camera */
            char saved[32] = "";
            camera_app_run(s_path, saved);
            dq_clear();
            s_full = true;
            reload(saved[0] ? saved : (s_n ? s_ent[s_sel].name : NULL));
            if (saved[0]) ksnprintf(s_status, sizeof s_status, "captured %s", saved);
            break;
        }
        case 'w': case 'W': {                                       /* launch Wi-Fi settings */
            wifi_app_run();
            dq_clear();
            s_full = true;
            reload(s_n ? s_ent[s_sel].name : NULL);
            break;
        }
        case 's': case 'S': {                                       /* launch SSH client */
            ssh_app_run();
            fbcon_pause(true);
            task_sleep(100);
            dq_clear();
            s_full = true;
            reload(s_n ? s_ent[s_sel].name : NULL);
            break;
        }
        case 't': case 'T': {                                       /* launch Tailscale VPN manager */
            tailscale_app_main();
            fbcon_pause(true);
            task_sleep(100);
            dq_clear();
            s_full = true;
            reload(s_n ? s_ent[s_sel].name : NULL);
            break;
        }
        case 'b': case 'B': {                                       /* launch Web Browser */
            web_app_run();
            fbcon_pause(true);
            task_sleep(100);
            dq_clear();
            s_full = true;
            reload(s_n ? s_ent[s_sel].name : NULL);
            break;
        }
        case 'v': case 'V': if (s_n && !s_ent[s_sel].dir) { view_file(&s_ent[s_sel]); s_full = true; } break;

        case ' ':
            if (s_n && s_ent[s_sel].name[0] != '.') { s_mark[s_sel] = !s_mark[s_sel]; move_to(s_sel + 1); }
            break;
        case 'c': case 'C': do_copy_move(false); reload(NULL); s_full = true; break;
        case 'm': case 'M': do_copy_move(true);  reload(NULL); s_full = true; break;
        case 'd': case 'D': case DQK_DEL: do_delete(); reload(NULL); s_full = true; break;
        case 'n': case 'N': do_rename(); reload(NULL); s_full = true; break;
        case 'K': do_mkdir(); reload(NULL); s_full = true; break;
        case 'r': case 'R': reload(NULL); say("reloaded"); break;
        default: continue;
        }
        redraw();
    }
}
