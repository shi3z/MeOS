#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../third_party/quakegeneric/source/mathlib.h"
static void clamp_matches(float value){
 float reference=value,fast=value;
 if(reference<0.01)reference=0.01;
 if(fast<(float)0.01)fast=0.01;
 assert(!memcmp(&reference,&fast,sizeof fast));
}
static uint32_t next(uint32_t *s){*s=*s*1664525u+1013904223u;return *s;}
int main(void){
 uint32_t seed=1;
 const float clamp_boundaries[]={0.0f,-0.0f,INFINITY,-INFINITY,NAN,0x1p-149f,-0x1p-149f,0x1.fffffep127f,-0x1.fffffep127f};
 for(unsigned i=0;i<sizeof clamp_boundaries/sizeof *clamp_boundaries;i++)clamp_matches(clamp_boundaries[i]);
 float clip=0.01f;
 for(int i=0;i<64;i++){clamp_matches(clip);clip=nextafterf(clip,-INFINITY);}
 clip=0.01f;
 for(int i=0;i<64;i++){clamp_matches(clip);clip=nextafterf(clip,INFINITY);}
 const float boundaries[]={0.0f,-0.0f,0x1p-149f,-0x1p-149f,1.0f,-1.0f,1.00000012f,-1.00000012f,-0x1p31f,0x1.fffffep30f};
 for(unsigned i=0;i<sizeof boundaries/sizeof *boundaries;i++)assert(Q_CeilFloatToInt(boundaries[i])==(int)ceil((double)boundaries[i]));
 for(unsigned i=0;i<1000000;i++){
  uint32_t raw=next(&seed);float clamp_input;memcpy(&clamp_input,&raw,4);clamp_matches(clamp_input);
  int32_t n=(int32_t)next(&seed);int32_t d=(int32_t)(next(&seed)&0x7fffffff);if(!d)d=1;
  int32_t q=n/d,r=n%d;if(r<0){q--;r+=d;}
  double ref=floor((double)n/d);assert(q==(int32_t)ref&&r==(int32_t)((double)n-ref*d));
  uint32_t bits=next(&seed);bits=(bits&0x807fffffu)|((next(&seed)%127u)<<23);
  float z;memcpy(&z,&bits,4);assert(z>-1&&z<1);
  float edge=z*0x1p30f;assert((int)ceil((double)edge)==Q_CeilFloatToInt(edge));
  int old=(int)((double)z*0x8000*0x10000),fast=(int)(z*0x8000*0x10000);
  assert(old==fast);
  bits=next(&seed)&0x7f7fffffu;float positive;memcpy(&positive,&bits,4);
  float ref_sqrt=(float)sqrt((double)positive),fp32_sqrt=__builtin_sqrtf(positive);
  assert(!memcmp(&ref_sqrt,&fp32_sqrt,4));
 }
 puts("Quake render math: 1M signed floor divisions and finite normalized depth scalings / FP32 square roots / edge ceilings / near clamps match double reference PASS");
}
