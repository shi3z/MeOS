/*
 * Launcher: the home screen.
 *
 *     M5OS
 *     [ Terminal ]
 *     [ Clock ]
 *     [ Calculator ]
 *     [ Snake ]
 *
 * Tapping a button starts that app - or, if it is already running, brings its window to the front.
 */
#include "../../apps/apps.h"
#include <m5os/gui.h>
#include <m5os/display.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>

#define BTN_COUNT 4
#define BTN_X     8
#define BTN_W     (GUI_CLIENT_W - 16)
#define BTN_H     38
#define BTN_PITCH 42
#define BTN_Y0    2
#define COL_BTN   RGB565(40, 70, 130)
#define COL_DOWN  RGB565(40, 160, 70)

static const struct { const char *label, *app; } s_items[BTN_COUNT] = {
    { "Terminal", "terminal" }, { "Clock", "clock" }, { "Calculator", "calculator" }, { "Snake", "snake" },
};

static bool streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void draw_button(window_t w, int i, bool down)
{
    int len = 0;
    while (s_items[i].label[len]) len++;
    uint16_t bg = down ? COL_DOWN : COL_BTN;
    int y = BTN_Y0 + i * BTN_PITCH;
    window_fill_rect(w, BTN_X, y, BTN_W, BTN_H, bg);
    window_draw_text(w, BTN_X + (BTN_W - len * 16) / 2, y + (BTN_H - 16) / 2, s_items[i].label, COLOR_WHITE, bg, 2);
}

static int button_at(int x, int y)
{
    if (x < BTN_X || x >= BTN_X + BTN_W || y < BTN_Y0) return -1;
    int i = (y - BTN_Y0) / BTN_PITCH;
    return (i < BTN_COUNT && (y - BTN_Y0) % BTN_PITCH < BTN_H) ? i : -1;
}

static void launch(const char *name)
{
    app_info_t all[APP_MAX];
    int n = app_list(all, APP_MAX);
    for (int i = 0; i < n; i++) {
        if (all[i].state == APP_RUNNING && streq(all[i].name, name) && gui_focus_app(all[i].app_id) == ERR_OK) return;
    }
    uint32_t id;
    int rc = app_start(name, &id);                                /* its window appears and takes the front */
    if (rc != ERR_OK) kprintf("launcher: cannot start %s (error %d)\n", name, rc);
}

void launcher_main(void *arg)
{
    (void)arg;
    int wi = window_create("MeOS", GUI_WIN_HOME);
    if (wi < 0) return;
    window_t w = (window_t)wi;
    int down = -1;

    for (;;) {
        gui_event_t ev;
        if (gui_next_event(&ev, 1000) != ERR_OK) continue;
        if (ev.type == GUI_EVT_CLOSE) break;
        if (ev.type == GUI_EVT_EXPOSE) {
            window_clear(w, COLOR_BLACK);
            for (int i = 0; i < BTN_COUNT; i++) draw_button(w, i, false);
            down = -1;
        } else if (ev.type == GUI_EVT_TOUCH_DOWN) {
            down = button_at(ev.x, ev.y);
            if (down >= 0) draw_button(w, down, true);
        } else if (ev.type == GUI_EVT_TOUCH_UP && down >= 0) {
            int i = down;
            down = -1;
            draw_button(w, i, false);
            if (button_at(ev.x, ev.y) == i) launch(s_items[i].app);     /* released on the same button */
        }
    }
    window_destroy(w);
}
