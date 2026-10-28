#include "dqwin.h"
#include <m5os/display.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/ime.h>

#define GLYPH     8
#define SCALE     M5OS_FBCON_SCALE
#define CELL      (GLYPH * SCALE)
#define FRAME_PX  (2 * SCALE)           /* thickness of the white frame */
#define GAP_PX    (SCALE)               /* black gap between frame and text area */
#define ESC_WAIT_MS 40u                 /* how long a lone ESC waits for the rest of an arrow-key sequence */
#define IDLE_MS     10u

#define FG  COLOR_WHITE
#define BG  COLOR_BLACK

int dq_cols(void) { return DISPLAY_WIDTH / CELL; }
int dq_rows(void) { return DISPLAY_HEIGHT / CELL; }

void dq_clear(void) { display_clear(BG); }

void dq_window(int cx, int cy, int cw, int ch)
{
    int x = cx * CELL, y = cy * CELL, w = cw * CELL, h = ch * CELL;
    display_fill_rect(x, y, w, h, BG);
    display_fill_rect(x, y, w, FRAME_PX, FG);                        /* top */
    display_fill_rect(x, y + h - FRAME_PX, w, FRAME_PX, FG);         /* bottom */
    display_fill_rect(x, y, FRAME_PX, h, FG);                        /* left */
    display_fill_rect(x + w - FRAME_PX, y, FRAME_PX, h, FG);         /* right */
    /* round the corners: take the outermost pixel of each corner away */
    int d = SCALE;
    display_fill_rect(x, y, d, d, BG);
    display_fill_rect(x + w - d, y, d, d, BG);
    display_fill_rect(x, y + h - d, d, d, BG);
    display_fill_rect(x + w - d, y + h - d, d, d, BG);
    (void)GAP_PX;
}

static void put_text(int cx, int cy, const char *s, uint16_t fg, uint16_t bg)
{
    if (cy < 0 || cy >= dq_rows() || cx < 0) return;
    char line[160];
    int n = 0, cells = 0;                                            /* UTF-8: one cell per character, whatever its bytes */
    while (s[n] && cx + cells < dq_cols() && n < (int)sizeof line - 4) {
        int k = 1;
        unsigned char c = (unsigned char)s[n];
        if (c >= 0xF0) k = 4; else if (c >= 0xE0) k = 3; else if (c >= 0xC0) k = 2;
        for (int i = 0; i < k; i++) { if (!s[n]) break; line[n] = s[n]; n++; }
        cells++;
    }
    line[n] = 0;
    if (n) display_draw_text(cx * CELL, cy * CELL, line, fg, bg, SCALE);
}

void dq_text(int cx, int cy, const char *s)     { put_text(cx, cy, s, FG, BG); }
void dq_text_rev(int cx, int cy, const char *s) { put_text(cx, cy, s, BG, FG); }

void dq_cursor(int cx, int cy, bool on)
{
    /* a right-pointing triangle: columns 0..3 of the glyph, 7, 5, 3, 1 pixels tall, centred */
    int x = cx * CELL, y = cy * CELL;
    display_fill_rect(x, y, CELL, CELL, BG);
    if (!on) return;
    for (int c = 0; c < 4; c++) {
        int hgt = 7 - 2 * c;
        display_fill_rect(x + (1 + c) * SCALE, y + ((GLYPH - hgt) / 2) * SCALE, SCALE, hgt * SCALE, FG);
    }
}

/* ---- keys ---------------------------------------------------------------------- */

static int raw_getc(uint32_t wait_ms)
{
    for (uint32_t t = 0; ; t += IDLE_MS) {
        int c = kconsole_getc();
        if (c >= 0) return c;
        if (wait_ms && t >= wait_ms) return -1;
        task_sleep(IDLE_MS);
    }
}

static int decode_key(int c)
{
    if (c < 0) return DQK_NONE;
    if (c == '\n') return DQK_ENTER;
    if (c == 0x08) return DQK_BACKSPACE;
    if (c != 0x1B) return c;
    int c2 = raw_getc(ESC_WAIT_MS);
    if (c2 != '[' && c2 != 'O') return DQK_ESC;                /* a lone ESC */
    int c3 = raw_getc(ESC_WAIT_MS);
    switch (c3) {
    case 'A': return DQK_UP;
    case 'B': return DQK_DOWN;
    case 'C': return DQK_RIGHT;
    case 'D': return DQK_LEFT;
    case 'H': return DQK_HOME;
    case 'F': return DQK_END;
    case 'P': return DQK_F1;
    case 'Q': return DQK_F2;
    case 'R': return DQK_F3;
    case 'S': return DQK_F4;
    case '1': case '3': case '4': case '5': case '6': case '7': case '8': {
        int c4 = raw_getc(ESC_WAIT_MS);                       /* ESC [ n ~ */
        if (c4 != '~') return DQK_NONE;
        return c3 == '5' ? DQK_PGUP : c3 == '6' ? DQK_PGDN : c3 == '3' ? DQK_DEL : (c3 == '1' || c3 == '7') ? DQK_HOME : DQK_END;
    }
    default: return DQK_NONE;
    }
}

int dq_getkey(void)
{
    return decode_key(raw_getc(0));
}

int dq_pollkey(void)
{
    return decode_key(kconsole_getc());
}

/* ---- dialogs -------------------------------------------------------------------- */

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

bool dq_confirm(const char *question)
{
    int w = slen(question) + 4;
    if (w < 16) w = 16;
    int h = 6, x = (dq_cols() - w) / 2, y = (dq_rows() - h) / 2;
    dq_window(x, y, w, h);
    dq_text(x + 2, y + 1, question);
    bool yes = false;
    for (;;) {
        dq_cursor(x + 2, y + 3, yes);  dq_text(x + 4, y + 3, "Yes");
        dq_cursor(x + 2, y + 4, !yes); dq_text(x + 4, y + 4, "No");
        int k = dq_getkey();
        if (k == DQK_UP || k == DQK_DOWN || k == 'k' || k == 'j') yes = !yes;
        else if (k == 'y' || k == 'Y') return true;
        else if (k == 'n' || k == 'N' || k == DQK_ESC) return false;
        else if (k == DQK_ENTER) return yes;
    }
}

static int u8_cells(const char *s, int bytes)                       /* characters in the first `bytes` bytes */
{
    int n = 0;
    for (int i = 0; i < bytes; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
    return n;
}

bool dq_input(const char *title, char *buf, int max)
{
    int w = dq_cols() * 3 / 5, h = 5, x = (dq_cols() - w) / 2, y = (dq_rows() - h) / 2;
    dq_window(x, y, w, h);
    dq_text(x + 2, y + 1, title);
    int len = slen(buf), field = w - 4;
    bool ok = false;
    for (;;) {
        char line[200];
        int n = 0, start = 0;
        while (u8_cells(buf + start, len - start) > field - 1) {          /* show the tail if the text is long */
            start++;
            while (start < len && ((unsigned char)buf[start] & 0xC0) == 0x80) start++;
        }
        for (int i = start; i < len && n < (int)sizeof line - 2; i++) line[n++] = buf[i];
        int shown = u8_cells(buf + start, len - start);
        line[n++] = '_';
        for (int c = shown + 1; c < field && n < (int)sizeof line - 1; c++) line[n++] = ' ';
        line[n] = 0;
        dq_text(x + 2, y + 3, line);
        ime_set_anchor(x + 2 + shown, y + 3);                             /* the Japanese input method composes here, inline */
        int k = dq_getkey();
        if (k == DQK_ENTER) { ok = true; break; }
        if (k == DQK_ESC) break;
        if (k == DQK_BACKSPACE || k == DQK_DEL) {
            if (len) { do len--; while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80); buf[len] = 0; }
        }
        else if (k == 0x15) { len = 0; buf[0] = 0; }                     /* Ctrl-U clears field */
        else if (k >= 0x20 && k != 0x7F && !(k >= 0x80 && k < 0xC0) && k < 0x100) {
            char ch[4];
            int cn = k >= 0xF0 ? 4 : k >= 0xE0 ? 3 : k >= 0xC0 ? 2 : 1;
            ch[0] = (char)k;
            for (int i = 1; i < cn; i++) { int c = dq_getkey(); ch[i] = (char)(c < 0 ? 0 : c); }
            if (len + cn < max) { for (int i = 0; i < cn; i++) buf[len++] = ch[i]; buf[len] = 0; }
        }
    }
    ime_clear_anchor();
    return ok;
}

void dq_message(const char *l1, const char *l2)
{
    int a = slen(l1), b = l2 ? slen(l2) : 0, w = (a > b ? a : b) + 4;
    if (w < 20) w = 20;
    int h = l2 ? 6 : 5, x = (dq_cols() - w) / 2, y = (dq_rows() - h) / 2;
    dq_window(x, y, w, h);
    dq_text(x + 2, y + 1, l1);
    if (l2) dq_text(x + 2, y + 2, l2);
    dq_text(x + w - 12, y + h - 2, "[any key]");
    dq_getkey();
}
