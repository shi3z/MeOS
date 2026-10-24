#include <m5os/board_config.h>
#if M5OS_QUAKE
#include "input.h"
#include "layout.h"
#include "../../drivers/usbkbd/hid_keys.h"
#include <string.h>
static bool sent[256],wanted[256],other[256];
typedef struct { uint8_t value; bool down,native; } input_event;
static input_event events[256];
static unsigned event_count,event_pos;
static uint8_t raw_live[32],raw_end[32];
static void enqueue(int value,bool down,bool native){if(value>=0&&value<256&&event_count<256)events[event_count++]=(input_event){(uint8_t)value,down,native};}
static uint32_t until[256],escape_at;
static unsigned escape_state,escape_param;
static uint32_t touch_mask;
static int action,look_id,look_x,look_y,mouse_x,mouse_y;
static void hold(int key,uint32_t now){if(key>=0&&key<256){until[key]=now+500;if(!until[key])until[key]=1;}}
void quake_input_reset(void){memset(sent,0,sizeof sent);memset(wanted,0,sizeof wanted);memset(until,0,sizeof until);escape_state=0;action=0;look_id=-1;mouse_x=mouse_y=0;touch_mask=0;event_count=event_pos=0;memset(raw_live,0,sizeof raw_live);memset(raw_end,0,sizeof raw_end);}
static void text_key(int c,int mode,uint32_t now){
 if(c==3){action=1;return;}
 if(c=='\r'||c=='\n')c=K_ENTER;
 if(c==127)c=K_BACKSPACE;
 if(mode==0){if(c=='p'||c=='P'){if(action!=1)action=2;return;}if(c>='A'&&c<='Z')c+=32;}
 if(mode!=0){enqueue(c,false,false);enqueue(c,true,false);enqueue(c,false,false);}else hold(c,now);
}
void quake_input_byte(int c,int mode,uint32_t now){
 if(c==20){if(action!=1)action=3;return;} /* serial-only profiling A/B toggle */
 if(escape_state==1 && (c=='['||c=='O')){escape_state=2;return;}
 if(escape_state==2){
  if(c>='0'&&c<='9'){escape_param=(unsigned)(c-'0');escape_state=3;return;}
  escape_state=0;
  int k=c=='A'?K_UPARROW:c=='B'?K_DOWNARROW:c=='C'?K_RIGHTARROW:c=='D'?K_LEFTARROW:c=='H'?K_HOME:c=='F'?K_END:-1;
  text_key(k,mode,now);return;
 }
 if(escape_state==3){
  if(c>=0x40&&c<=0x7e){
   int k=c=='A'?K_UPARROW:c=='B'?K_DOWNARROW:c=='C'?K_RIGHTARROW:c=='D'?K_LEFTARROW:
      c=='~'?(escape_param==5?K_PGUP:escape_param==6?K_PGDN:escape_param==3?K_DEL:-1):-1;
   escape_state=0;text_key(k,mode,now);
  }
  return;
 }
 if(escape_state==1){escape_state=0;text_key(K_ESCAPE,mode,now);}
 if(c==27){escape_state=1;escape_at=now;return;}
 text_key(c,mode,now);
}
static int usage_key(unsigned usage){
 if(usage>=0x4f&&usage<=0x52){static const int arrows[]={K_RIGHTARROW,K_LEFTARROW,K_DOWNARROW,K_UPARROW};return arrows[usage-0x4f];}
 if(usage==0x4b)return K_PGUP;
 if(usage==0x4e)return K_PGDN;
 if(usage==0xe0||usage==0xe4)return K_CTRL;
 if(usage==0xe1||usage==0xe5)return K_SHIFT;
 if(usage==0xe2||usage==0xe6)return K_ALT;
 char text[8];hid_decode((uint8_t)usage,0,false,text);
 if(text[0]&& !text[1])return text[0]==127?K_BACKSPACE:(unsigned char)text[0];
 return -1;
}
void quake_input_sample(int mode,const keyboard_raw_state *raw,const tab5_touch_point_t *points,unsigned count,uint32_t now){
 memset(wanted,0,sizeof wanted);memset(raw_end,0,sizeof raw_end);mouse_x=mouse_y=0;touch_mask=0;
 if(escape_state && (int32_t)(now-escape_at)>=60){if(escape_state==1)text_key(K_ESCAPE,mode,now);escape_state=0;}
 for(unsigned k=0;k<256;k++){if(until[k]&&(int32_t)(now-until[k])>=0)until[k]=0;if(until[k])wanted[k]=true;}
 memcpy(other,wanted,sizeof other);
 if(raw&&raw->ready){
  memcpy(raw_end,raw->keys,32);
  for(unsigned m=0;m<8;m++)if(raw->mods&(1u<<m))raw_end[(224+m)/8]|=(uint8_t)(1u<<((224+m)%8));
  for(unsigned j=0;j<raw->count&&j<KEYBOARD_RAW_EVENTS;j++)enqueue(raw->events[j].usage,raw->events[j].down,true);
  for(unsigned usage=4;usage<256;usage++)if(raw->keys[usage/8]&(1u<<(usage%8))){int k=usage_key(usage);if(k>=0)wanted[k]=true;}
  if(raw->mods&0x11)wanted[K_CTRL]=true;
  if(raw->mods&0x22)wanted[K_SHIFT]=true;
  if(raw->mods&0x44)wanted[K_ALT]=true;
  if((raw->mods&0x11)&&(raw->keys[6/8]&(1u<<(6%8))))action=1;
 }
 int look=-1;
 for(unsigned j=0;j<count;j++){
  int i=quake_button_at(points[j].x,points[j].y);
  if(i>=0){touch_mask|=1u<<i;if(i==QB_EXIT)action=1;else{int key=quake_button_key((unsigned)i,mode==3);if(key>=0){wanted[key]=true;other[key]=true;}}}
  else if(mode==0 && points[j].x>=160&&points[j].x<1120 && points[j].y>=0&&points[j].y<720){
   if(look<0||look_id==points[j].id)look=(int)j;
  }
 }
 if(look>=0){
  const tab5_touch_point_t *p=&points[look];
  if(look_id==p->id){int dx=p->x-look_x,dy=p->y-look_y;if(dx<160&&dx>-160&&dy<160&&dy>-160){mouse_x=dx*2;mouse_y=dy*2;}}
  look_id=p->id;look_x=p->x;look_y=p->y;
 } else look_id=-1;
}

static bool native_holds(int key){for(unsigned u=4;u<256;u++)if((raw_live[u/8]&(1u<<(u%8)))&&usage_key(u)==key)return true;return false;}
int quake_input_next(int *down,int *key){
 while(event_pos<event_count){
  input_event e=events[event_pos++];int k=e.value;
  if(e.native){if(e.down)raw_live[k/8]|=(uint8_t)(1u<<(k%8));else raw_live[k/8]&=(uint8_t)~(1u<<(k%8));k=usage_key((unsigned)k);}
  if(e.native&&e.down&&k=='c'&&native_holds(K_CTRL))action=1;
  if(k<0||k>=256)continue;
  bool state=e.down||other[k]||native_holds(k);
  if(sent[k]!=state){sent[k]=state;*down=state;*key=k;return 1;}
 }
 event_count=event_pos=0;memcpy(raw_live,raw_end,sizeof raw_live);
 for(int k=0;k<256;k++)if(sent[k]!=wanted[k]){sent[k]=wanted[k];*down=sent[k];*key=k;return 1;}
 return 0;
}
uint32_t quake_input_touch_buttons(void){return touch_mask;}
int quake_input_action(void){int n=action;action=0;return n;}
void quake_input_mouse(int *x,int *y){*x=mouse_x;*y=mouse_y;mouse_x=mouse_y=0;}
#endif
