#pragma once
#include <m5os/keyboard_raw.h>
#include "../../drivers/tab5kbd/tab5touch.h"
void quake_input_reset(void);
void quake_input_sample(int mode,const keyboard_raw_state *raw,const tab5_touch_point_t *points,unsigned count,uint32_t now);
void quake_input_byte(int c,int mode,uint32_t now);
int quake_input_next(int *down,int *key);
int quake_input_action(void); /* 1 quit, 2 diagnostic capture; consumes flags */
void quake_input_mouse(int *x,int *y);
uint32_t quake_input_touch_buttons(void);
