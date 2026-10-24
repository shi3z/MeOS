/*
 * Japanese input method: keys in, UTF-8 text out.  See kernel/include/m5os/ime.h for the user's view.
 *
 * Structure, bottom to top:
 *   romaji.c    typed letters -> kana
 *   ime_dict.c  reading -> candidate spellings (SKK dictionaries)
 *   conv.c      a whole reading -> phrases, each with its candidates
 *   ime.c       this file: the keys, the three states (idle / reading / converting) and the screen
 *
 * The screen part has two forms.  A program that prints through the console (the shell, the editor) gets an
 * overlay at the text cursor, drawn by fbcon on top of the console without changing its contents.  A program
 * that has paused fbcon to draw its own screen (filer, ssh, web) has no cursor we could know, so the same UI
 * is drawn as a bar over the bottom two text rows, and those pixels are put back when it is done.
 */
#include <m5os/ime.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/display.h>
#include <m5os/kmalloc.h>
#include <m5os/board_config.h>
#include <m5os/arch.h>
#include "../lcd/fbcon.h"
#include "ime_util.h"
#include "ime_dict.h"
#include "romaji.h"
#include "conv.h"
#include <string.h>

#define PRE_MAX     60              /* kana in the reading */
#define CELLS       200             /* cells of preedit text on the screen */
#define PAGE        9               /* candidates per page */
#define CAND_W_MAX  20
#define SCALE       M5OS_FBCON_SCALE
#define CELL        (8 * SCALE)
#define SCR_COLS    (DISPLAY_WIDTH / CELL)
#define SCR_ROWS    (DISPLAY_HEIGHT / CELL)

typedef enum { IME_OFF, IME_HIRA, IME_KATA } ime_mode_t;

static struct {
    bool       inited;
    ime_mode_t mode, last_on;
    uint16_t   pre[PRE_MAX];
    int        plen, pcur;                 /* the reading and the insertion point in it */
    romaji_t   rom;                        /* romaji typed after pcur that is not kana yet */
    char       raw[PRE_MAX];               /* the keys typed for the reading, for F9 / F10 */
    int        rawn;
    bool       raw_ok;
    bool       converting, listing;
    int        focus;
    bool       shown;                      /* something of ours is on the screen */
    bool       bar_saved;                  /* a copy of the screen rows we draw over is in bar_buf */
    uint16_t  *bar_buf;
    int        snap_y, snap_rows, drawn_rows;   /* the saved rows (pixels / cells) and how many we have painted on */
    bool       bypass;                     /* keys go through untouched (see ime_bypass) */
    bool       anchor_set;                 /* the program told us where its insertion point is */
    bool       app_view;
    uint32_t   view_revision;
    int        ax, ay;
} g;
#define SNAP_ROWS (PAGE + 3)

/* Everything big lives in PSRAM (allocated once in ime_init): the converter's tables alone are tens of KB. */
typedef struct { uint16_t cp[CELLS]; uint8_t st[CELLS]; int n; int focus_at; } cells_t;
#define CAND_LINES (PAGE + 1)

static struct ime_big {
    conv_t       cv;
    conv_cands_t cl;                       /* candidates of the focused phrase */
    conv_cands_t tmp;
    char         text[CONV_MAX_SEG * CONV_MAX_TEXT * 3 + 4];
    cells_t      pre, lines[CAND_LINES], bpre, bline, bcand;
} *B;
#define s_cv (B->cv)

extern const uint8_t ime_mini_dict[];
extern const uint32_t ime_mini_dict_size;
void ime_dict_load_async(void);            /* ime_flash.c */

/* ---- output --------------------------------------------------------------------------------- */

static void out_raw(const char *s, size_t n)
{
    size_t done = 0;
    int tries = 0;
    while (done < n) {
        size_t k = kconsole_inject_n(s + done, n - done);
        done += k;
        if (k) tries = 0;
        else if (++tries > 40) break;                       /* nobody is reading the console: give up rather than hang */
        else task_sleep(5);
    }
}

static void out_str(const char *s) { out_raw(s, strlen(s)); }

static void out_cps(const uint16_t *c, int n)
{
    char buf[PRE_MAX * 3 + 4];
    int len = ime_cps_to_u8(c, n, buf, (int)sizeof buf);
    out_raw(buf, (size_t)len);
}

/* ---- keys ----------------------------------------------------------------------------------- */

typedef enum {
    K_CHAR, K_ENTER, K_BS, K_DEL, K_ESC, K_TAB, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SLEFT, K_SRIGHT, K_HOME, K_END,
    K_F6, K_F7, K_F8, K_F9, K_F10, K_TOGGLE, K_CTRL, K_OTHER
} key_t;

static key_t parse_key(const char *k, char *ch)
{
    *ch = k[0];
    if (k[0] == 0x1B) {
        if (k[1] == 0) return K_ESC;
        static const struct { const char *s; key_t k; } T[] = {
            {"[A", K_UP}, {"[B", K_DOWN}, {"[C", K_RIGHT}, {"[D", K_LEFT}, {"[H", K_HOME}, {"[F", K_END}, {"[3~", K_DEL},
            {"[1;2C", K_SRIGHT}, {"[1;2D", K_SLEFT}, {"[17~", K_F6}, {"[18~", K_F7}, {"[19~", K_F8}, {"[20~", K_F9},
            {"[21~", K_F10}, {"[99~", K_TOGGLE},
        };
        for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (strcmp(k + 1, T[i].s) == 0) return T[i].k;
        return K_OTHER;
    }
    if (k[1] != 0) return K_OTHER;
    uint8_t c = (uint8_t)k[0];
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 0x7F || c == 0x08) return K_BS;
    if (c == '\t') return K_TAB;
    if (c >= 0x20 && c < 0x7F) return K_CHAR;
    if (c < 0x20) return K_CTRL;
    return K_OTHER;
}

/* Keys the IME does not use go on to the application; Shift+arrow means nothing to it. */
static void pass(const char *key, key_t kt)
{
    if (kt == K_SLEFT) key = "\x1b[D";
    else if (kt == K_SRIGHT) key = "\x1b[C";
    out_str(key);
}

/* ---- the reading ---------------------------------------------------------------------------- */

static bool composing(void) { return g.plen > 0 || g.rom.n > 0; }

static void pre_reset(void)
{
    g.plen = g.pcur = 0;
    memset(&g.rom, 0, sizeof g.rom);
    g.rawn = 0;
    g.raw_ok = true;
    g.converting = g.listing = false;
    g.focus = 0;
}

static void pre_insert(uint16_t cp)
{
    if (g.plen >= PRE_MAX) return;
    for (int i = g.plen; i > g.pcur; i--) g.pre[i] = g.pre[i - 1];
    g.pre[g.pcur++] = cp;
    g.plen++;
}

static void raw_add(char c)
{
    if (g.rawn < PRE_MAX - 1) g.raw[g.rawn++] = c; else g.raw_ok = false;
}

static void insert_kana(const uint16_t *k, int n)
{
    for (int i = 0; i < n; i++) pre_insert(g.mode == IME_KATA ? ime_to_kata(k[i]) : k[i]);
}

static void rom_feed(char c)
{
    uint16_t out[8];
    raw_add(c);
    insert_kana(out, romaji_feed(&g.rom, c, out));
}

static void rom_flush(void)
{
    uint16_t out[8];
    insert_kana(out, romaji_flush(&g.rom, out));
}

static void pre_commit(void)
{
    rom_flush();
    out_cps(g.pre, g.plen);
    pre_reset();
}

static void pre_backspace(void)
{
    if (g.rom.n > 0) {
        g.rom.n--;
        if (g.rawn > 0) g.rawn--;
        g.rom.tent = 0;
        return;
    }
    if (g.pcur > 0) {
        for (int i = g.pcur - 1; i + 1 < g.plen; i++) g.pre[i] = g.pre[i + 1];
        g.pcur--; g.plen--;
        g.raw_ok = false;
    }
}

/* F6 F7 F9 F10: respell the reading */
static void pre_respell(key_t kt)
{
    rom_flush();
    if (kt == K_F6 || kt == K_F7) {
        for (int i = 0; i < g.plen; i++) g.pre[i] = kt == K_F7 ? ime_to_kata(ime_to_hira(g.pre[i])) : ime_to_hira(g.pre[i]);
    } else if (g.raw_ok && g.rawn > 0) {
        g.plen = g.pcur = 0;
        for (int i = 0; i < g.rawn; i++)
            pre_insert(kt == K_F9 ? (g.raw[i] == ' ' ? 0x3000 : (uint16_t)(g.raw[i] + 0xFEE0)) : (uint16_t)(uint8_t)g.raw[i]);
    }
    g.pcur = g.plen;
}

/* ---- converting ----------------------------------------------------------------------------- */

static void load_cands(void) { conv_cands(&s_cv, g.focus, &B->cl); }

static void conv_begin(void)
{
    rom_flush();
    if (g.plen == 0) return;
    uint16_t hira[PRE_MAX];
    for (int i = 0; i < g.plen; i++) hira[i] = ime_to_hira(g.pre[i]);
    conv_start(&s_cv, hira, g.plen);
    if (g.mode == IME_KATA) {                                        /* katakana mode: the katakana spelling is the default */
        for (int i = 0; i < s_cv.nseg; i++) {
            conv_cands(&s_cv, i, &B->tmp);
            conv_select(&s_cv, i, &B->tmp, B->tmp.n - 1);
        }
    }
    g.converting = true;
    g.listing = false;
    g.focus = 0;
    load_cands();
}

static void conv_commit(void)
{
    conv_learn(&s_cv);
    int n = conv_text(&s_cv, B->text, (int)sizeof B->text);
    out_raw(B->text, (size_t)n);
    pre_reset();
}

static void conv_pick(int idx)
{
    if (B->cl.n == 0) return;
    idx = (idx % B->cl.n + B->cl.n) % B->cl.n;
    conv_select(&s_cv, g.focus, &B->cl, idx);
}

static void conv_key(key_t kt, char ch, const char *key)
{
    int sel = s_cv.seg[g.focus].sel;
    switch (kt) {
    case K_CHAR:
        if (ch == ' ') { g.listing = true; conv_pick(sel + 1); return; }
        if (g.listing && ch >= '1' && ch <= '9') {
            int idx = sel / PAGE * PAGE + (ch - '1');
            if (idx < B->cl.n) conv_pick(idx);
            return;
        }
        conv_commit();                                               /* anything else starts the next sentence */
        ime_feed(key);
        return;
    case K_DOWN:  g.listing = true; conv_pick(sel + 1); return;
    case K_UP:    g.listing = true; conv_pick(sel - 1); return;
    case K_LEFT:  if (g.focus > 0) { g.focus--; g.listing = false; load_cands(); } return;
    case K_RIGHT: if (g.focus + 1 < s_cv.nseg) { g.focus++; g.listing = false; load_cands(); } return;
    case K_SLEFT:
    case K_SRIGHT:
        if (conv_resize(&s_cv, g.focus, kt == K_SRIGHT ? 1 : -1)) {
            if (g.focus >= s_cv.nseg) g.focus = s_cv.nseg - 1;
            g.listing = false;
            load_cands();
        }
        return;
    case K_ENTER: conv_commit(); return;
    case K_ESC:
    case K_BS:    g.converting = g.listing = false; g.pcur = g.plen; return;       /* back to the reading */
    case K_F6: case K_F7: case K_F9: case K_F10:
        g.converting = g.listing = false;
        g.pcur = g.plen;
        pre_respell(kt);
        return;
    case K_CTRL:
        if (ch == 0x03 || ch == 0x07) { g.converting = g.listing = false; g.pcur = g.plen; return; }
        /* fall through */
    default:
        conv_commit();
        pass(key, kt);
        return;
    }
}

/* ---- the three states ------------------------------------------------------------------------ */

static void pre_key(key_t kt, char ch, const char *key)
{
    switch (kt) {
    case K_CHAR:
        if (ch == ' ') { conv_begin(); return; }
        if (ch >= 'a' && ch <= 'z') { rom_feed(ch); return; }
        rom_flush();
        if (romaji_punct(ch)) pre_insert(romaji_punct(ch));
        else pre_insert((uint16_t)(uint8_t)ch);                       /* capitals, digits and the like stay as typed */
        raw_add(ch);
        return;
    case K_ENTER: pre_commit(); return;
    case K_BS:    pre_backspace(); return;
    case K_DEL:
        rom_flush();
        if (g.pcur < g.plen) {
            for (int i = g.pcur; i + 1 < g.plen; i++) g.pre[i] = g.pre[i + 1];
            g.plen--;
            g.raw_ok = false;
        }
        return;
    case K_LEFT:  rom_flush(); if (g.pcur > 0) g.pcur--; g.raw_ok = false; return;
    case K_RIGHT: rom_flush(); if (g.pcur < g.plen) g.pcur++; g.raw_ok = false; return;
    case K_HOME:  rom_flush(); g.pcur = 0; g.raw_ok = false; return;
    case K_END:   rom_flush(); g.pcur = g.plen; return;
    case K_ESC:   pre_reset(); return;
    case K_UP: case K_DOWN: case K_SLEFT: case K_SRIGHT: case K_F8: case K_OTHER: return;
    case K_F6: case K_F7: case K_F9: case K_F10: pre_respell(kt); return;
    case K_CTRL:
        if (ch == 0x03 || ch == 0x07) { pre_reset(); return; }        /* Ctrl-C / Ctrl-G: give up */
        /* fall through */
    default:
        pre_commit();
        pass(key, kt);
        return;
    }
}

static void idle_key(key_t kt, char ch, const char *key)
{
    switch (kt) {
    case K_CHAR:
        if (ch >= 'a' && ch <= 'z') { pre_reset(); rom_feed(ch); return; }
        if (ch >= 'A' && ch <= 'Z') { pre_reset(); pre_insert((uint16_t)(uint8_t)ch); raw_add(ch); return; }
        if (ch != '-' && romaji_punct(ch)) { uint16_t p = romaji_punct(ch); out_cps(&p, 1); return; }
        break;
    case K_F6: g.mode = IME_HIRA; g.last_on = IME_HIRA; return;
    case K_F7: g.mode = IME_KATA; g.last_on = IME_KATA; return;
    default: break;
    }
    pass(key, kt);
}

static void toggle(void)
{
    if (g.converting) conv_commit();
    else if (composing()) pre_commit();
    if (g.mode == IME_OFF) g.mode = g.last_on;
    else { g.last_on = g.mode; g.mode = IME_OFF; }
}

/* ---- the screen ----------------------------------------------------------------------------- */

static void cell_add(cells_t *c, uint16_t cp, int style)
{
    if (c->n >= CELLS) return;
    c->cp[c->n] = cp;
    c->st[c->n] = (uint8_t)style;
    c->n++;
}

static void cells_text(cells_t *c, const char *u8, int style, int maxcells)
{
    int n = 0;
    while (*u8 && n < maxcells) { uint32_t cp; u8 += ime_u8_dec(u8, &cp); cell_add(c, (uint16_t)cp, style); n++; }
}

/* What is being composed, as styled cells. */
static void build_preedit(cells_t *c)
{
    c->n = 0;
    c->focus_at = 0;
    if (g.converting) {
        for (int i = 0; i < s_cv.nseg; i++) {
            if (i == g.focus) c->focus_at = c->n;
            int style = i == g.focus ? FBCON_OVL_FOCUS : (i & 1) ? FBCON_OVL_PREEDIT : FBCON_OVL_SEGMENT;
            for (int k = 0; k < s_cv.seg[i].tlen; k++) cell_add(c, s_cv.seg[i].text[k], style);
        }
        return;
    }
    for (int i = 0; i < g.pcur; i++) cell_add(c, g.pre[i], FBCON_OVL_PREEDIT);
    c->focus_at = c->n;
    for (int i = 0; i < g.rom.n; i++) cell_add(c, (uint16_t)(uint8_t)g.rom.pend[i], FBCON_OVL_PENDING);
    if (g.pcur < g.plen) {
        cell_add(c, g.pre[g.pcur], FBCON_OVL_FOCUS);                  /* the character after the insertion point */
        for (int i = g.pcur + 1; i < g.plen; i++) cell_add(c, g.pre[i], FBCON_OVL_PREEDIT);
    } else {
        cell_add(c, ' ', FBCON_OVL_FOCUS);                            /* block cursor at the end */
    }
}

/* The candidate list of the focused phrase: one line per candidate plus a counter. Returns the lines. */
static int build_cand_window(cells_t lines[CAND_LINES], int *width)
{
    int sel = s_cv.seg[g.focus].sel, page = sel / PAGE, first = page * PAGE;
    int nl = 0, w = 6;
    for (int i = first; i < B->cl.n && i < first + PAGE; i++) {
        cells_t *c = &lines[nl];
        c->n = 0;
        int style = i == sel ? FBCON_OVL_CAND_SEL : FBCON_OVL_CAND;
        cell_add(c, (uint16_t)('1' + (i - first)), style);
        cell_add(c, ' ', style);
        cells_text(c, B->cl.pool + B->cl.off[i], style, CAND_W_MAX);
        cell_add(c, ' ', style);
        if (c->n > w) w = c->n;
        nl++;
    }
    cells_t *f = &lines[nl];
    f->n = 0;
    char cnt[24];
    ksnprintf(cnt, sizeof cnt, " %d/%d", sel + 1, B->cl.n);
    cells_text(f, cnt, FBCON_OVL_CAND, 12);
    nl++;
    for (int i = 0; i < nl; i++) while (lines[i].n < w) cell_add(&lines[i], ' ', lines[i].n ? lines[i].st[0] : FBCON_OVL_CAND);
    *width = w;
    return nl;
}

static void emit_runs(const cells_t *c, int row_off, int col0, int cols, int cur_c)
{
    /* cells flow from column col0 of row row_off, wrapping at `cols`; spans are relative to the text cursor */
    int i = 0;
    while (i < c->n) {
        int pos = col0 + i, r = row_off + pos / cols, col = pos % cols;
        int j = i + 1;
        while (j < c->n && c->st[j] == c->st[i] && (col0 + j) % cols != 0) j++;
        fbcon_overlay_text(FBCON_ANCHOR_CURSOR, r, col - cur_c, &c->cp[i], j - i, c->st[i]);
        i = j;
    }
}

static void badge(void)
{
    uint16_t b = g.mode == IME_KATA ? 0x30A2 : 0x3042;               /* ア / あ */
    fbcon_overlay_text(FBCON_ANCHOR_SCREEN, 0, -1, &b, 1, FBCON_OVL_BADGE);
}

static void view_overlay(void)
{
    cells_t *pre = &B->pre, *lines = B->lines;
    int rows, cols, cr, cc;
    fbcon_get_geometry(&rows, &cols, &cr, &cc);
    fbcon_overlay_begin();
    if (g.mode != IME_OFF) badge();
    if (composing() || g.converting) {
        build_preedit(pre);
        emit_runs(pre, 0, cc, cols, cc);
        if (g.converting && g.listing && B->cl.n > 0) {
            int w, nl = build_cand_window(lines, &w);
            int last_row = (cc + pre->n - 1) / cols, first_row = (cc + pre->focus_at) / cols;
            int row = last_row + 1;
            if (cr + row + nl > rows) row = first_row - nl;           /* no room below: above the reading */
            int col = (cc + pre->focus_at) % cols;
            if (col + w > cols) col = cols - w;
            if (col < 0) col = 0;
            for (int i = 0; i < nl; i++) emit_runs(&lines[i], row + i, col, cols, cc);
        }
    }
    fbcon_overlay_end();
    g.shown = g.mode != IME_OFF || composing() || g.converting;
}

static void bar_hide(void)
{
    if (g.bar_saved && g.bar_buf) display_draw_bitmap(0, g.snap_y, DISPLAY_WIDTH, g.snap_rows * CELL, g.bar_buf);
    g.bar_saved = false;
}

/* Copies rows [row0, row0 + rows) of the screen so that they can be put back. */
static bool snap_save(int row0, int rows)
{
    if (!g.bar_buf) g.bar_buf = kmalloc_psram((size_t)DISPLAY_WIDTH * SNAP_ROWS * CELL * sizeof(uint16_t));
    if (!g.bar_buf) return false;
    g.snap_y = row0 * CELL;
    g.snap_rows = rows;
    g.drawn_rows = 0;
    display_read_bitmap(0, g.snap_y, DISPLAY_WIDTH, rows * CELL, g.bar_buf);
    g.bar_saved = true;
    return true;
}

static void bar_draw_cells(const cells_t *c, int from, int row_y, int cols)
{
    int i = from, x = 0;
    while (i < c->n && x < cols) {
        int j = i + 1;
        while (j < c->n && c->st[j] == c->st[i] && j - i < cols - x) j++;
        uint16_t fg, bg;
        fbcon_overlay_colors(c->st[i], &fg, &bg);
        display_draw_codepoints(x * CELL, row_y, &c->cp[i], j - i, fg, bg, SCALE);
        x += j - i;
        i = j;
    }
    if (x < cols) {                                                  /* the rest of the line, plain */
        uint16_t sp[SCR_COLS], fg, bg;
        for (int k = 0; k < cols - x; k++) sp[k] = ' ';
        fbcon_overlay_colors(FBCON_OVL_BAR, &fg, &bg);
        display_draw_codepoints(x * CELL, row_y, sp, cols - x, fg, bg, SCALE);
    }
}

static void view_bar(void)
{
    cells_t *line = &B->bline, *cand = &B->bcand, *pre = &B->bpre;
    bool want = composing() || g.converting;
    if (!want) { bar_hide(); g.shown = false; return; }
    int y0 = DISPLAY_HEIGHT - 2 * CELL, y1 = y0 + CELL;
    if (!g.bar_saved || g.snap_y != y0) {
        bar_hide();
        if (!snap_save(SCR_ROWS - 2, 2)) return;
    }
    build_preedit(pre);
    line->n = 0;
    cell_add(line, g.mode == IME_KATA ? 0x30A2 : 0x3042, FBCON_OVL_BADGE);
    cell_add(line, ' ', FBCON_OVL_BAR);
    int room = SCR_COLS - line->n;
    int from = 0;
    if (pre->n > room) { from = pre->focus_at - room / 2; if (from < 0) from = 0; if (from > pre->n - room) from = pre->n - room; }
    for (int i = from; i < pre->n && line->n < SCR_COLS; i++) cell_add(line, pre->cp[i], pre->st[i]);
    bar_draw_cells(line, 0, y1, SCR_COLS);
    if (g.converting && g.listing && B->cl.n > 0) {
        int sel = s_cv.seg[g.focus].sel, first = sel / PAGE * PAGE;
        cand->n = 0;
        for (int i = first; i < B->cl.n && i < first + PAGE; i++) {
            int style = i == sel ? FBCON_OVL_CAND_SEL : FBCON_OVL_CAND;
            cell_add(cand, (uint16_t)('1' + (i - first)), style);
            cell_add(cand, ':', style);
            cells_text(cand, B->cl.pool + B->cl.off[i], style, CAND_W_MAX);
            cell_add(cand, ' ', style);
        }
        char cnt[24];
        ksnprintf(cnt, sizeof cnt, "%d/%d", sel + 1, B->cl.n);
        cells_text(cand, cnt, FBCON_OVL_BAR, 12);
        bar_draw_cells(cand, 0, y0, SCR_COLS);
    } else {
        display_draw_bitmap(0, y0, DISPLAY_WIDTH, CELL, g.bar_buf);   /* the upper row of the bar is the program's again */
    }
    g.shown = true;
}

/* Paints cells flowing from column col0 of row ay + row_off, wrapping at the screen edge. */
static void draw_runs(const cells_t *c, int col0, int row_off)
{
    int i = 0;
    while (i < c->n) {
        int pos = col0 + i, row = g.ay + row_off + pos / SCR_COLS, col = pos % SCR_COLS;
        int j = i + 1;
        while (j < c->n && c->st[j] == c->st[i] && (col0 + j) % SCR_COLS != 0) j++;
        uint16_t fg, bg;
        fbcon_overlay_colors(c->st[i], &fg, &bg);
        display_draw_codepoints(col * CELL, row * CELL, &c->cp[i], j - i, fg, bg, SCALE);
        i = j;
    }
}

/* Composition drawn at the program's insertion point (ime_set_anchor), candidate list below it. */
static bool view_inline(void)
{
    if (!(composing() || g.converting)) { bar_hide(); g.shown = false; return true; }
    cells_t *pre = &B->bpre, *lines = B->lines;
    build_preedit(pre);
    int prerows = (g.ax + pre->n - 1) / SCR_COLS + 1, nl = 0, w = 0;
    if (g.converting && g.listing && B->cl.n > 0) nl = build_cand_window(lines, &w);
    int need = prerows + nl, avail = SCR_ROWS - g.ay;
    if (need > SNAP_ROWS || need > avail) return false;                    /* no room: use the bottom bar instead */
    int rows = avail < SNAP_ROWS ? avail : SNAP_ROWS;
    if (!g.bar_saved || g.snap_y != g.ay * CELL || g.snap_rows != rows) {
        bar_hide();
        if (!snap_save(g.ay, rows)) return true;
    } else if (g.drawn_rows > 0) {
        display_draw_bitmap(0, g.snap_y, DISPLAY_WIDTH, g.drawn_rows * CELL, g.bar_buf);      /* wipe the last picture */
    }
    draw_runs(pre, g.ax, 0);
    if (nl > 0) {
        int col = (g.ax + pre->focus_at) % SCR_COLS;
        if (col + w > SCR_COLS) col = SCR_COLS - w;
        if (col < 0) col = 0;
        for (int i = 0; i < nl; i++) draw_runs(&lines[i], col, prerows + i);
    }
    g.drawn_rows = need;
    g.shown = true;
    return true;
}

static void view_update(void);

void ime_bypass(bool on)
{
    if (on == g.bypass) return;
    g.bypass = on;
    if (on && B && (composing() || g.converting)) { pre_reset(); view_update(); }      /* drop a half-typed word */
}

void ime_set_anchor(int cx, int cy)
{
    if (g.bar_saved && (g.ax != cx || g.ay != cy)) bar_hide();            /* the field moved: the old picture is stale */
    g.ax = cx; g.ay = cy; g.anchor_set = true;
}

void ime_clear_anchor(void)
{
    if (g.anchor_set) { bar_hide(); g.shown = false; }
    g.anchor_set = false;
}

static void view_update(void)
{
    if (g.app_view) return;
    bool bar = fbcon_is_paused();
    if (!bar && g.bar_saved) bar_hide();
    if (bar) {
        if (g.shown && !g.bar_saved) { fbcon_overlay_begin(); fbcon_overlay_end(); }       /* the console overlay is stale */
        if (!(g.anchor_set && view_inline())) view_bar();
    } else {
        view_overlay();
    }
}

/* ---- public --------------------------------------------------------------------------------- */

void ime_init(void)
{
    if (g.inited) return;
    g.inited = true;
    B = kmalloc_psram(sizeof *B);
    if (!B) { kprintf("ime: no PSRAM for the input method (%u bytes)\n", (unsigned)sizeof *B); return; }
    memset(B, 0, sizeof *B);
    g.mode = IME_OFF;
    g.last_on = IME_HIRA;
    g.raw_ok = true;
    ime_dict_add(ime_mini_dict, ime_mini_dict_size);
    ime_dict_load_async();
}

bool ime_active(void) { return g.mode != IME_OFF; }

const char *ime_status(void)
{
    return g.mode == IME_OFF ? "off" : g.mode == IME_HIRA ? "hiragana" : "katakana";
}

void ime_feed(const char *key)
{
    if (!g.inited) ime_init();
    if (!B || g.bypass) { out_str(key); return; }                   /* no memory, or an app wants the raw keys */
    char ch;
    key_t kt = parse_key(key, &ch);
    if (kt == K_TOGGLE) toggle();
    else if (g.mode == IME_OFF) pass(key, kt);
    else if (g.converting) conv_key(kt, ch, key);
    else if (composing()) pre_key(kt, ch, key);
    else idle_key(kt, ch, key);
    g.view_revision++;
    if (g.mode != IME_OFF || g.shown) view_update();
}

void ime_set_app_view(bool enabled)
{
    bar_hide();
    fbcon_overlay_begin(); fbcon_overlay_end();
    uint32_t irq = arch_irq_disable();
    pre_reset(); g.app_view = enabled; g.shown = false; g.view_revision++;
    arch_irq_restore(irq);
}

bool ime_read_view(ime_view *out)
{
    uint32_t irq = arch_irq_disable();
    memset(out, 0, sizeof *out);
    if (!B) { arch_irq_restore(irq); return false; }
    out->revision = g.view_revision; out->active = g.mode != IME_OFF;
    out->converting = g.converting; out->listing = g.listing;
    build_preedit(&B->bpre);
    out->count = B->bpre.n;
    for (int i = 0; i < out->count; i++) {
        out->text[i] = B->bpre.cp[i]; out->styles[i] = B->bpre.st[i];
    }
    out->selected = -1;
    if (g.converting && g.listing) {
        int selected = s_cv.seg[g.focus].sel, first = selected / PAGE * PAGE;
        for (int i = first; i < B->cl.n && i < first + PAGE; i++) {
            int n = out->candidate_count++;
            const char *text = B->cl.pool + B->cl.off[i];
            size_t length = strlen(text);
            if (length >= sizeof out->candidates[n]) length = sizeof out->candidates[n] - 1;
            while (length && ((unsigned char)text[length] & 0xc0) == 0x80) length--;
            memcpy(out->candidates[n], text, length); out->numbers[n] = i - first + 1;
            if (i == selected) out->selected = n;
        }
    }
    arch_irq_restore(irq); return true;
}
