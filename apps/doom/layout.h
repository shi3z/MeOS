#pragma once
#include <m5os/types.h>
#include "doomkeys.h"
/* Doom's 320x200 framebuffer was displayed at 4:3 on a CRT. Restore that
 * aspect at full panel height; leave two thumb-width control rails. */
#define DOOM_VIEW_X 160
#define DOOM_VIEW_Y 0
#define DOOM_VIEW_W 960
#define DOOM_VIEW_H 720
typedef struct { int x,y,w,h; uint8_t key; const char *label; } doom_button_t;
static const doom_button_t doom_buttons[] = {
    {   8,444, 68,112, KEY_LEFTARROW, "<" },
    {  84,444, 68,112, KEY_RIGHTARROW,">" },
    {   8,320,144,112, KEY_UPARROW,   "FORWARD" },
    {   8,568,144,112, KEY_DOWNARROW, "BACK" },
    {1128,424,144,256, KEY_FIRE,      "FIRE" },
    {1128,300,144,112, KEY_USE,       "USE" },
    {   8,  8,144, 72, KEY_ESCAPE,    "MENU" },
    {1128, 96,144, 72, KEY_ENTER,     "ENTER" },
    {1128,  8,144, 72, 0,             "QUIT" },
};
#define DOOM_BUTTON_COUNT (sizeof doom_buttons / sizeof doom_buttons[0])
static inline int doom_button_at(int x,int y)
{
    for (unsigned i=0;i<DOOM_BUTTON_COUNT;i++) {
        const doom_button_t *b=&doom_buttons[i];
        if (x>=b->x && y>=b->y && x<b->x+b->w && y<b->y+b->h) return (int)i;
    }
    return -1;
}
