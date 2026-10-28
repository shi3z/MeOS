#include "hid_keys.h"
#include <m5os/ime.h>

/* HID usage 0x04..0x38 -> ASCII, unshifted and shifted (US layout). */
static const char UNSHIFTED[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\x7f\t -=[]\\#;'`,./";
static const char SHIFTED[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\x7f\t _+{}|~:\"~<>?";
#define USAGE_LAST   0x38
#define USAGE_LETTER_LAST 0x1D        /* 'z' */

static void put(char out[8], const char *s)
{
    int i = 0;
    while (s[i] && i < 7) { out[i] = s[i]; i++; }
    out[i] = 0;
}

void hid_decode(uint8_t usage, uint8_t mods, bool caps, char out[8])
{
    for (int i = 0; i < 8; i++) out[i] = 0;
    bool shift = mods & (HID_MOD_LSHIFT | HID_MOD_RSHIFT);
    bool ctrl  = mods & (HID_MOD_LCTRL | HID_MOD_RCTRL);
    if ((ctrl && usage == 0x2C) || ((mods & HID_MOD_LALT) && usage == 0x35)) {      /* Ctrl+Space, Alt+` : Japanese input on/off */
        put(out, IME_TOGGLE_KEY);
        return;
    }
    if (usage >= 0x4F && usage <= 0x52 && shift) {                                  /* Shift+arrow: ESC [ 1 ; 2 X */
        put(out, usage == 0x4F ? "\x1b[1;2C" : usage == 0x50 ? "\x1b[1;2D" : usage == 0x51 ? "\x1b[1;2B" : "\x1b[1;2A");
        return;
    }
    if (usage >= HID_USAGE_FIRST && usage <= USAGE_LAST) {
        int i = usage - HID_USAGE_FIRST;
        bool letter = usage <= USAGE_LETTER_LAST;
        bool upper = letter ? (shift != caps) : shift;                 /* Caps Lock only flips letters */
        char c = upper ? SHIFTED[i] : UNSHIFTED[i];
        if (ctrl && letter) c = (char)(UNSHIFTED[i] - 'a' + 1);        /* Ctrl-C = 0x03, ... */
        out[0] = c;
        return;
    }
    switch (usage) {
    case 0x3A: out[0] = 0x1B; out[1] = 'O'; out[2] = 'P'; break;           /* F1 */
    case 0x3B: out[0] = 0x1B; out[1] = 'O'; out[2] = 'Q'; break;           /* F2 */
    case 0x3C: out[0] = 0x1B; out[1] = 'O'; out[2] = 'R'; break;           /* F3 */
    case 0x3D: out[0] = 0x1B; out[1] = 'O'; out[2] = 'S'; break;           /* F4 */
    case 0x3E: put(out, "\x1b[15~"); break;                                    /* F5..F12 */
    case 0x3F: put(out, "\x1b[17~"); break;
    case 0x40: put(out, "\x1b[18~"); break;
    case 0x41: put(out, "\x1b[19~"); break;
    case 0x42: put(out, "\x1b[20~"); break;
    case 0x43: put(out, "\x1b[21~"); break;
    case 0x44: put(out, "\x1b[23~"); break;
    case 0x45: put(out, "\x1b[24~"); break;
    case 0x4A: out[0] = 0x1B; out[1] = '['; out[2] = 'H'; break;           /* Home */
    case 0x4B: out[0] = 0x1B; out[1] = '['; out[2] = '5'; out[3] = '~'; break; /* PageUp */
    case 0x4C: out[0] = 0x1B; out[1] = '['; out[2] = '3'; out[3] = '~'; break; /* Delete */
    case 0x4D: out[0] = 0x1B; out[1] = '['; out[2] = 'F'; break;           /* End */
    case 0x4E: out[0] = 0x1B; out[1] = '['; out[2] = '6'; out[3] = '~'; break; /* PageDown */
    case 0x4F: out[0] = 0x1B; out[1] = '['; out[2] = 'C'; break;           /* right */
    case 0x50: out[0] = 0x1B; out[1] = '['; out[2] = 'D'; break;           /* left  */
    case 0x51: out[0] = 0x1B; out[1] = '['; out[2] = 'B'; break;           /* down  */
    case 0x52: out[0] = 0x1B; out[1] = '['; out[2] = 'A'; break;           /* up    */
    default: break;
    }
}
