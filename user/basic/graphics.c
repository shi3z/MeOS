#include "graphics.h"
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/display.h>
#include <m5os/task.h>
#include <m5os/ime.h>
#include "../../drivers/lcd/fbcon.h"
#include <string.h>

static int point(int32_t x, int32_t y)
{
    return x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT;
}
const char *basic_graphics_command(basic_graphics *g, int op, const int32_t a[5],
                                   const char *text, size_t len,
                                   int (*cancel)(void *), void *opaque)
{
    if (op == UBASIC_SCREEN) {
        if (!a[0]) {
            if (g->active) {
                display_set_target(NULL);
                ime_bypass(false);
                fbcon_pause(false);
                g->active = 0;
                display_release_claim(g);
            }
            return NULL;
        }
        if (a[0] != 1) return "SCREEN mode must be 0 or 1";
        if (g->active) return NULL;
        if (!display_is_ready()) return "display unavailable";
        if (fbcon_is_paused() || !display_try_claim(g)) return "display busy";
        g->active = 1; g->background = 0;
        fbcon_pause(true); task_sleep(100);
        ime_bypass(true); display_set_target(NULL); display_clear(0);
        return NULL;
    }
    if (op == UBASIC_WAIT) {
        if (a[0] < 0 || a[0] > 60000) return "WAIT must be 0..60000 milliseconds";
        int32_t left = a[0];
        do {
            if (cancel && cancel(opaque)) { ubasic_interrupt(); return NULL; }
            if (!left) break;
            uint32_t n = left > 10 ? 10 : (uint32_t)left;
            task_sleep(n); left -= n;
        } while (1);
        return NULL;
    }
    if (!g->active) return "use SCREEN 1 before drawing";
    if (op != UBASIC_CLS && op != UBASIC_PSET && op != UBASIC_LINE && op != UBASIC_TEXT) return "unknown graphics operation";
    int color = op == UBASIC_CLS ? 0 : op == UBASIC_LINE ? 4 : 2;
    if (a[color] < 0 || a[color] > 65535) return "color must be RGB565 0..65535";
    if (op != UBASIC_CLS && !point(a[0], a[1])) return "pixel coordinate out of range";
    if (op == UBASIC_LINE && !point(a[2], a[3])) return "pixel coordinate out of range";
    if (op == UBASIC_CLS) { g->background = (uint16_t)a[0]; display_clear(g->background); }
    else if (op == UBASIC_PSET) display_fill_rect(a[0], a[1], 1, 1, (uint16_t)a[2]);
    else if (op == UBASIC_LINE) display_draw_line(a[0], a[1], a[2], a[3], (uint16_t)a[4]);
    else {
        if (len >= BASIC_LINE_CAP || (!text && len)) return "graphics text too long";
        for (size_t i = 0; i < len; i++) if ((unsigned char)text[i] < 32 || (unsigned char)text[i] > 126) return "TEXT requires printable ASCII";
        char copy[BASIC_LINE_CAP]; if (len) memcpy(copy, text, len); copy[len] = 0;
        display_draw_text(a[0], a[1], copy, (uint16_t)a[2], g->background, 2);
    }
    return NULL;
}
#else
const char *basic_graphics_command(basic_graphics *g, int op, const int32_t args[5],
                                   const char *text, size_t length,
                                   int (*cancel)(void *), void *opaque)
{
    (void)g; (void)text; (void)length; (void)cancel; (void)opaque;
    return op == UBASIC_SCREEN && !args[0] ? NULL : "graphics requires Tab5";
}
#endif
