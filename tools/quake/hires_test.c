#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/lcd/quake_hires.h"
#include "../../drivers/lcd/dma_column.h"
#include "../../apps/quake/layout.h"
static uint8_t pixels[512*300] __attribute__((aligned(16))),previous[512*300] __attribute__((aligned(16)));
static uint8_t unaligned[512*300+2] __attribute__((aligned(16))),old_unaligned[512*300+2] __attribute__((aligned(16)));
static uint16_t rgb[512*300],actual[1280*720],expected[1280*720];
int main(void){
 uint16_t palette[256],column[768] __attribute__((aligned(128)));
 for(unsigned i=0;i<256;i++)palette[i]=(uint16_t)(i*239);
 memset(actual,0x5a,sizeof actual);memcpy(expected,actual,sizeof actual);
 for(unsigned f=0;f<40;f++){
  for(unsigned y=0;y<300;y++)for(unsigned x=0;x<512;x++)if(f==0||(f%2&&y>255)||(f%2==0&&x%11==0))pixels[y*512+x]=(uint8_t)(y+x+f);
  if(f==20)palette[17]^=0xffff;
  for(unsigned i=0;i<512*300;i++)rgb[i]=palette[pixels[i]];
  int a,b;assert(tab5_bitmap_resize(expected,128,60,512,300,rgb,1024,600,&a,&b));
  tab5_hires_delta(actual,pixels,previous,palette,f==0||f==20,&a,&b);
  assert(!memcmp(actual,expected,sizeof actual));assert(!memcmp(previous,pixels,sizeof pixels));
  assert(!tab5_hires_delta(actual,pixels,previous,palette,false,&a,&b));
 }
 for(unsigned x=0;x<512;x++){
  memset(column,0x5a,sizeof column);tab5_dma_column(column,pixels,palette,x);
  for(unsigned y=0;y<600;y++)assert(column[y]==palette[pixels[(y/2)*512+x]]);
  for(unsigned y=600;y<768;y++)assert(column[y]==0x5a5a);
 }
 /* Partial tiles, untouched rows, last column and native DMA destination guards. */
 uint8_t tile[TAB5_HIRES_TILE_COLS][300];
 for(unsigned base=0;base<512;base+=13){
  unsigned count=512-base;if(count>13)count=13;
  memset(tile,0xa5,sizeof tile);tab5_hires_stage(tile,pixels,base,count);
  for(unsigned i=0;i<TAB5_HIRES_TILE_COLS;i++)for(unsigned y=0;y<300;y++)
   assert(tile[i][y]==(i<count?pixels[y*512+base+i]:0xa5));
  for(unsigned i=0;i<count;i++){
   memset(column,0x5a,sizeof column);tab5_hires_expand(column,tile[i],palette,257,299);
   for(unsigned y=0;y<768;y++)assert(column[y]==(y>=514&&y<600?palette[pixels[(y/2)*512+base+i]]:0x5a5a));
  }
 }
 memset(unaligned,0x5a,sizeof unaligned);memset(old_unaligned,0x5a,sizeof old_unaligned);
 memcpy(unaligned+1,pixels,sizeof pixels);memcpy(old_unaligned+1,pixels,sizeof pixels);
 unaligned[1+299*512+511]^=37;
 for(unsigned i=0;i<512*300;i++)rgb[i]=palette[unaligned[1+i]];
 int a,b;assert(tab5_bitmap_resize(expected,128,60,512,300,rgb,1024,600,&a,&b));
 assert(tab5_hires_delta(actual,unaligned+1,old_unaligned+1,palette,false,&a,&b));
 assert(!memcmp(actual,expected,sizeof actual)&&!memcmp(unaligned+1,old_unaligned+1,sizeof pixels));
 assert(old_unaligned[0]==0x5a&&old_unaligned[sizeof old_unaligned-1]==0x5a);
 for(unsigned i=0;i<QUAKE_BUTTON_COUNT;i++){
  const quake_button *b=&quake_buttons[i];assert(b->x>=0&&b->x+b->w<=1280);
  assert(b->x+b->w<=128||b->x>=1152);
 }
 puts("512x300 baseline: independent RGB reference, rows above 255, palette, guards, DMA columns and control rails PASS");
}
