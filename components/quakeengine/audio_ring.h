#pragma once
#include <stdint.h>
/* Native-rate software DMA ring -> fixed-rate mono speaker PCM. The callback
 * consumes samples only when its integer phase advances, so repeated output
 * samples are preserved and consumed data cannot loop after a loader stall. */
typedef struct {uint32_t cursor,phase;uint32_t frames,nonzero;} quake_audio_state;
static inline void quake_audio_render(int16_t *ring,unsigned samples,quake_audio_state *state,
    int16_t *out,unsigned frames,unsigned input_rate,unsigned output_rate){
 uint32_t pos=__atomic_load_n(&state->cursor,__ATOMIC_RELAXED),phase=state->phase;
 for(unsigned i=0;i<frames;i++){
  int16_t mono=(int16_t)(((int)ring[pos]+ring[(pos+1)&(samples-1)])/2);
  out[i*2]=out[i*2+1]=mono;if(mono)state->nonzero++;
  phase+=input_rate;
  if(phase>=output_rate){phase-=output_rate;ring[pos]=ring[(pos+1)&(samples-1)]=0;pos=(pos+2)&(samples-1);}
 }
 state->phase=phase;state->frames+=frames;
 __atomic_store_n(&state->cursor,pos,__ATOMIC_RELEASE);
}
