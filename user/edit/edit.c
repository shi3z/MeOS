/*
 * A small full-screen editor.  Design choices, since an OS course should say why:
 *  - One flat byte buffer with the cursor as a byte offset.  Inserting or deleting moves the tail with
 *    memmove; for files of a few KB that is faster than the screen redraw it triggers, and far simpler
 *    than a gap buffer or a line table.
 *  - Lines are not stored.  Every operation that needs one (cursor up, redraw) finds it by scanning for
 *    '\n' from a known offset.
 *  - The screen is redrawn completely after each key.  The framebuffer makes that cheap and means there
 *    is no "dirty region" bookkeeping to get wrong.
 */
#include "edit.h"
#include "../tui/dqwin.h"
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>
#include <m5os/kmalloc.h>
#include <m5os/ime.h>
#include <string.h>

#define HEADER_ROWS  3
#define FOOTER_ROWS  3
#define TAB_SPACES   2
#define STATUS_LEN   80

#define CTRL(c)      ((c) - 'a' + 1)
#define K_SAVE       CTRL('s')
#define K_QUIT       CTRL('q')

static char s_buf[EDIT_MAX + 1];
static int  s_len, s_cur, s_top, s_hscroll, s_want;
static bool s_modified, s_saved_any;
static char s_status[STATUS_LEN];
static char s_path[VFS_PATH_MAX];

/* ---- lines ----------------------------------------------------------------------- */

static int line_start(int pos) { while (pos > 0 && s_buf[pos - 1] != '\n') pos--; return pos; }
static int line_end(int pos)   { while (pos < s_len && s_buf[pos] != '\n') pos++; return pos; }

/* The text is UTF-8.  Positions are byte offsets, but the screen, the cursor column and the horizontal scroll count
 * characters: every character, wide or not, is one cell of the console font. */
static int cp_len(int pos)                                          /* bytes of the character at pos */
{
    unsigned char c = (unsigned char)s_buf[pos];
    int k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (pos + k > s_len) return 1;
    for (int i = 1; i < k; i++) if (((unsigned char)s_buf[pos + i] & 0xC0) != 0x80) return 1;
    return k;
}

static int prev_start(int pos)                                      /* start of the character before pos */
{
    int p = pos - 1;
    while (p > 0 && ((unsigned char)s_buf[p] & 0xC0) == 0x80 && pos - p < 4) p--;
    return p + cp_len(p) == pos ? p : pos - 1;
}

static int col_of(int ls, int pos)                                  /* character column of pos in the line starting at ls */
{
    int col = 0;
    for (int p = ls; p < pos; p += cp_len(p)) col++;
    return col;
}

static int pos_at_col(int ls, int le, int col)                      /* byte position of a column, clamped to the line end */
{
    int p = ls;
    while (col > 0 && p < le) { p += cp_len(p); col--; }
    return p;
}

static int vis_rows(void)  { return dq_rows() - HEADER_ROWS - FOOTER_ROWS; }
static int vis_cols(void)  { return dq_cols() - 4; }

static int line_number(int pos)            /* 1-based */
{
    int n = 1;
    for (int i = 0; i < pos; i++) if (s_buf[i] == '\n') n++;
    return n;
}

static void scroll_to_cursor(void)
{
    int ls = line_start(s_cur);
    if (ls < s_top) s_top = ls;
    int below = 0;                                          /* lines between top and the cursor's line */
    for (int i = s_top; i < ls; i++) if (s_buf[i] == '\n') below++;
    while (below >= vis_rows()) { s_top = line_end(s_top) + 1; below--; }
    int col = col_of(ls, s_cur);
    if (col < s_hscroll) s_hscroll = col;
    if (col >= s_hscroll + vis_cols()) s_hscroll = col - vis_cols() + 1;
}

/* ---- drawing --------------------------------------------------------------------- */

/* Flicker: the panel is scanned out continuously from the framebuffer, so a frame that is black for a
 * moment (clear, then draw) is seen as a flash.  Therefore the frame is drawn once (`full`), and after
 * that every text row is written in ONE pass with its background, and only if it changed. */
#define MAX_SCREEN_ROWS 64
#define ROW_CHARS       400                                         /* bytes: up to 80 cells of 3-byte characters, with room */
static struct row { char text[ROW_CHARS]; int cursor; } *s_shown;  /* PSRAM: allocated by edit_file */
static bool s_full = true;
static int s_anchor_x, s_anchor_y;                                  /* where the cursor is on screen, for the input method */

static int u8_step(const char *s)                                   /* bytes of the character at s (1 for a stray byte) */
{
    unsigned char c = (unsigned char)s[0];
    int k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    for (int i = 1; i < k; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
    return k;
}

static void row_out(int y, const char *s, int cursor_col)          /* pad to the window width, write if changed */
{
    if (y < 0 || y >= MAX_SCREEN_ROWS || !s_shown) return;
    int vw = vis_cols();
    char line[ROW_CHARS];
    int n = 0, cells = 0, cur_at = -1, cur_len = 0;
    while (s[n] && cells < vw && n < ROW_CHARS - 5) {
        int k = u8_step(s + n);
        if (cells == cursor_col) { cur_at = n; cur_len = k; }
        for (int i = 0; i < k; i++) { line[n] = s[n]; n++; }
        cells++;
    }
    while (cells < vw && n < ROW_CHARS - 1) { if (cells == cursor_col) { cur_at = n; cur_len = 1; } line[n++] = ' '; cells++; }
    line[n] = 0;
    if (!s_full && s_shown[y].cursor == cursor_col && strcmp(line, s_shown[y].text) == 0) return;   /* identical to what is on screen */
    dq_text(2, y, line);
    if (cur_at >= 0) {
        char under[5] = { 0 };
        for (int i = 0; i < cur_len; i++) under[i] = line[cur_at + i];
        dq_text_rev(2 + cursor_col, y, under);
    }
    memcpy(s_shown[y].text, line, (size_t)n + 1);
    s_shown[y].cursor = cursor_col;
}

static void draw(void)
{
    if (s_full) {
        dq_window(0, 0, dq_cols(), dq_rows());
        for (int y = 0; y < MAX_SCREEN_ROWS; y++) { s_shown[y].cursor = -2; s_shown[y].text[0] = 1; }
    }
    char head[160];
    ksnprintf(head, sizeof head, "EDIT  %s%s", s_path, s_modified ? "  [modified]" : "");
    row_out(1, head, -1);

    int vw = vis_cols(), vr = vis_rows();
    int cur_ls = line_start(s_cur);
    int pos = s_top;
    char line[ROW_CHARS];
    for (int r = 0; r < vr; r++) {
        int n = 0, cc = -1;
        if (pos <= s_len) {
            int le = line_end(pos);
            int i = pos, cells = 0;
            for (int skip = s_hscroll; skip > 0 && i < le; skip--) i += cp_len(i);
            while (i < le && cells < vw && n < ROW_CHARS - 5) {
                int k = cp_len(i);
                unsigned char c = (unsigned char)s_buf[i];
                if (k == 1 && (c < 0x20 || c >= 0x7F)) { line[n++] = '.'; i++; }   /* control or stray byte */
                else { for (int j = 0; j < k; j++) line[n++] = s_buf[i + j]; i += k; }
                cells++;
            }
            if (pos == cur_ls) {
                cc = col_of(cur_ls, s_cur) - s_hscroll;
                while (cells <= cc && n < ROW_CHARS - 1) { line[n++] = ' '; cells++; }   /* the cursor may sit past the end of the line */
                s_anchor_x = 2 + cc;
                s_anchor_y = HEADER_ROWS + r;
            }
            pos = le + 1;
        }
        line[n] = 0;
        row_out(HEADER_ROWS + r, line, cc);
    }

    int fy = dq_rows() - FOOTER_ROWS;
    row_out(fy, "^S:save  ^Q/ESC:quit  arrows/PgUp/PgDn/Home/End:move  Del/BS:delete  Tab:indent", -1);
    char st[160];
    ksnprintf(st, sizeof st, "Ln %d Col %d   %d/%d bytes   %s", line_number(s_cur), col_of(cur_ls, s_cur) + 1, s_len, EDIT_MAX, s_status);
    row_out(fy + 1, st, -1);
    s_full = false;
}

/* ---- editing --------------------------------------------------------------------- */

static void say(const char *m) { ksnprintf(s_status, sizeof s_status, "%s", m); }

static void insert(char c)
{
    if (s_len >= EDIT_MAX) { say("buffer full"); return; }
    memmove(&s_buf[s_cur + 1], &s_buf[s_cur], (size_t)(s_len - s_cur));
    s_buf[s_cur++] = c;
    s_len++;
    s_modified = true;
}

static void insert_bytes(const char *b, int n)
{
    if (s_len + n > EDIT_MAX) { say("buffer full"); return; }
    memmove(&s_buf[s_cur + n], &s_buf[s_cur], (size_t)(s_len - s_cur));
    for (int i = 0; i < n; i++) s_buf[s_cur++] = b[i];
    s_len += n;
    s_modified = true;
}

static void erase_at(int pos)                    /* delete the character at pos */
{
    if (pos < 0 || pos >= s_len) return;
    int n = cp_len(pos);
    memmove(&s_buf[pos], &s_buf[pos + n], (size_t)(s_len - pos - n));
    s_len -= n;
    s_modified = true;
}

static void set_cursor(int pos) { s_cur = pos; s_want = col_of(line_start(pos), pos); }

static void move_up(void)
{
    int ls = line_start(s_cur);
    if (ls == 0) return;
    int pend = ls - 1, pls = line_start(pend);
    s_cur = pos_at_col(pls, pend, s_want);
}

static void move_down(void)
{
    int le = line_end(s_cur);
    if (le >= s_len) return;
    int nls = le + 1, nle = line_end(nls);
    s_cur = pos_at_col(nls, nle, s_want);
}

/* ---- file ------------------------------------------------------------------------ */

static int load(void)
{
    vfs_info_t st;
    s_len = 0;
    if (vfs_stat(s_path, &st) != ERR_OK) return 0;                 /* a new file */
    if (st.type != FS_TYPE_FILE) return ERR_INVAL;
    if (st.size > EDIT_MAX) return ERR_NOSPC;
    int fd = vfs_open(s_path, VFS_O_READ);
    if (fd < 0) return fd;
    int n;
    while (s_len < EDIT_MAX && (n = vfs_read(fd, &s_buf[s_len], (size_t)(EDIT_MAX - s_len))) > 0) s_len += n;
    vfs_close(fd);
    for (int i = 0; i < s_len;) {                                  /* text only: ASCII and well-formed UTF-8, no control bytes */
        unsigned char c = (unsigned char)s_buf[i];
        if (c < 0x80) {
            if ((c < 0x20 && c != '\n' && c != '\t' && c != '\r') || c == 0x7F) return ERR_INVAL;
            i++;
            continue;
        }
        if (c < 0xC2 || c > 0xF4) return ERR_INVAL;
        int k = cp_len(i);
        if (k == 1) return ERR_INVAL;
        i += k;
    }
    return 0;
}

static bool save(void)
{
    int fd = vfs_open(s_path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) { ksnprintf(s_status, sizeof s_status, "save failed: cannot open (error %d)", fd); return false; }
    int w = s_len ? vfs_write(fd, s_buf, (size_t)s_len) : 0;
    vfs_close(fd);
    if (w != s_len) { ksnprintf(s_status, sizeof s_status, "save failed: wrote %d of %d bytes (error %d)", w, s_len, w < 0 ? w : ERR_NOSPC); return false; }
    s_modified = false;
    s_saved_any = true;
    say("saved");
    return true;
}

bool edit_file(const char *path)
{
    ksnprintf(s_path, sizeof s_path, "%s", path);
    if (!s_shown) s_shown = kmalloc_psram(sizeof(struct row) * MAX_SCREEN_ROWS);
    if (!s_shown) { dq_message("Out of memory.", NULL); return false; }
    s_full = true;
    s_status[0] = 0; s_cur = s_top = s_hscroll = s_want = 0; s_modified = s_saved_any = false;
    int rc = load();
    if (rc == ERR_NOSPC) { dq_message("File is larger than the editor buffer", "(32 KB)."); return false; }
    if (rc == ERR_INVAL) { dq_message("Cannot edit: not a UTF-8 text file.", NULL); return false; }
    if (rc < 0)          { dq_message("Cannot open the file.", NULL); return false; }

    for (;;) {
        scroll_to_cursor();
        draw();
        ime_set_anchor(s_anchor_x, s_anchor_y);                     /* Japanese input is composed at the cursor */
        int k = dq_getkey();
        s_status[0] = 0;
        switch (k) {
        case K_SAVE: save(); break;
        case K_QUIT: case DQK_ESC:
            if (!s_modified || dq_confirm("Discard unsaved changes?")) { ime_clear_anchor(); return s_saved_any; }
            s_full = true;                                           /* the dialog covered the text */
            break;
        case DQK_UP:    move_up(); break;
        case DQK_DOWN:  move_down(); break;
        case DQK_LEFT:  if (s_cur > 0) set_cursor(prev_start(s_cur)); break;
        case DQK_RIGHT: if (s_cur < s_len) set_cursor(s_cur + cp_len(s_cur)); break;
        case DQK_HOME:  set_cursor(line_start(s_cur)); break;
        case DQK_END:   set_cursor(line_end(s_cur)); break;
        case DQK_PGUP:  for (int i = 0; i < vis_rows() - 1; i++) move_up(); break;
        case DQK_PGDN:  for (int i = 0; i < vis_rows() - 1; i++) move_down(); break;
        case DQK_ENTER: insert('\n'); set_cursor(s_cur); break;
        case DQK_BACKSPACE: if (s_cur > 0) { int st = prev_start(s_cur); erase_at(st); set_cursor(st); } break;
        case DQK_DEL:   erase_at(s_cur); set_cursor(s_cur); break;
        case '\t':      for (int i = 0; i < TAB_SPACES; i++) insert(' '); set_cursor(s_cur); break;
        default:
            if (k >= 0x20 && k < 0x7F) { insert((char)k); set_cursor(s_cur); }
            else if (k >= 0xC2 && k <= 0xF4) {                       /* a UTF-8 character from the input method */
                char b[4];
                int n = k >= 0xF0 ? 4 : k >= 0xE0 ? 3 : 2;
                b[0] = (char)k;
                for (int i = 1; i < n; i++) { int c = dq_getkey(); b[i] = (char)(c < 0 ? 0 : c); }
                insert_bytes(b, n);
                set_cursor(s_cur);
            }
            break;
        }
    }
}
