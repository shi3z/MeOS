/*
 * On-screen keyboard for the Tab5 (no physical keyboard needed).
 *
 * The console (drivers/lcd/fbcon.c) is told to use only the top rows of the screen; the keyboard owns
 * what is left: 5 rows of keys, 16 pixels of margin.  A task polls the touch panel (tab5touch.c) every
 * 20 ms, finds the key under a new finger, and feeds the key's HID usage through hid_decode() to
 * ime_feed(), exactly like the USB keyboard driver does, so Enter, arrows, Ctrl-C and the Japanese
 * input method all behave the same.  Shift and Ctrl are one-shot (tap, then the key); tapping Shift
 * twice locks it.  Backspace, Delete, Space and the arrows repeat while held.
 *
 * Hidden, the keyboard folds into a one-row bar at the bottom of the screen ("keyboard" at the right);
 * tapping it brings the keys back.
 *
 * Full-screen programs (files, edit, ...) pause the console and own the whole display.  The keyboard is
 * out of their way then, but a tap on the RIGHT EDGE of the screen calls it up at any time, as an
 * overlay: the pixels it covers are saved first, and the display's bottom clip (display_clip_bottom)
 * makes the program's own drawing skip that area while the keys are up, so the two never mix.  "Hide"
 * puts the saved pixels back and asks the program to repaint (kconsole_request_refresh), because
 * whatever it drew under the keyboard meanwhile was discarded.
 */
#include "osk.h"
#include "tab5touch.h"
#include "../usbkbd/hid_keys.h"
#include "../lcd/fbcon.h"
#include <m5os/display.h>
#include <m5os/ime.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/battery.h>
#include <m5os/suspend.h>
#include <m5os/power.h>
#include <string.h>

#define UNIT            40              /* key widths are multiples of this; 1280 = 32 units */
#define KEY_H           52
#define ROW_PITCH       56
#define KEY_ROWS        5
#define KBD_H           (KEY_ROWS * ROW_PITCH + 8)
#define POLL_MS         20u
#define REPEAT_DELAY_MS 400u
#define REPEAT_PERIOD_MS 60u
#define SHIFT_LOCK_MS   500u
#define BATTERY_REFRESH_MS 5000u        /* how often the bar's battery text is re-read (the bar only) */
#define BAR_MARGIN      16              /* left margin of the battery text in the bar */
#define EDGE_PX         64              /* a tap this close to the right edge calls the keyboard up */
#define TASK_STACK      12288           /* the input method converts on this stack */

#define COL_KBD_BG      RGB565(18, 18, 24)
#define COL_KEY         RGB565(52, 52, 64)
#define COL_KEY_SPECIAL RGB565(34, 34, 46)
#define COL_KEY_DOWN    RGB565(90, 140, 220)
#define COL_KEY_ACTIVE  RGB565(60, 110, 90)     /* a latched modifier */
#define COL_TEXT        RGB565(240, 240, 245)
#define COL_BAR         RGB565(30, 30, 40)

/* HID usages (USB HID usage tables, keyboard page) */
#define U_ENTER 0x28
#define U_ESC   0x29
#define U_BKSP  0x2A
#define U_TAB   0x2B
#define U_SPACE 0x2C
#define U_DEL   0x4C
#define U_RIGHT 0x4F
#define U_LEFT  0x50
#define U_DOWN  0x51
#define U_UP    0x52
#define U_SHIFT 0xF1                    /* not HID usages: handled here */
#define U_CTRL  0xF2
#define U_JP    0xF3                    /* Ctrl+Space: Japanese input on/off */
#define U_HIDE  0xF4
#define U_SLEEP 0xF5                    /* the "Zz" key: suspend now */

typedef struct { const char *lo, *hi; uint8_t usage; uint8_t w; } key_t;   /* w in UNITs */

static const key_t ROW0[] = {
    { "`", "~", 0x35, 2 }, { "1", "!", 0x1E, 2 }, { "2", "@", 0x1F, 2 }, { "3", "#", 0x20, 2 }, { "4", "$", 0x21, 2 },
    { "5", "%", 0x22, 2 }, { "6", "^", 0x23, 2 }, { "7", "&", 0x24, 2 }, { "8", "*", 0x25, 2 }, { "9", "(", 0x26, 2 },
    { "0", ")", 0x27, 2 }, { "-", "_", 0x2D, 2 }, { "=", "+", 0x2E, 2 }, { "Bksp", "Bksp", U_BKSP, 6 }, { 0 } };
static const key_t ROW1[] = {
    { "Tab", "Tab", U_TAB, 3 }, { "q", "Q", 0x14, 2 }, { "w", "W", 0x1A, 2 }, { "e", "E", 0x08, 2 }, { "r", "R", 0x15, 2 },
    { "t", "T", 0x17, 2 }, { "y", "Y", 0x1C, 2 }, { "u", "U", 0x18, 2 }, { "i", "I", 0x0C, 2 }, { "o", "O", 0x12, 2 },
    { "p", "P", 0x13, 2 }, { "[", "{", 0x2F, 2 }, { "]", "}", 0x30, 2 }, { "\\", "|", 0x31, 2 }, { "Del", "Del", U_DEL, 3 }, { 0 } };
static const key_t ROW2[] = {
    { "Ctrl", "Ctrl", U_CTRL, 3 }, { "a", "A", 0x04, 2 }, { "s", "S", 0x16, 2 }, { "d", "D", 0x07, 2 }, { "f", "F", 0x09, 2 },
    { "g", "G", 0x0A, 2 }, { "h", "H", 0x0B, 2 }, { "j", "J", 0x0D, 2 }, { "k", "K", 0x0E, 2 }, { "l", "L", 0x0F, 2 },
    { ";", ":", 0x33, 2 }, { "'", "\"", 0x34, 2 }, { "Enter", "Enter", U_ENTER, 7 }, { 0 } };
static const key_t ROW3[] = {
    { "Shift", "Shift", U_SHIFT, 5 }, { "z", "Z", 0x1D, 2 }, { "x", "X", 0x1B, 2 }, { "c", "C", 0x06, 2 }, { "v", "V", 0x19, 2 },
    { "b", "B", 0x05, 2 }, { "n", "N", 0x11, 2 }, { "m", "M", 0x10, 2 }, { ",", "<", 0x36, 2 }, { ".", ">", 0x37, 2 },
    { "/", "?", 0x38, 2 }, { "^", "^", U_UP, 2 }, { "Shift", "Shift", U_SHIFT, 5 }, { 0 } };
static const key_t ROW4[] = {
    { "Esc", "Esc", U_ESC, 3 }, { "Jp/En", "Jp/En", U_JP, 4 }, { "Zz", "Zz", U_SLEEP, 3 }, { "", "", U_SPACE, 13 }, { "<", "<", U_LEFT, 2 },
    { "v", "v", U_DOWN, 2 }, { ">", ">", U_RIGHT, 2 }, { "Hide", "Hide", U_HIDE, 3 }, { 0 } };
static const key_t *const ROWS_[KEY_ROWS] = { ROW0, ROW1, ROW2, ROW3, ROW4 };

static battery_tracker_t s_bat_track;
static char     s_bat_text[32] = "Bat --";
static uint64_t s_bat_at;
static bool s_avail, s_visible = true, s_debug;
static bool s_ov;                                 /* the overlay keyboard is up (a full-screen program owns the screen) */
static uint16_t *s_saved;                         /* the screen under the overlay */
static int  s_top;                                /* y of the keyboard area (visible) or of the bar (hidden) */
static int  s_shift, s_ctrl;                      /* 0 off, 1 one-shot, 2 locked (shift only) */
static const key_t *s_down;                       /* the key under the finger, if any */
static int  s_down_row, s_down_x, s_down_y;

static int key_x(const key_t *row, const key_t *k)
{
    int x = 0;
    for (const key_t *p = row; p != k; p++) x += p->w * UNIT;
    return x;
}

static bool is_repeating(uint8_t u) { return u == U_BKSP || u == U_DEL || u == U_SPACE || u == U_LEFT || u == U_RIGHT || u == U_UP || u == U_DOWN; }

static void draw_key(const key_t *row, int r, const key_t *k, bool down)
{
    int x = key_x(row, k), y = s_top + 4 + r * ROW_PITCH, w = k->w * UNIT;
    bool special = k->usage >= U_ENTER || k->usage == U_TAB;
    uint16_t bg = down ? COL_KEY_DOWN : (k->usage == U_SHIFT && s_shift) || (k->usage == U_CTRL && s_ctrl) ? COL_KEY_ACTIVE : special ? COL_KEY_SPECIAL : COL_KEY;
    display_fill_rect(x + 2, y, w - 4, KEY_H, bg);
    const char *label = s_shift ? k->hi : k->lo;
    int len = 0; while (label[len]) len++;
    if (len) display_draw_text(x + (w - len * 16) / 2, y + (KEY_H - 16) / 2, label, COL_TEXT, bg, 2);
}

static void draw_keyboard(void)
{
    display_fill_rect(0, s_top, DISPLAY_WIDTH, DISPLAY_HEIGHT - s_top, COL_KBD_BG);
    for (int r = 0; r < KEY_ROWS; r++)
        for (const key_t *k = ROWS_[r]; k->lo; k++) draw_key(ROWS_[r], r, k, false);
}

#define BAR_KBD_X   (DISPLAY_WIDTH - 12 * 16 - 8)            /* "[ keyboard ]" */
#define BAR_ZZ_X    (BAR_KBD_X - 6 * 16 - 32)                /* "[ Zz ]": suspend now, left of it */

static void draw_bar(void)
{
    int y = s_top + (DISPLAY_HEIGHT - s_top - 16) / 2;
    display_fill_rect(0, s_top, DISPLAY_WIDTH, DISPLAY_HEIGHT - s_top, COL_BAR);
    display_draw_text(BAR_MARGIN, y, s_bat_text, COL_TEXT, COL_BAR, 2);                       /* battery: left */
    display_draw_text(BAR_ZZ_X, y, "[ Zz ]", COL_TEXT, COL_BAR, 2);
    display_draw_text(BAR_KBD_X, y, "[ keyboard ]", COL_TEXT, COL_BAR, 2);   /* the way back to the keys: right */
}

/* Re-read the battery and rebuild the bar's text.  Returns true if the text changed. */
static bool battery_refresh(void)
{
    power_sample_t p;
    power_read(&p);
    uint64_t now = hal_time_now_us();
    if (p.result == POWER_OK) battery_tracker_update(&s_bat_track, p.battery_ua, now);
    battery_status_t st;
    battery_status_make(&s_bat_track, &p, M5OS_BATTERY_MAH, &st);
    char text[sizeof s_bat_text];
    battery_format(&st, text, sizeof text);
    s_bat_at = now;
    if (!strcmp(text, s_bat_text)) return false;
    memcpy(s_bat_text, text, sizeof text);
    return true;
}

static void layout(void)
{
    int cell = fbcon_cell_px();
    int rows = s_visible ? (DISPLAY_HEIGHT - KBD_H) / cell : fbcon_max_rows() - 1;
    s_top = rows * cell;
    fbcon_set_rows(rows);
    if (s_visible) draw_keyboard(); else draw_bar();
}

static const key_t *hit(int x, int y, int *row_out)
{
    if (y < s_top + 4) return NULL;
    int r = (y - s_top - 4) / ROW_PITCH;
    if (r >= KEY_ROWS) return NULL;
    int kx = 0;
    for (const key_t *k = ROWS_[r]; k->lo; k++) {
        int w = k->w * UNIT;
        if (x >= kx && x < kx + w) { *row_out = r; return k; }
        kx += w;
    }
    return NULL;
}

static void emit(const key_t *k)
{
    uint8_t mods = (s_shift ? HID_MOD_LSHIFT : 0) | (s_ctrl ? HID_MOD_LCTRL : 0);
    uint8_t usage = k->usage;
    if (usage == U_JP) { usage = U_SPACE; mods = HID_MOD_LCTRL; }
    char buf[8];
    hid_decode(usage, mods, false, buf);
    if (buf[0]) ime_feed(buf);
}

static void redraw_modifiers(void)
{
    for (int r = 0; r < KEY_ROWS; r++)
        for (const key_t *k = ROWS_[r]; k->lo; k++)
            if (k->usage == U_SHIFT || k->usage == U_CTRL) draw_key(ROWS_[r], r, k, false);
}

static void overlay_hide(void);

static void overlay_show(void)
{
    if (s_ov) return;
    if (!s_saved) s_saved = kmalloc_psram((size_t)DISPLAY_WIDTH * KBD_H * sizeof(uint16_t));
    if (!s_saved) return;                                     /* no memory: better no keyboard than a damaged screen */
    s_top = DISPLAY_HEIGHT - KBD_H;
    display_read_bitmap(0, s_top, DISPLAY_WIDTH, KBD_H, s_saved);
    s_ov = true;
    s_shift = s_ctrl = 0;
    draw_keyboard();
}

static void overlay_hide(void)
{
    if (!s_ov) return;
    display_draw_bitmap(0, s_top, DISPLAY_WIDTH, KBD_H, s_saved);
    s_ov = false;
    s_down = NULL;
    kconsole_request_refresh();                               /* what the program drew under the keys was discarded */
}

static void press(const key_t *k, int r)
{
    static uint64_t last_shift_us;
    uint64_t now = hal_time_now_us();
    if (k->usage == U_SLEEP) { suspend_request(); return; }
    if (k->usage == U_HIDE) { if (s_ov) overlay_hide(); else osk_show(false); return; }
    if (k->usage == U_SHIFT) {
        if (s_shift == 2) s_shift = 0;
        else if (s_shift == 1 && now - last_shift_us < SHIFT_LOCK_MS * 1000u) s_shift = 2;
        else s_shift = s_shift ? 0 : 1;
        last_shift_us = now;
        draw_keyboard();                                   /* labels change with Shift */
        return;
    }
    if (k->usage == U_CTRL) { s_ctrl = !s_ctrl; redraw_modifiers(); return; }
    emit(k);
    bool was = s_shift == 1 || s_ctrl;
    if (s_shift == 1) s_shift = 0;
    s_ctrl = 0;
    draw_key(ROWS_[r], r, k, true);
    if (was) { if (s_shift == 0) draw_keyboard(); else redraw_modifiers(); draw_key(ROWS_[r], r, k, true); }
}

static bool s_paused_before, s_down_before;
static uint64_t s_pressed_at, s_last_repeat;

/* One poll of the touch panel (every POLL_MS).  Runs with the display clip lifted, see osk_task. */
static void osk_poll(void)
{
    bool paused = fbcon_is_paused();
    if (paused && !s_paused_before) { s_paused_before = true; s_ov = false; s_down = NULL; s_down_before = false; }   /* a full-screen program starts: keys hidden */
    if (!paused && s_paused_before) {
        s_paused_before = false; s_ov = false; s_down = NULL;
        task_sleep(60);                                               /* the console repaints itself... */
        layout();                                                     /* ...then the keyboard area is drawn again */
    }
    bool up = paused ? s_ov : s_visible;                              /* are the keys on screen? */
    if (!paused && !s_visible && !display_is_suspended() && hal_time_now_us() - s_bat_at >= BATTERY_REFRESH_MS * 1000u)
        if (battery_refresh()) draw_bar();                            /* only while the bar is what is on screen */

    int x = 0, y = 0;
    bool down = tab5_touch_read(&x, &y);
    if (down && suspend_filter_input()) { s_down_before = true; s_down = NULL; return; }   /* asleep: this touch only wakes; it types nothing, even once awake */
    uint64_t now = hal_time_now_us();
    if (down && !s_down_before) {
        if (s_debug) { uint32_t f, e; int rx, ry, mx, my; tab5_touch_stats(&f, &e, &rx, &ry, &mx, &my); kprintf("osk: touch raw %d,%d -> %d,%d (map %d)\n", rx, ry, x, y, tab5_touch_map()); }
        if (!up) {
            if (x >= DISPLAY_WIDTH - EDGE_PX) { if (paused) overlay_show(); else osk_show(true); }
            else if (!paused && y >= s_top) {                         /* the bar */
                if (x >= BAR_ZZ_X - 16 && x < BAR_KBD_X - 8) suspend_request();
                else osk_show(true);
            }
        } else {
            int r;
            const key_t *k = hit(x, y, &r);
            if (k) { s_down = k; s_down_row = r; s_down_x = x; s_down_y = y; press(k, r); s_pressed_at = s_last_repeat = now; }
        }
    } else if (!down && s_down_before && s_down) {
        if (up && s_down->usage < U_SHIFT) draw_key(ROWS_[s_down_row], s_down_row, s_down, false);
        s_down = NULL;
    } else if (down && s_down && is_repeating(s_down->usage) &&
               now - s_pressed_at > REPEAT_DELAY_MS * 1000u && now - s_last_repeat > REPEAT_PERIOD_MS * 1000u) {
        emit(s_down); s_last_repeat = now;
    }
    s_down_before = down;
}

static void osk_task(void *arg)
{
    (void)arg;
    s_paused_before = fbcon_is_paused();
    layout();
    for (;;) {
        task_sleep(suspend_poll_ms(POLL_MS));
        /* This task outranks the programs it overlays and does not sleep while drawing, so it can lift the
         * display clip for its own drawing and put it back before anyone else runs. */
        display_clip_bottom(DISPLAY_HEIGHT);
        osk_poll();
        display_clip_bottom(s_ov ? s_top : DISPLAY_HEIGHT);
    }
}

void osk_show(bool on)
{
    if (!s_avail || on == s_visible) return;
    s_visible = on;
    s_down = NULL;
    layout();
}

bool osk_visible(void) { return s_avail && s_visible; }
bool osk_available(void) { return s_avail; }
void osk_set_debug(bool on) { s_debug = on; }

void osk_start(void)
{
    if (!tab5_touch_init()) return;
    s_avail = true;
    task_create_stack("osk", osk_task, NULL, TASK_PRIO_DEFAULT + 2, TASK_STACK);
    kprintf("osk: on-screen keyboard ready (shell: osk on|off|map N|debug)\n");
}
