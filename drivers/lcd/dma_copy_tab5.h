#pragma once
#include <m5os/types.h>
bool tab5_dma_copy_begin(uint32_t owner,uint16_t *fb);
bool tab5_dma_copy_present(const uint8_t *pixels,uint8_t *previous,const uint16_t palette[256],bool force);
void tab5_dma_copy_end(uint32_t owner);
