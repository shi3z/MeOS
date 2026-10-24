#include <m5os/keyboard_raw.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../apps/quake/input.h"
#include "../../apps/quake/layout.h"
static int owner,other_owner;
static unsigned irq;
uint32_t arch_irq_disable(void){unsigned old=irq;irq++;return old;}
void arch_irq_restore(uint32_t old){assert(irq==old+1);irq=old;}
static bool live[256];static int downs[256],ups[256];
static void drain(void){int down,key;unsigned n=0;while(quake_input_next(&down,&key)){assert(++n<800&&key>=0&&key<256);live[key]=down;if(down)downs[key]++;else ups[key]++;}}
static void sample(int mode,uint32_t now,const tab5_touch_point_t *points,unsigned n){keyboard_raw_state s;keyboard_raw_read(&owner,&s);quake_input_sample(mode,&s,points,n,now);drain();assert(!irq);}
static void reset(void){keyboard_raw_release(&owner);assert(keyboard_raw_claim(&owner));quake_input_reset();memset(live,0,sizeof live);memset(downs,0,sizeof downs);memset(ups,0,sizeof ups);}
int main(void){
 reset();quake_input_byte(20,0,0);assert(quake_input_action()==3);quake_input_byte(3,0,0);quake_input_byte(20,0,0);assert(quake_input_action()==1);assert(!keyboard_raw_claim(&other_owner));uint32_t epoch=keyboard_raw_generation();
 keyboard_raw_matrix(epoch,0xa2);keyboard_raw_matrix(epoch,0xb4);keyboard_raw_matrix(epoch,0xcd);sample(0,100,NULL,0);
 assert(live['w']&&live['d']&&live[K_SPACE]);keyboard_raw_matrix(epoch,0x34);sample(0,110,NULL,0);assert(live['w']&&!live['d']&&live[K_SPACE]);
 sample(0,5000,NULL,0);assert(live['w']);keyboard_raw_matrix(epoch,0x22);keyboard_raw_matrix(epoch,0x4d);sample(0,5010,NULL,0);assert(!live['w']&&!live[K_SPACE]);
 keyboard_raw_matrix(epoch,0xbd);keyboard_raw_matrix(epoch,0x3d);sample(3,5020,NULL,0);assert(downs[K_ENTER]==1&&ups[K_ENTER]==1&&!live[K_ENTER]);
 keyboard_raw_matrix(epoch,0xc0);keyboard_raw_matrix(epoch,0xc4);keyboard_raw_matrix(epoch,0x44);keyboard_raw_matrix(epoch,0x40);sample(0,5030,NULL,0);assert(quake_input_action()==1);
 keyboard_raw_release(&other_owner);assert(keyboard_raw_active());keyboard_raw_release(&owner);assert(!keyboard_raw_active());assert(keyboard_raw_claim(&owner));
 keyboard_raw_matrix(epoch,0xa2);sample(0,6000,NULL,0);assert(!live['w']); /* stale producer rejected */
 reset();epoch=keyboard_raw_generation();
 uint8_t report[8]={0x02,0,0x1a,0x07,0x2c,0,0,0};keyboard_raw_usb(epoch,report);sample(0,7000,NULL,0);assert(live['w']&&live['d']&&live[K_SHIFT]&&live[K_SPACE]);
 report[3]=0;keyboard_raw_usb(epoch,report);sample(0,7010,NULL,0);assert(!live['d']&&live['w']);
 report[2]=1;keyboard_raw_usb(epoch,report);sample(0,7020,NULL,0);assert(!live['w']&&!live[K_SHIFT]&&!live[K_SPACE]);
 reset();epoch=keyboard_raw_generation();
 keyboard_raw_matrix(epoch,0xa2);sample(0,8000,NULL,0);tab5_touch_point_t finger={80,350,1};
 keyboard_raw_matrix(epoch,0x22);sample(0,8010,&finger,1);assert(live['w']);sample(0,8020,NULL,0);assert(!live['w']);
 reset();quake_input_byte(27,3,9000);sample(3,9000,NULL,0);assert(!live[K_ESCAPE]);quake_input_byte('[',3,9010);quake_input_byte('A',3,9020);sample(3,9020,NULL,0);assert(downs[K_UPARROW]==1&&!downs[K_ESCAPE]);
 quake_input_byte('a',3,9030);quake_input_byte('a',3,9030);sample(3,9030,NULL,0);assert(downs['a']==2&&ups['a']==2);
 quake_input_byte(27,3,9040);sample(3,9110,NULL,0);assert(downs[K_ESCAPE]==1&&!live[K_ESCAPE]);
 quake_input_byte('w',0,9200);quake_input_byte(' ',0,9200);sample(0,9200,NULL,0);assert(live['w']&&live[K_SPACE]);sample(0,9800,NULL,0);assert(!live['w']&&!live[K_SPACE]);
 reset();tab5_touch_point_t look[2]={{700,350,4},{730,330,4}};sample(0,10000,look,1);int x,y;quake_input_mouse(&x,&y);assert(!x&&!y);
 sample(0,10010,look+1,1);quake_input_mouse(&x,&y);assert(x==60&&y==-40);quake_input_mouse(&x,&y);assert(!x&&!y);
 tab5_touch_point_t two[2]={{500,300,2},{740,335,4}};sample(0,10020,two,2);quake_input_mouse(&x,&y);assert(x==20&&y==10);
 for(unsigned i=0;i<QUAKE_BUTTON_COUNT;i++){const quake_button *b=&quake_buttons[i];assert((b->x+b->w<=160||b->x>=1120)&&b->x>=0&&b->x+b->w<=1280&&b->y>=0&&b->y+b->h<=720);assert(quake_button_at(b->x,b->y)==(int)i);}
 assert(quake_button_at(700,350)<0);assert(quake_button_key(QB_FIRE,1)==K_ENTER);assert(quake_button_key(QB_LEFT,0)=='a');
 reset();epoch=keyboard_raw_generation();for(unsigned i=0;i<80;i++)keyboard_raw_matrix(epoch,(uint8_t)((i%2?0:128)|0x22));sample(0,11000,NULL,0);assert(!live['w']);keyboard_raw_clear();sample(0,11010,NULL,0);assert(!live['w']);
 puts("Quake input: independent releases, held keys, brief taps, USB rollover, ownership epochs, ANSI/menu typing, source merging, touch layout and swipe tracking PASS");
}
