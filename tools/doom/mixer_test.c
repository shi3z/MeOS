#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../apps/doom/mixer.h"
#include <m5os/audio.h>
static int16_t pcm[512*2];
static void header(uint8_t *p, unsigned len)
{ memcpy(p,"MUS\x1a",4); p[4]=len; p[5]=len>>8; p[6]=16; p[8]=1; }
static unsigned energy(void)
{ unsigned e=0; for (unsigned i=0;i<1024;i++) e+=(unsigned)abs(pcm[i]); return e; }
static void wav_header(FILE *f, unsigned samples)
{
    uint8_t h[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0};
    unsigned values[]={36+samples*4,AUDIO_RATE,AUDIO_RATE*4,samples*4};
    unsigned offsets[]={4,24,28,40};
    for(unsigned j=0;j<4;j++) for(unsigned b=0;b<4;b++) h[offsets[j]+b]=values[j]>>(b*8);
    h[32]=4;h[34]=16;memcpy(h+36,"data",4); fwrite(h,1,44,f);
}
static uint32_t le32(const uint8_t *p)
{ return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
int main(int argc,char **argv)
{
    doom_mix_reset(); uint8_t dmx[96];memset(dmx,255,sizeof dmx);
    dmx[0]=3;dmx[1]=0;dmx[2]=AUDIO_RATE&255;dmx[3]=AUDIO_RATE>>8;
    dmx[4]=88;dmx[5]=dmx[6]=dmx[7]=0;
    assert(doom_mix_sfx_start(0,dmx,sizeof dmx,127,0));
    doom_mix_render(pcm,1,NULL);assert(pcm[0]>0 && pcm[1]==0);
    doom_mix_sfx_params(0,127,254);doom_mix_render(pcm,1,NULL);assert(pcm[0]==0 && pcm[1]>0);
    doom_mix_render(pcm,512,NULL);assert(!doom_mix_sfx_playing(0));
    for(unsigned n=0;n<96;n++) assert(!doom_mix_sfx_start(0,dmx,n,127,127));
    assert(!doom_mix_sfx_start(8,dmx,96,127,127));
    uint8_t mus[32]={0}; const uint8_t score[]={0x40,0,30,0x90,0x80|60,100,0x81,12,0x80,60,14,0x60};
    header(mus,sizeof score);memcpy(mus+16,score,sizeof score);
    assert(doom_mix_music_register(mus,16+sizeof score));doom_mix_music_play(false);
    doom_mix_render(pcm,512,NULL);assert(energy()>0 && doom_mix_music_playing());
    doom_mix_music_pause(true);doom_mix_render(pcm,512,NULL);assert(!energy());
    doom_mix_music_pause(false);doom_mix_music_volume(0);doom_mix_render(pcm,512,NULL);assert(!energy());
    doom_mix_music_volume(100);
    for(unsigned i=0;i<100;i++) doom_mix_render(pcm,512,NULL);
    assert(!doom_mix_music_playing());
    /* Exhaustion and invalid events end safely; bounded zero-delay loops. */
    for(unsigned n=0;n<16+sizeof score;n++) assert(!doom_mix_music_register(mus,n));
    mus[16]=0xff;assert(doom_mix_music_register(mus,28));doom_mix_music_play(false);
    doom_mix_render(pcm,512,NULL);assert(!doom_mix_music_playing());
    uint32_t random=1;
    for(unsigned t=0;t<1000;t++) {
        for(unsigned i=16;i<32;i++) { random=random*1664525u+1013904223u;mus[i]=random>>24; }
        header(mus,16);assert(doom_mix_music_register(mus,32));doom_mix_music_play(t&1);
        doom_mix_render(pcm,512,NULL);
    }
    if(argc>1) {
        FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long len=ftell(f);rewind(f);
        uint8_t *wad=malloc(len);assert(wad && fread(wad,1,len,f)==(size_t)len);fclose(f);
        unsigned lumps=le32(wad+4), dir=le32(wad+8);bool found=false;
        for(unsigned i=0;i<lumps;i++) {
            uint8_t *e=wad+dir+i*16;
            if(!memcmp(e+8,"D_E1M1",6)) {
                doom_mix_reset();assert(doom_mix_music_register(wad+le32(e),le32(e+4)));
                doom_mix_music_play(true);found=true;break;
            }
        }
        assert(found);f=fopen("/tmp/meos-doom-music.wav","wb");assert(f);
        unsigned blocks=240;wav_header(f,blocks*512);unsigned nonzero=0;
        for(unsigned n=0;n<blocks;n++){doom_mix_render(pcm,512,NULL);nonzero+=energy()>0;fwrite(pcm,2,1024,f);}
        assert(nonzero>200 && doom_mix_music_playing());fclose(f);doom_mix_music_stop();free(wad);
        puts("E1M1 MUS: rendered >5 seconds to /tmp/meos-doom-music.wav PASS");
    }
    puts("Doom audio: DMX bounds, pan, release, MUS playback/pause/volume/end, malformed streams PASS");
}
