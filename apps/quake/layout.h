#pragma once
#include "../../third_party/quakegeneric/source/quakekeys.h"
#include <m5os/quake_video.h>
#define QUAKE_VIEW_X QV_X
#define QUAKE_VIEW_W QV_OUT_WIDTH
enum { QB_MENU,QB_WEAPON,QB_FORWARD,QB_LEFT,QB_RIGHT,QB_BACK,QB_EXIT,QB_OK,QB_TURN_LEFT,QB_TURN_RIGHT,QB_JUMP,QB_FIRE };
typedef struct { int x,y,w,h,key; const char *label; } quake_button;
static const quake_button quake_buttons[]={
#if M5OS_QUAKE_HIGHRES
 {8,8,112,64,K_ESCAPE,"MENU"},{8,88,112,64,'/',"WEAPON"},
 {32,328,64,100,'w',"^"},{8,440,52,100,'a',"<"},{68,440,52,100,'d',">"},{32,552,64,100,'s',"v"},
 {1160,8,112,64,0,"EXIT"},{1160,88,112,64,K_ENTER,"OK"},
 {1160,232,52,96,K_LEFTARROW,"<"},{1220,232,52,96,K_RIGHTARROW,">"},
 {1160,344,112,104,K_SPACE,"JUMP"},{1160,464,112,216,K_CTRL,"FIRE"}
#else
 {8,8,144,64,K_ESCAPE,"MENU"},{8,88,144,64,'/',"WEAPON"},
 {44,328,72,100,'w',"^"},{8,440,68,100,'a',"<"},{84,440,68,100,'d',">"},{44,552,72,100,'s',"v"},
 {1128,8,144,64,0,"EXIT"},{1128,88,144,64,K_ENTER,"OK"},
 {1128,232,68,96,K_LEFTARROW,"<"},{1204,232,68,96,K_RIGHTARROW,">"},
 {1128,344,144,104,K_SPACE,"JUMP"},{1128,464,144,216,K_CTRL,"FIRE"}
#endif
};
#define QUAKE_BUTTON_COUNT (sizeof quake_buttons/sizeof quake_buttons[0])
static inline int quake_button_at(int x,int y){for(unsigned i=0;i<QUAKE_BUTTON_COUNT;i++){const quake_button *b=&quake_buttons[i];if(x>=b->x&&x<b->x+b->w&&y>=b->y&&y<b->y+b->h)return (int)i;}return -1;}
static inline int quake_button_key(unsigned i,int menu){if(i>=QUAKE_BUTTON_COUNT)return -1;if(menu){if(i==QB_FORWARD)return K_UPARROW;if(i==QB_BACK)return K_DOWNARROW;if(i==QB_LEFT)return K_LEFTARROW;if(i==QB_RIGHT)return K_RIGHTARROW;if(i==QB_FIRE)return K_ENTER;}return quake_buttons[i].key;}
