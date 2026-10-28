#pragma once
#include <m5os/types.h>
/* Tab5 touch panel: auto-detect GT911 (ILI9881C) or ST712x (ST7121/ST7123), on the system I2C bus.
 * Coordinates come out in the logical landscape frame of the display (0..1279, 0..719). */
bool tab5_touch_init(void);                      /* false: no supported touch controller on the bus */
const char *tab5_touch_name(void);               /* detected kind, or "none" */
bool tab5_touch_read(int *x, int *y);            /* true while a finger is down; x/y are then valid */
void tab5_touch_set_map(int map);                /* raw -> logical mapping 0..3 (bring-up aid, see tab5touch.c) */
int  tab5_touch_map(void);
void tab5_touch_stats(uint32_t *frames, uint32_t *errors, int *raw_x, int *raw_y, int *max_x, int *max_y);
