#pragma once
#include <stdint.h>
/* Fixed RGB565 stage for P4 SRM: preserve the original vertical nearest-neighbor
 * positions. dst must be 32-bit aligned; the DMA caller provides 128 alignment. */
static inline void tab5_ppa_stage(uint16_t *dst,const uint8_t *pixels,const uint16_t palette[256]){
 for(unsigned y=0;y<720;y++){
  const uint8_t *src=pixels+(y*200/720)*320;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  typedef uint32_t alias_word __attribute__((may_alias));
  alias_word *packed=(alias_word *)(dst+y*320);
  for(unsigned x=0;x<320;x+=2)packed[x/2]=(uint32_t)palette[src[x]]|((uint32_t)palette[src[x+1]]<<16);
#else
  for(unsigned x=0;x<320;x++)dst[y*320+x]=palette[src[x]];
#endif
 }
}
