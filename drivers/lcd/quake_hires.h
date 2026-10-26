#pragma once
#include "bitmap_resize.h"
/* Read adjacent PSRAM pixels together before expanding columns. A full indexed
 * frame is larger than L2; walking all 300 rows separately for each column
 * repeatedly evicts adjacent pixels. The 19,200-byte tile lives on the internal
 * task stack and is never passed to DMA. */
#define TAB5_HIRES_TILE_COLS 64
static inline void tab5_hires_stage(uint8_t tile[TAB5_HIRES_TILE_COLS][300],
    const uint8_t *pixels,unsigned x,unsigned count){
 for(unsigned y=0;y<300;y++)for(unsigned i=0;i<count;i++)tile[i][y]=pixels[y*512+x+i];
}
static inline void tab5_hires_expand(uint16_t *dst,const uint8_t column[300],
    const uint16_t palette[256],unsigned first,unsigned last){
 typedef uint32_t word __attribute__((may_alias));word *out=(word *)dst;
 for(unsigned y=first;y<=last;y++){uint32_t color=palette[column[y]];out[y]=color|(color<<16);}
}
static inline bool tab5_hires_damage(const uint8_t *pixels,uint8_t *previous,bool force,
    uint16_t first[512],uint16_t last[512],unsigned *left,unsigned *right){
 for(unsigned x=0;x<512;x++){first[x]=300;last[x]=0;}
 if(force){for(unsigned x=0;x<512;x++){first[x]=0;last[x]=299;}memcpy(previous,pixels,512*300);}
 else if(!(((uintptr_t)pixels|(uintptr_t)previous)&3u)){
  typedef uint32_t word __attribute__((may_alias));const word *now=(const word *)pixels;word *old=(word *)previous;
  for(unsigned y=0;y<300;y++)for(unsigned x=0;x<512;x+=4){
   unsigned at=(y*512+x)/4;uint32_t value=now[at],diff=value^old[at];if(!diff)continue;old[at]=value;
   for(unsigned b=0;b<4;b++,diff>>=8)if(diff&255u){if(first[x+b]==300)first[x+b]=(uint16_t)y;last[x+b]=(uint16_t)y;}
  }
 } else for(unsigned y=0;y<300;y++)for(unsigned x=0;x<512;x++){
  unsigned at=y*512+x;uint8_t value=pixels[at];
  if(previous[at]!=value){if(first[x]==300)first[x]=(uint16_t)y;last[x]=(uint16_t)y;previous[at]=value;}
 }
 *left=512;*right=0;for(unsigned x=0;x<512;x++)if(first[x]<300){if(*left==512)*left=x;*right=x;}
 return *left!=512;
}
static inline bool tab5_hires_delta(uint16_t *fb,const uint8_t *pixels,uint8_t *previous,
    const uint16_t palette[256],bool force,int *first_col,int *last_col){
 if(!fb||!pixels||!previous||!palette)return false;
 uint16_t first[512],last[512];unsigned left,right;
 if(!tab5_hires_damage(pixels,previous,force,first,last,&left,&right))return false;
 uint32_t column[300] __attribute__((aligned(16)));
 uint8_t tile[TAB5_HIRES_TILE_COLS][300];
 for(unsigned base=left;base<=right;base+=TAB5_HIRES_TILE_COLS){
  unsigned count=right-base+1;if(count>TAB5_HIRES_TILE_COLS)count=TAB5_HIRES_TILE_COLS;
  bool dirty=false;for(unsigned i=0;i<count;i++)if(first[base+i]<300)dirty=true;
  if(!dirty)continue;
  tab5_hires_stage(tile,pixels,base,count);
  for(unsigned i=0;i<count;i++){unsigned x=base+i;if(first[x]>=300)continue;
  tab5_hires_expand((uint16_t *)column,tile[i],palette,first[x],last[x]);
  for(unsigned repeat=0;repeat<2;repeat++)tab5_copy_column(fb+(1279-(128+x*2+repeat))*720+60+first[x]*2,
   (const uint16_t *)column+first[x]*2,(last[x]-first[x]+1)*2);
  }
 }
 *first_col=128+(int)left*2;*last_col=128+(int)right*2+1;return true;
}
