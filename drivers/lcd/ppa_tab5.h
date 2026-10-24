#pragma once
#include <m5os/types.h>
bool tab5_ppa_begin(uint32_t owner,uint16_t *framebuffer);
bool tab5_ppa_present(const uint8_t *pixels,const uint16_t palette[256]);
void tab5_ppa_end(uint32_t owner);
