#include "home.h"
#include "../tui/dqwin.h"
#include "../../drivers/lcd/fbcon.h"
#include <m5os/task.h>

typedef struct {
    home_action_t action;
    const char *label;
    const char *detail;
} home_item_t;

static const home_item_t items[] = {
    { HOME_WRITE,     "Write",     "Japanese prose and notes" },
    { HOME_SCHEME,    "Scheme",    "Editor, REPL and plots" },
    { HOME_FILES,     "Files",     "Browse and manage storage" },
    { HOME_CAMERA,    "Camera",    "Viewfinder and BMP capture" },
    { HOME_WEB,       "Web",       "Japanese text web browser" },
    { HOME_WIFI,      "Wi-Fi",     "Networks and connection" },
    { HOME_SSH,       "SSH",       "Saved remote hosts" },
    { HOME_TAILSCALE, "Tailscale", "Mesh VPN status and login" },
    { HOME_APPS,      "Apps",      "Installed WebAssembly apps" },
    { HOME_BASIC,     "BASIC",     "Interactive programming" },
#if M5OS_DOOM
    { HOME_DOOM,      "Doom",      "Native game from your IWAD" },
#endif
#if M5OS_QUAKE
    { HOME_QUAKE,     "Quake",     "Native game from SD assets" },
#endif
    { HOME_SHELL,     "Shell",     "Command line and diagnostics" },
};

#define COLS 2

int home_item_count(void) { return (int)(sizeof items / sizeof items[0]); }

home_action_t home_item_action(int index)
{
    return index >= 0 && index < home_item_count() ? items[index].action : HOME_NONE;
}

const char *home_item_label(int index)
{
    return index >= 0 && index < home_item_count() ? items[index].label : "";
}

const char *home_item_detail(int index)
{
    return index >= 0 && index < home_item_count() ? items[index].detail : "";
}

typedef struct { int x, y, w, h; } tile_t;

static tile_t tile_geometry(int index)
{
    int cols = dq_cols(), rows = dq_rows();
    int nrows = (home_item_count() + COLS - 1) / COLS;
    int gap = 2, margin = 2, top = 4, footer = 4;
    int w = (cols - margin * 2 - gap) / COLS;
    int h = (rows - top - footer) / nrows;
    if (h < 4) h = 4;
    return (tile_t){ margin + (index % COLS) * (w + gap),
                     top + (index / COLS) * h, w, h };
}

static void padded_text(int x, int y, int width, const char *text, bool reverse)
{
    char line[96];
    int n = 0;
    while (text[n] && n < width && n < (int)sizeof line - 1) { line[n] = text[n]; n++; }
    while (n < width && n < (int)sizeof line - 1) line[n++] = ' ';
    line[n] = 0;
    if (reverse) dq_text_rev(x, y, line); else dq_text(x, y, line);
}

static void draw_tile(int index, bool selected)
{
    tile_t t = tile_geometry(index);
    dq_window(t.x, t.y, t.w, t.h);
    padded_text(t.x + 2, t.y + 1, t.w - 4, items[index].label, selected);
    if (t.h >= 5) padded_text(t.x + 2, t.y + 3, t.w - 4, items[index].detail, false);
}

static void draw(int selected)
{
    int cols = dq_cols(), rows = dq_rows();
    dq_clear();
    dq_window(0, 0, cols, 3);
    dq_text(3, 1, "MeOS HOME");
    dq_text(cols - 25, 1, "M5Stack Tab5 / microSD");
    for (int i = 0; i < home_item_count(); i++) draw_tile(i, i == selected);
    dq_window(0, rows - 3, cols, 3);
    dq_text(3, rows - 2, "Arrows / touch: select   Enter: open   Q/Esc: shell");
}

static int touched_item(int px, int py)
{
    int cell = fbcon_cell_px();
    if (cell <= 0) return -1;
    int x = px / cell, y = py / cell;
    for (int i = 0; i < home_item_count(); i++) {
        tile_t t = tile_geometry(i);
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) return i;
    }
    return -1;
}

home_action_t home_select(int *selected, home_touch_reader_t touch, void *context)
{
    int count = home_item_count();
    int current = selected && *selected >= 0 && *selected < count ? *selected : 0;
    int pressed = -1;
    bool was_down = false;
    draw(current);

    for (;;) {
        int key = dq_pollkey();
        int next = current;
        if (key == DQK_LEFT && current % COLS) next--;
        else if (key == DQK_RIGHT && current % COLS + 1 < COLS && current + 1 < count) next++;
        else if (key == DQK_UP && current >= COLS) next -= COLS;
        else if (key == DQK_DOWN && current + COLS < count) next += COLS;
        else if (key == DQK_HOME) next = 0;
        else if (key == DQK_END) next = count - 1;
        else if (key == DQK_ENTER || key == ' ') {
            if (selected) *selected = current;
            return items[current].action;
        } else if (key == DQK_ESC || key == 'q' || key == 'Q') {
            if (selected) *selected = current;
            return HOME_SHELL;
        }
        if (next != current) {
            draw_tile(current, false);
            current = next;
            draw_tile(current, true);
        }

        if (touch) {
            int x = 0, y = 0;
            bool down = touch(&x, &y, context);
            int hit = down ? touched_item(x, y) : -1;
            if (down && !was_down) {
                pressed = hit;
                if (hit >= 0 && hit != current) {
                    draw_tile(current, false);
                    current = hit;
                    draw_tile(current, true);
                }
            } else if (!down && was_down) {
                int released = pressed;
                pressed = -1;
                if (released >= 0 && released == current) {
                    if (selected) *selected = current;
                    return items[current].action;
                }
            }
            was_down = down;
        }
        task_sleep(20);
    }
}
