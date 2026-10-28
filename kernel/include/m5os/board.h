#pragma once
#include <m5os/types.h>

/* Board-level bring-up (power ICs, resets).  CoreS3 implementation lives in
 * hal/esp32s3/board_cores3.c. */
bool board_init(void);                       /* false if a power IC does not answer */
void board_backlight(uint8_t level);         /* 0 = off .. 255 = full */

/* Tab5 */
uint8_t board_backlight_get(void);            /* the level last set by board_backlight() */
/* Switch the 5 V pin of the extension port (the USB-A host port's 5 V is a separate switch and is not touched).
 * *was_on (optional) gets the state before the call.  False if the I/O expander does not answer. */
bool board_ext5v_set(bool on, bool *was_on);
