#include "fbcon.h"
#include <m5os/display.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/hal_time.h>
#include <m5os/arch.h>
#include <m5os/kmalloc.h>

#define SCALE     M5OS_FBCON_SCALE
#define CELL      (8 * SCALE)
#define COLS      (DISPLAY_WIDTH / CELL)
#define ROWS      (DISPLAY_HEIGHT / CELL)
#define POLL_MS   40u
#define BLINK_MS  500u
#define HISTORY   1500              /* how much already-printed output to show when starting */
#define COL_FG    RGB565(210, 255, 210)
#define COL_BG    COLOR_BLACK

static uint16_t s_scr[ROWS][COLS];
static bool s_dirty[ROWS];
static int  s_rows = ROWS;        /* rows in use: fbcon_set_rows() shrinks the console (on-screen keyboard below it) */
static int  s_row, s_col;
static volatile bool s_paused;
static bool s_cursor_on;
static int  s_cursor_row = -1, s_cursor_col;

static uint8_t  s_esc_state; /* 0=normal, 1=saw ESC, 2=in CSI '[', 3=in OSC ']', 4=skip 1 byte */
static char     s_csi_buf[32];
static uint8_t  s_csi_len;
static uint32_t s_utf8_cp;
static uint8_t  s_utf8_rem;

static void clear_row(int r)
{
    for (int c = 0; c < COLS; c++) s_scr[r][c] = ' ';
    s_dirty[r] = true;
}

static void newline(void)
{
    s_col = 0;
    if (s_row + 1 < s_rows) { s_row++; return; }
    for (int r = 0; r + 1 < s_rows; r++) {
        for (int c = 0; c < COLS; c++) s_scr[r][c] = s_scr[r + 1][c];
        s_dirty[r] = true;
    }
    clear_row(s_rows - 1);
}

static void parse_csi_params(const char *s, int *p0, int *p1, int def0, int def1)
{
    while (*s == '?' || *s == '>' || *s == '=') s++;
    int v0 = 0, v1 = 0;
    bool any0 = false, any1 = false;
    while (*s >= '0' && *s <= '9') {
        v0 = v0 * 10 + (*s - '0');
        any0 = true;
        s++;
    }
    if (*s == ';') {
        s++;
        while (*s >= '0' && *s <= '9') {
            v1 = v1 * 10 + (*s - '0');
            any1 = true;
            s++;
        }
    }
    *p0 = any0 ? v0 : def0;
    *p1 = any1 ? v1 : def1;
}

static void put_codepoint(uint32_t cp)
{
    if (s_col >= COLS) {
        s_col = 0;
        newline();
    }
    s_scr[s_row][s_col++] = (uint16_t)(cp <= 0xFFFFu ? cp : 0xFFFDu);
    s_dirty[s_row] = true;
}

static void feed(char ch)
{
    unsigned char u = (unsigned char)ch;
    if (s_esc_state == 1) {
        if (ch == '[') { s_esc_state = 2; s_csi_len = 0; return; }
        if (ch == ']') { s_esc_state = 3; return; }
        if (ch == '(' || ch == ')' || ch == '*' || ch == '+') { s_esc_state = 4; return; }
        if (ch == 'M') { if (s_row > 0) s_row--; s_esc_state = 0; return; }
        if (ch == 'D' || ch == 'E') { if (ch == 'E') s_col = 0; newline(); s_esc_state = 0; return; }
        if (ch == 'c') {
            for (int r = 0; r < s_rows; r++) clear_row(r);
            s_row = 0; s_col = 0; s_esc_state = 0;
            return;
        }
        s_esc_state = 0;
        return;
    }
    if (s_esc_state == 2) {
        if (u >= 0x40 && u <= 0x7E) {
            s_csi_buf[s_csi_len] = 0;
            int p0 = 0, p1 = 0;
            parse_csi_params(s_csi_buf, &p0, &p1, 0, 0);
            int n = (p0 > 0) ? p0 : 1;
            if (ch == 'J') {
                if (p0 == 2 || p0 == 3) {
                    for (int r = 0; r < s_rows; r++) clear_row(r);
                    s_row = 0; s_col = 0;
                } else if (p0 == 0) {
                    for (int c = s_col; c < COLS; c++) s_scr[s_row][c] = ' ';
                    s_dirty[s_row] = true;
                    for (int r = s_row + 1; r < s_rows; r++) clear_row(r);
                } else if (p0 == 1) {
                    for (int r = 0; r < s_row; r++) clear_row(r);
                    for (int c = 0; c <= s_col && c < COLS; c++) s_scr[s_row][c] = ' ';
                    s_dirty[s_row] = true;
                }
            } else if (ch == 'H' || ch == 'f') {
                int r = (p0 > 0) ? (p0 - 1) : 0;
                int c = (p1 > 0) ? (p1 - 1) : 0;
                if (r < 0) r = 0;
                if (r >= s_rows) r = s_rows - 1;
                if (c < 0) c = 0;
                if (c >= COLS) c = COLS - 1;
                s_row = r; s_col = c;
            } else if (ch == 'K') {
                int start_c = (p0 == 2 || p0 == 1) ? 0 : s_col;
                int end_c   = (p0 == 1) ? (s_col + 1 < COLS ? s_col + 1 : COLS) : COLS;
                for (int c = start_c; c < end_c; c++) s_scr[s_row][c] = ' ';
                s_dirty[s_row] = true;
            } else if (ch == 'A') {
                s_row -= n; if (s_row < 0) s_row = 0;
            } else if (ch == 'B') {
                s_row += n; if (s_row >= s_rows) s_row = s_rows - 1;
            } else if (ch == 'C') {
                s_col += n; if (s_col >= COLS) s_col = COLS - 1;
            } else if (ch == 'D') {
                s_col -= n; if (s_col < 0) s_col = 0;
            } else if (ch == 'G') {
                s_col = n - 1;
                if (s_col < 0) s_col = 0;
                if (s_col >= COLS) s_col = COLS - 1;
            } else if (ch == 'd') {
                s_row = n - 1;
                if (s_row < 0) s_row = 0;
                if (s_row >= s_rows) s_row = s_rows - 1;
            } else if (ch == 'P') {
                if (n > COLS - s_col) n = COLS - s_col;
                for (int c = s_col; c + n < COLS; c++) s_scr[s_row][c] = s_scr[s_row][c + n];
                for (int c = COLS - n; c < COLS; c++) s_scr[s_row][c] = ' ';
                s_dirty[s_row] = true;
            } else if (ch == '@') {
                if (n > COLS - s_col) n = COLS - s_col;
                for (int c = COLS - 1; c >= s_col + n; c--) s_scr[s_row][c] = s_scr[s_row][c - n];
                for (int c = s_col; c < s_col + n && c < COLS; c++) s_scr[s_row][c] = ' ';
                s_dirty[s_row] = true;
            } else if (ch == 'X') {
                for (int c = s_col; c < s_col + n && c < COLS; c++) s_scr[s_row][c] = ' ';
                s_dirty[s_row] = true;
            }
            s_esc_state = 0;
            return;
        }
        if (s_csi_len + 1 < sizeof(s_csi_buf)) s_csi_buf[s_csi_len++] = ch;
        return;
    }
    if (s_esc_state == 3) {
        if (u == 0x07) s_esc_state = 0;
        else if (u == 0x1B) s_esc_state = 4;
        return;
    }
    if (s_esc_state == 4) {
        s_esc_state = 0;
        return;
    }

    if (u == 0x1B) { s_utf8_rem = 0; s_esc_state = 1; return; }
    if (u >= 0x80u) {
        if ((u & 0xC0u) == 0x80u) {
            if (s_utf8_rem > 0) {
                s_utf8_cp = (s_utf8_cp << 6) | (uint32_t)(u & 0x3Fu);
                s_utf8_rem--;
                if (s_utf8_rem == 0) {
                    put_codepoint(s_utf8_cp);
                }
            }
        } else if ((u & 0xE0u) == 0xC0u) {
            s_utf8_cp  = (uint32_t)(u & 0x1Fu);
            s_utf8_rem = 1;
        } else if ((u & 0xF0u) == 0xE0u) {
            s_utf8_cp  = (uint32_t)(u & 0x0Fu);
            s_utf8_rem = 2;
        } else if ((u & 0xF8u) == 0xF0u) {
            s_utf8_cp  = (uint32_t)(u & 0x07u);
            s_utf8_rem = 3;
        } else {
            s_utf8_rem = 0;
        }
        return;
    }

    s_utf8_rem = 0;
    if (ch == '\r') { s_col = 0; return; }
    if (ch == '\n') { newline(); return; }
    if (ch == '\b') { if (s_col > 0) s_col--; return; }
    if (ch == '\t') {
        int next_tab = (s_col + 8) & ~7;
        if (next_tab >= COLS) next_tab = COLS - 1;
        s_col = next_tab;
        return;
    }
    if (u < 0x20 || u == 0x7F) return;
    put_codepoint((uint32_t)u);
}


void fbcon_overlay_colors(int style, uint16_t *fg, uint16_t *bg)
{
    switch (style) {
    case FBCON_OVL_SEGMENT:  *fg = RGB565(255, 255, 255); *bg = RGB565(40, 90, 170);  break;
    case FBCON_OVL_FOCUS:    *fg = RGB565(0, 0, 0);       *bg = RGB565(255, 215, 70); break;
    case FBCON_OVL_CAND:     *fg = RGB565(0, 0, 0);       *bg = RGB565(225, 228, 235); break;
    case FBCON_OVL_CAND_SEL: *fg = RGB565(255, 255, 255); *bg = RGB565(0, 100, 200);  break;
    case FBCON_OVL_BADGE:    *fg = RGB565(0, 0, 0);       *bg = RGB565(80, 220, 120); break;
    case FBCON_OVL_BAR:      *fg = RGB565(255, 255, 255); *bg = RGB565(45, 45, 60);   break;
    case FBCON_OVL_PENDING:  *fg = RGB565(255, 215, 70);  *bg = RGB565(20, 40, 110);  break;
    default:                 *fg = RGB565(255, 255, 255); *bg = RGB565(20, 40, 110);  break;
    }
}

#if M5OS_BOARD_TAB5        /* only the Tab5 has the Japanese input method; the other boards keep their RAM */
/* ---- overlay (see fbcon.h) --------------------------------------------------------------------- */

#define OVL_SPANS 24
#define OVL_CELLS 80

typedef struct {
    int16_t row, col;               /* anchor-relative (cursor) or absolute (screen) */
    uint8_t anchor, len, style;
    uint16_t cp[OVL_CELLS];
} ovl_span_t;

typedef struct { int16_t row, col; uint8_t len, style; uint16_t cp[OVL_CELLS]; } ovl_abs_t;

/* The spans take ~16 KB, so they live in PSRAM and are allocated when the first overlay is built. */
static struct ovl_mem {
    ovl_span_t stage[OVL_SPANS], live[OVL_SPANS];
    ovl_abs_t  paint[OVL_SPANS];                /* what is on the screen now */
    ovl_abs_t  now[OVL_SPANS];                  /* scratch for ovl_prepare */
} *s_ovl;
#define s_ovl_stage (s_ovl->stage)
#define s_ovl_live  (s_ovl->live)
#define s_ovl_paint (s_ovl->paint)
static int s_ovl_nstage, s_ovl_nlive, s_ovl_npaint;
static volatile uint32_t s_ovl_gen;
static uint32_t s_ovl_seen;

void fbcon_overlay_begin(void)
{
    if (!s_ovl) s_ovl = kmalloc_psram(sizeof *s_ovl);
    s_ovl_nstage = 0;
}

void fbcon_overlay_text(int anchor, int row, int col, const uint16_t *cps, int n, int style)
{
    if (!s_ovl || s_ovl_nstage >= OVL_SPANS || n <= 0) return;
    if (n > OVL_CELLS) n = OVL_CELLS;
    ovl_span_t *s = &s_ovl_stage[s_ovl_nstage++];
    s->anchor = (uint8_t)anchor; s->row = (int16_t)row; s->col = (int16_t)col; s->len = (uint8_t)n; s->style = (uint8_t)style;
    for (int i = 0; i < n; i++) s->cp[i] = cps[i];
}

void fbcon_overlay_end(void)
{
    if (!s_ovl) return;
    uint32_t irq = arch_irq_disable();
    s_ovl_nlive = s_ovl_nstage;
    for (int i = 0; i < s_ovl_nstage; i++) s_ovl_live[i] = s_ovl_stage[i];
    s_ovl_gen++;
    arch_irq_restore(irq);
}

void fbcon_get_geometry(int *rows, int *cols, int *cur_row, int *cur_col)
{
    if (rows) *rows = s_rows;
    if (cols) *cols = COLS;
    if (cur_row) *cur_row = s_row;
    if (cur_col) *cur_col = s_col < COLS ? s_col : COLS - 1;
}

static bool ovl_covers(int r, int c)
{
    if (!s_ovl) return false;
    for (int i = 0; i < s_ovl_npaint; i++)
        if (s_ovl_paint[i].row == r && c >= s_ovl_paint[i].col && c < s_ovl_paint[i].col + s_ovl_paint[i].len) return true;
    return false;
}

/* Works out where the live spans go on the screen now; marks the rows of the old ones for repainting when
 * anything moved.  Returns true if the overlay has to be painted again. */
static bool ovl_prepare(void)
{
    if (!s_ovl) return false;
    ovl_abs_t *now = s_ovl->now;
    int n = 0;
    bool changed = s_ovl_gen != s_ovl_seen;
    uint32_t irq = arch_irq_disable();
    s_ovl_seen = s_ovl_gen;
    for (int i = 0; i < s_ovl_nlive; i++) {
        const ovl_span_t *s = &s_ovl_live[i];
        int r = s->row, c = s->col;
        if (s->anchor == FBCON_ANCHOR_CURSOR) { r += s_row; c += (s_col < COLS ? s_col : COLS - 1); }
        else { if (r < 0) r += s_rows; if (c < 0) c += COLS; }
        int len = s->len;
        const uint16_t *cp = s->cp;
        if (c < 0) { cp -= c; len += c; c = 0; }
        if (c + len > COLS) len = COLS - c;
        if (r < 0 || r >= s_rows || len <= 0) continue;
        now[n].row = (int16_t)r; now[n].col = (int16_t)c; now[n].len = (uint8_t)len; now[n].style = s->style;
        for (int k = 0; k < len; k++) now[n].cp[k] = cp[k];
        n++;
    }
    arch_irq_restore(irq);
    if (n != s_ovl_npaint) changed = true;
    for (int i = 0; !changed && i < n; i++)
        if (now[i].row != s_ovl_paint[i].row || now[i].col != s_ovl_paint[i].col || now[i].len != s_ovl_paint[i].len) changed = true;
    if (!changed) return false;
    for (int i = 0; i < s_ovl_npaint; i++) s_dirty[s_ovl_paint[i].row] = true;          /* put the console text back */
    s_cursor_on = false;
    for (int i = 0; i < n; i++) s_ovl_paint[i] = now[i];
    s_ovl_npaint = n;
    return true;
}

static void ovl_paint(void)
{
    if (!s_ovl) return;
    for (int i = 0; i < s_ovl_npaint; i++) {
        uint16_t fg, bg;
        fbcon_overlay_colors(s_ovl_paint[i].style, &fg, &bg);
        display_draw_codepoints(s_ovl_paint[i].col * CELL, s_ovl_paint[i].row * CELL, s_ovl_paint[i].cp, s_ovl_paint[i].len, fg, bg, SCALE);
    }
}

#else
void fbcon_overlay_begin(void) {}
void fbcon_overlay_text(int anchor, int row, int col, const uint16_t *cps, int n, int style) { (void)anchor; (void)row; (void)col; (void)cps; (void)n; (void)style; }
void fbcon_overlay_end(void) {}
void fbcon_get_geometry(int *rows, int *cols, int *cur_row, int *cur_col)
{
    if (rows) *rows = s_rows;
    if (cols) *cols = COLS;
    if (cur_row) *cur_row = s_row;
    if (cur_col) *cur_col = s_col < COLS ? s_col : COLS - 1;
}
static inline bool ovl_covers(int r, int c) { (void)r; (void)c; return false; }
static inline bool ovl_prepare(void) { return false; }
static inline void ovl_paint(void) {}
#endif

static void draw_cursor(bool on)
{
    int r = s_row, c = s_col < COLS ? s_col : COLS - 1;
    if (ovl_covers(r, c)) { s_cursor_row = r; s_cursor_col = c; s_cursor_on = false; return; }      /* the overlay is there instead */
    uint16_t under = s_scr[r][c];
    display_draw_codepoints(c * CELL, r * CELL, &under, 1, on ? COL_BG : COL_FG, on ? COL_FG : COL_BG, SCALE);
    s_cursor_row = r; s_cursor_col = c; s_cursor_on = on;
}

static void flush_rows(void)
{
    for (int r = 0; r < s_rows; r++) {
        if (!s_dirty[r]) continue;
        display_draw_codepoints(0, r * CELL, s_scr[r], COLS, COL_FG, COL_BG, SCALE);
        s_dirty[r] = false;
        if (r == s_cursor_row) s_cursor_on = false;         /* the row was repainted: the cursor cell is gone */
    }
}

static void fbcon_task(void *arg)
{
    (void)arg;
    for (int r = 0; r < s_rows; r++) clear_row(r);
    uint32_t end = kconsole_log_end();
    uint32_t pos = end > HISTORY ? end - HISTORY : 0;
    uint32_t last_blink = (uint32_t)(hal_time_now_us() / 1000u);
    bool blink = true;

    for (;;) {
        char buf[64];
        size_t n;
        bool moved = false;
        while ((n = kconsole_log_read(&pos, buf, sizeof buf)) > 0) { for (size_t i = 0; i < n; i++) feed(buf[i]); moved = true; }
        if (s_paused || display_is_suspended()) { task_sleep(POLL_MS); continue; }     /* suspended: keep the changes, paint them on wake */
        ovl_prepare();
        flush_rows();

        uint32_t now = (uint32_t)(hal_time_now_us() / 1000u);
        if (moved) { blink = true; last_blink = now; }
        else if (now - last_blink >= BLINK_MS) { blink = !blink; last_blink = now; moved = true; }

        int cur_c = s_col < COLS ? s_col : COLS - 1;
        if (moved || s_cursor_row != s_row || s_cursor_col != cur_c) {
            if (s_cursor_row >= 0 && (s_cursor_row != s_row || s_cursor_col != cur_c) && s_cursor_on && !ovl_covers(s_cursor_row, s_cursor_col)) {
                uint16_t under = s_scr[s_cursor_row][s_cursor_col];
                display_draw_codepoints(s_cursor_col * CELL, s_cursor_row * CELL, &under, 1, COL_FG, COL_BG, SCALE);
            }
            draw_cursor(blink);
        }
        ovl_paint();                                            /* on top of everything else, every pass: it is small */
        task_sleep(POLL_MS);
    }
}

bool fbcon_is_paused(void) { return s_paused; }

int fbcon_cell_px(void) { return CELL; }
int fbcon_max_rows(void) { return ROWS; }

void fbcon_set_rows(int rows)
{
    if (rows < 4) rows = 4;
    if (rows > ROWS) rows = ROWS;
    uint32_t irq = arch_irq_disable();
    if (rows == s_rows) { arch_irq_restore(irq); return; }
    if (s_row >= rows) {                                        /* keep the cursor line visible: scroll up */
        int d = s_row - (rows - 1);
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < COLS; c++) s_scr[r][c] = s_scr[r + d][c];
        s_row = rows - 1;
    }
    for (int r = rows; r < ROWS; r++) clear_row(r);
    for (int r = 0; r < rows; r++) s_dirty[r] = true;
    s_cursor_row = -1;
    s_rows = rows;
    arch_irq_restore(irq);
}

void fbcon_pause(bool pause)
{
    if (!pause) {
        display_clear(COL_BG);
        for (int r = 0; r < s_rows; r++) s_dirty[r] = true;      /* repaint everything on the next pass */
        s_cursor_row = -1;
    }
    s_paused = pause;
}

void fbcon_start(void)
{
    task_create_stack("fbcon", fbcon_task, NULL, TASK_PRIO_DEFAULT + 1, 4096);
}
