#pragma once
#include <m5os/types.h>
/* The keyboard API shared by the Tab5 keyboard drivers (drivers/tab5kbd, drivers/usbkbd).  keyboard_start()
 * finds a keyboard, starts a task that reads it, and feeds what is typed to the console input
 * (kconsole_inject), so the shell reads it exactly like characters arriving over the serial port. */
void        keyboard_start(void);
const char *keyboard_type(void);       /* "tab5-i2c", "usb-hid" or "none" */

/* Diagnostics for bring-up: how many key presses were seen, the last key code, and the last byte sent to
 * the console.  (i2c_errors counts failed bus/USB transfers.) */
typedef struct { uint32_t presses; int last_x, last_y; int last_char; uint32_t i2c_errors; } keyboard_stats_t;
void keyboard_stats(keyboard_stats_t *out);
