#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../../drivers/lcd/bitmap_resize.h"
#include "../../drivers/lcd/ppa_stage.h"
static uint16_t stage[320*720] __attribute__((aligned(4)));
static uint16_t actual[DISPLAY_WIDTH*DISPLAY_HEIGHT],expected[DISPLAY_WIDTH*DISPLAY_HEIGHT];
static uint8_t pixels[64000],previous[64000];
int main(void){
 uint16_t palette[256];for(unsigned i=0;i<256;i++)palette[i]=(uint16_t)(i*239);
 for(unsigned i=0;i<64000;i++)pixels[i]=(uint8_t)(i*13);
 memset(actual,0x5a,sizeof actual);memcpy(expected,actual,sizeof actual);
 for(unsigned frame=0;frame<100;frame++){
  bool force=frame==0||frame==50;
  if(frame==50)palette[17]^=0xffff;
  if(frame%3==1)for(unsigned i=0;i<64000;i++)if(i/320<152)pixels[i]=(uint8_t)(i+frame);
  if(frame%3==2)for(unsigned i=0;i<40;i++)pixels[(frame*613+i*577)%64000]^=37;
  int first,last;assert(tab5_indexed_resize(expected,160,0,320,200,pixels,palette,960,720,&first,&last));
  bool changed=tab5_indexed_viewport_delta(actual,160,pixels,previous,palette,force,&first,&last);
  if(force)assert(changed&&first==160&&last==1119);
  assert(!memcmp(actual,expected,sizeof actual));assert(!memcmp(previous,pixels,sizeof pixels));
  assert(!tab5_indexed_viewport_delta(actual,160,pixels,previous,palette,false,&first,&last));
 }
 int first,last;assert(!tab5_indexed_viewport_delta(actual,-1,pixels,previous,palette,true,&first,&last));
 tab5_ppa_stage(stage,pixels,palette);
 for(unsigned y=0;y<720;y++)for(unsigned x=0;x<320;x++)assert(stage[y*320+x]==palette[pixels[(y*200/720)*320+x]]);
 puts("Quake delta display: 100 full/reference matches, moving viewport/static HUD, sparse edits, palette change, no-change and untouched rails PASS");
}
