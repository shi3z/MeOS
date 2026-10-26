#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../../drivers/lcd/bitmap_resize.h"
#include "../../drivers/lcd/ppa_stage.h"
#include "../../drivers/lcd/dma_column.h"
static uint16_t stage[320*720] __attribute__((aligned(4)));
static uint16_t actual[DISPLAY_WIDTH*DISPLAY_HEIGHT],expected[DISPLAY_WIDTH*DISPLAY_HEIGHT];
static uint8_t pixels[64000] __attribute__((aligned(16))),previous[64000] __attribute__((aligned(16)));
static uint8_t unaligned[64002] __attribute__((aligned(16))),old_unaligned[64002] __attribute__((aligned(16)));
static uint16_t decoded[64000];
static void test_copy_alignment(void){
 uint16_t src[724] __attribute__((aligned(16))),dst[728] __attribute__((aligned(16))),ref[728];
 uint16_t triplet[DISPLAY_HEIGHT*3+8] __attribute__((aligned(16))),triplet_ref[DISPLAY_HEIGHT*3+8];
 for(unsigned i=0;i<724;i++)src[i]=(uint16_t)(i*173+7);
 for(unsigned so=0;so<4;so++)for(unsigned off=0;off<4;off++)for(unsigned n=0;n<=720;n++){
  memset(dst,0x5a,sizeof dst);memcpy(ref,dst,sizeof dst);
  memcpy(ref+off,src+so,n*sizeof *src);tab5_copy_column(dst+off,src+so,n);
  assert(!memcmp(dst,ref,sizeof dst));
  memset(triplet,0x5a,sizeof triplet);memcpy(triplet_ref,triplet,sizeof triplet);
  for(unsigned row=0;row<3;row++)memcpy(triplet_ref+off+row*DISPLAY_HEIGHT,src+so,n*sizeof *src);
  tab5_copy_column_triplet(triplet+off,src+so,n);assert(!memcmp(triplet,triplet_ref,sizeof triplet));
 }
}
int main(void){
 test_copy_alignment();
 uint16_t palette[256];for(unsigned i=0;i<256;i++)palette[i]=(uint16_t)(i*239);
 for(unsigned i=0;i<64000;i++)pixels[i]=(uint8_t)(i*13);
 memset(actual,0x5a,sizeof actual);memcpy(expected,actual,sizeof actual);
 for(unsigned frame=0;frame<100;frame++){
  bool force=frame==0||frame==50;
  if(frame==50)palette[17]^=0xffff;
  if(frame%3==1)for(unsigned i=0;i<64000;i++)if(i/320<152)pixels[i]=(uint8_t)(i+frame);
  if(frame%3==2)for(unsigned i=0;i<40;i++)pixels[(frame*613+i*577)%64000]^=37;
  if(frame%7==3)for(unsigned y=24;y<152;y++)for(unsigned x=0;x<320;x++)pixels[y*320+x]^=37;
  for(unsigned i=0;i<64000;i++)decoded[i]=palette[pixels[i]];
  /* Generic RGB reference uses independent scalar scaling and no column helper. */
  int first,last;assert(tab5_bitmap_resize(expected,160,0,320,200,decoded,960,720,&first,&last));
  bool changed=tab5_indexed_viewport_delta(actual,160,pixels,previous,palette,force,&first,&last);
  if(force)assert(changed&&first==160&&last==1119);
  assert(!memcmp(actual,expected,sizeof actual));assert(!memcmp(previous,pixels,sizeof pixels));
  assert(!tab5_indexed_viewport_delta(actual,160,pixels,previous,palette,false,&first,&last));
 }
 int first,last;assert(!tab5_indexed_viewport_delta(actual,-1,pixels,previous,palette,true,&first,&last));
 memset(unaligned,0x5a,sizeof unaligned);memset(old_unaligned,0x5a,sizeof old_unaligned);
 memcpy(unaligned+1,pixels,sizeof pixels);memcpy(old_unaligned+1,pixels,sizeof pixels);
 for(unsigned byte=0;byte<4;byte++){
  unaligned[1+320*24+byte]^=37;pixels[320*24+byte]^=37;
  for(unsigned i=0;i<64000;i++)decoded[i]=palette[pixels[i]];
  assert(tab5_bitmap_resize(expected,160,0,320,200,decoded,960,720,&first,&last));
  assert(tab5_indexed_viewport_delta(actual,160,unaligned+1,old_unaligned+1,palette,false,&first,&last));
  assert(!memcmp(actual,expected,sizeof actual));assert(!memcmp(old_unaligned+1,pixels,sizeof pixels));
 }
 assert(unaligned[0]==0x5a&&unaligned[64001]==0x5a&&old_unaligned[0]==0x5a&&old_unaligned[64001]==0x5a);
 tab5_ppa_stage(stage,pixels,palette);
 for(unsigned y=0;y<720;y++)for(unsigned x=0;x<320;x++)assert(stage[y*320+x]==palette[pixels[(y*200/720)*320+x]]);
 uint16_t dma_column[768] __attribute__((aligned(128)));
 for(unsigned x=0;x<320;x++){
  memset(dma_column,0x5a,sizeof dma_column);tab5_dma_column(dma_column,pixels,palette,x);
  for(unsigned y=0;y<720;y++)assert(dma_column[y]==palette[pixels[(y*200/720)*320+x]]);
  for(unsigned y=720;y<768;y++)assert(dma_column[y]==0x5a5a);
 }
 for(unsigned first_row=0;first_row<200;first_row++)for(unsigned last_row=first_row;last_row<200;last_row++){
  unsigned start=((first_row*18+4)/5)&~1u,end=(((last_row+1)*18+4)/5+1)&~1u;
  assert(!(start&1)&&!(end&1)&&end<=720&&start<=((first_row*18+4)/5)&&end>=(((last_row+1)*18+4)/5));
 }
 puts("Quake delta display: copy lengths 0..720/all halfword alignments, 100 independent RGB reference matches, odd-start motion, sparse edits, palette and guards PASS");
}
