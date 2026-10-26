#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "../../components/quakeengine/audio_ring.h"
int main(void){
 int16_t ring[64],copy[64],a[640],b[640];quake_audio_state one={0},split={0};
 for(unsigned i=0;i<64;i++)ring[i]=copy[i]=(int16_t)((int)i*300-9000);
 quake_audio_render(ring,64,&one,a,40,11025,24000);
 for(unsigned n=0;n<40;n++){
  unsigned index=(n*11025/24000)*2;
  int16_t ref=(int16_t)(((int)copy[index]+copy[index+1])/2);
  assert(a[n*2]==ref&&a[n*2+1]==ref);
 }
 quake_audio_render(copy,64,&split,b,13,11025,24000);
 quake_audio_render(copy,64,&split,b+26,27,11025,24000);
 assert(!memcmp(a,b,80*sizeof(int16_t))&&!memcmp(&one,&split,sizeof one));
 memset(ring,0,sizeof ring);memset(&one,0,sizeof one);
 quake_audio_render(ring,64,&one,a,320,11025,24000);
 assert(one.cursor==(147*2)%64&&one.phase==0&&one.nonzero==0);
 for(unsigned i=0;i<640;i++)assert(a[i]==0);
 for(unsigned i=0;i<64;i++)ring[i]=30000;
 memset(&one,0,sizeof one);quake_audio_render(ring,64,&one,a,320,11025,24000);
 for(unsigned i=0;i<64;i++)assert(ring[i]==0);
 for(unsigned i=100;i<320;i++)assert(a[i*2]==0);
 puts("Quake audio ring: exact rational upsampling, mono, chunk continuity, phase/wrap and stall silence PASS");
}
