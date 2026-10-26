#pragma once
#include <stdint.h>
#include <m5os/quake_video.h>
/* One exact 200->720 RGB565 column, matching floor(y*200/720). */
static inline void tab5_dma_column(uint16_t *dst,const uint8_t *pixels,const uint16_t *palette,unsigned col){
#if M5OS_QUAKE_HIGHRES
 typedef uint32_t word __attribute__((may_alias));word *out=(word *)dst;
 for(unsigned y=0;y<QV_HEIGHT;y++){uint32_t color=palette[pixels[y*QV_WIDTH+col]];out[y]=color|(color<<16);}
#else
 typedef uint32_t word __attribute__((may_alias));word *out=(word *)dst;
 for(unsigned y=0;y<200;y+=5){
  uint32_t a=palette[pixels[y*320+col]],b=palette[pixels[(y+1)*320+col]],c=palette[pixels[(y+2)*320+col]],d=palette[pixels[(y+3)*320+col]],e=palette[pixels[(y+4)*320+col]];
  out[0]=out[1]=a|(a<<16);out[2]=out[3]=b|(b<<16);out[4]=c|(c<<16);
  out[5]=c|(d<<16);out[6]=d|(d<<16);out[7]=d|(e<<16);out[8]=e|(e<<16);out+=9;
 }
#endif
}
