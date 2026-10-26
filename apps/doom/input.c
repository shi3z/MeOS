#include <m5os/board_config.h>
#if M5OS_DOOM
#include <m5os/kprintf.h>
#include <m5os/hal_time.h>
#include "../../drivers/tab5kbd/tab5touch.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "runtime.h"
#include "layout.h"
#include <string.h>

static uint32_t s_held[256], s_touch_poll_at, s_escape_at;
static bool s_touch[256], s_sent[256];
static unsigned s_escape_state;
void doom_input_reset(void)
{
    memset(s_held, 0, sizeof s_held); memset(s_touch, 0, sizeof s_touch);
    memset(s_sent, 0, sizeof s_sent);
    s_touch_poll_at = 0; s_escape_at = 0; s_escape_state = 0;
}
static int convert_key(int c)
{
    if (c == 'q' || c == 'Q' || c == 3) doom_exit(0);
    if (c == 'w' || c == 'W') return KEY_UPARROW;
    if (c == 's' || c == 'S') return KEY_DOWNARROW;
    if (c == 'a' || c == 'A') return KEY_LEFTARROW;
    if (c == 'd' || c == 'D') return KEY_RIGHTARROW;
    if (c == ' ') return KEY_FIRE;
    if (c == 'e' || c == 'E') return KEY_USE;
    if (c == '\n' || c == '\r') return KEY_ENTER;
    return c;
}
int DG_GetKey(int *pressed, unsigned char *key)
{
    uint32_t now = DG_GetTicksMs();
    if (now - s_touch_poll_at >= 20) {
        s_touch_poll_at = now;
        tab5_touch_point_t points[TAB5_TOUCH_MAX_POINTS];
        uint64_t began=hal_time_now_us();
        unsigned n = tab5_touch_read_points(points, TAB5_TOUCH_MAX_POINTS);
        doom_profile_input((uint32_t)(hal_time_now_us()-began));
        memset(s_touch, 0, sizeof s_touch);
        for (unsigned j = 0; j < n; j++) {
            int x = points[j].x, y = points[j].y;
            int i = doom_button_at(x,y);
            if (i < 0) continue;
            if (!doom_buttons[i].key) doom_exit(0);
            s_touch[doom_buttons[i].key] = true;
        }
    }
    for (int i = 0; i < 256; i++)
        if (s_held[i] && (int32_t)(now - s_held[i]) >= 0) s_held[i] = 0;
    /* Serial/text keyboards have no key-up: repeats refresh a short hold.
     * Merge both sources before emitting transitions, so a keyboard timeout
     * cannot release a key that is still held by a finger (or vice versa). */
    int c;
    for (unsigned read = 0; read < 64 && (c = kconsole_getc()) >= 0; read++) {
        if (s_escape_state == 1 && (c == '[' || c == 'O')) { s_escape_state = 2; continue; }
        if (s_escape_state == 2) {
            c = c == 'A' ? KEY_UPARROW : c == 'B' ? KEY_DOWNARROW : c == 'C' ? KEY_RIGHTARROW : c == 'D' ? KEY_LEFTARROW : -1;
            s_escape_state = 0; if (c < 0) continue;
        } else if (c == 27) { s_escape_state = 1; s_escape_at = now; continue; }
        else s_escape_state = 0;
        c = convert_key(c);
        if (c < 0 || c > 255) continue;
        s_held[c] = now + 180; if (!s_held[c]) s_held[c] = 1;
    }
    if (s_escape_state && now - s_escape_at > 50) {
        s_escape_state = 0; s_held[KEY_ESCAPE] = now + 100;
        if (!s_held[KEY_ESCAPE]) s_held[KEY_ESCAPE] = 1;
    }
    for (int i = 0; i < 256; i++) {
        bool wanted = s_touch[i] || s_held[i];
        if (wanted != s_sent[i]) {
            s_sent[i] = wanted; *key = i; *pressed = wanted; return 1;
        }
    }
    return 0;
}
#endif
