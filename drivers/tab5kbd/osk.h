#pragma once
#include <m5os/types.h>
/* On-screen keyboard for the Tab5: a QWERTY keyboard drawn under the LCD console, driven by the touch panel.
 * Taps go through the same HID decoding and the Japanese input method as a USB keyboard. */
void osk_start(void);                    /* after fbcon_start(); does nothing without a touch controller */
void osk_show(bool on);
bool osk_visible(void);
bool osk_available(void);
void osk_set_debug(bool on);             /* print every touch-down (raw and mapped) to the console */
